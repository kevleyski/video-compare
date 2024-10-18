#include "video_compare.h"
#include <algorithm>
#include <chrono>
#include <deque>
#include <iostream>
#include <thread>
#include "ffmpeg.h"
#include "sorted_flat_deque.h"
#include "string_utils.h"
extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/time.h>
}

const size_t VideoCompare::QUEUE_SIZE{5};

static inline bool is_behind(int64_t frame1_pts, int64_t frame2_pts, int64_t delta_pts) {
  const float t1 = static_cast<float>(frame1_pts) * AV_TIME_TO_SEC;
  const float t2 = static_cast<float>(frame2_pts) * AV_TIME_TO_SEC;
  const float delta_s = static_cast<float>(delta_pts) * AV_TIME_TO_SEC - 1e-5F;

  const float diff = t1 - t2;
  const float tolerance = std::max(delta_s, 1.0F / 480.0F);

  return diff < -tolerance;
}

static inline int64_t compute_min_delta(const int64_t delta_left_pts, const int64_t delta_right_pts) {
  return std::min(delta_left_pts, delta_right_pts) * 8 / 10;
};

static inline bool is_in_sync(const int64_t left_pts, const int64_t right_pts, const int64_t delta_left_pts, const int64_t delta_right_pts) {
  const int64_t min_delta = compute_min_delta(delta_left_pts, delta_right_pts);

  return !is_behind(left_pts, right_pts, min_delta) && !is_behind(right_pts, left_pts, min_delta);
};

static inline int64_t compute_frame_delay(const int64_t left_pts, const int64_t right_pts) {
  return std::max(left_pts, right_pts);
}

VideoCompare::VideoCompare(const VideoCompareConfig& config)
    : auto_loop_mode_(config.auto_loop_mode),
      frame_buffer_size_(config.frame_buffer_size),
      time_shift_ms_(config.time_shift_ms),
      demuxer_{std::make_unique<Demuxer>(config.left.demuxer, config.left.file_name, config.left.demuxer_options, config.left.decoder_options),
               std::make_unique<Demuxer>(config.right.demuxer, config.right.file_name, config.right.demuxer_options, config.right.decoder_options)},
      video_decoder_{
          std::make_unique<VideoDecoder>(config.left.decoder, config.left.hw_accel_spec, demuxer_[LEFT]->video_codec_parameters(), config.left.peak_luminance_nits, config.left.hw_accel_options, config.left.decoder_options),
          std::make_unique<VideoDecoder>(config.right.decoder, config.right.hw_accel_spec, demuxer_[RIGHT]->video_codec_parameters(), config.right.peak_luminance_nits, config.right.hw_accel_options, config.right.decoder_options)},
      video_filterer_{std::make_unique<VideoFilterer>(demuxer_[LEFT].get(),
                                                      video_decoder_[LEFT].get(),
                                                      config.left.peak_luminance_nits,
                                                      config.left.video_filters,
                                                      demuxer_[RIGHT].get(),
                                                      video_decoder_[RIGHT].get(),
                                                      config.right.peak_luminance_nits,
                                                      config.tone_mapping_mode,
                                                      config.boost_tone,
                                                      config.disable_auto_filters),
                      std::make_unique<VideoFilterer>(demuxer_[RIGHT].get(),
                                                      video_decoder_[RIGHT].get(),
                                                      config.right.peak_luminance_nits,
                                                      config.right.video_filters,
                                                      demuxer_[LEFT].get(),
                                                      video_decoder_[LEFT].get(),
                                                      config.left.peak_luminance_nits,
                                                      config.tone_mapping_mode,
                                                      config.boost_tone,
                                                      config.disable_auto_filters)},
      max_width_{std::max(video_filterer_[LEFT]->dest_width(), video_filterer_[RIGHT]->dest_width())},
      max_height_{std::max(video_filterer_[LEFT]->dest_height(), video_filterer_[RIGHT]->dest_height())},
      shortest_duration_{std::min(demuxer_[LEFT]->duration(), demuxer_[RIGHT]->duration()) * AV_TIME_TO_SEC},
      format_converter_{std::make_unique<FormatConverter>(video_filterer_[LEFT]->dest_width(),
                                                          video_filterer_[LEFT]->dest_height(),
                                                          max_width_,
                                                          max_height_,
                                                          video_filterer_[LEFT]->dest_pixel_format(),
                                                          config.use_10_bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24,
                                                          video_decoder_[LEFT]->color_space(),
                                                          video_decoder_[LEFT]->color_range()),
                        std::make_unique<FormatConverter>(video_filterer_[RIGHT]->dest_width(),
                                                          video_filterer_[RIGHT]->dest_height(),
                                                          max_width_,
                                                          max_height_,
                                                          video_filterer_[RIGHT]->dest_pixel_format(),
                                                          config.use_10_bpc ? AV_PIX_FMT_RGB48LE : AV_PIX_FMT_RGB24,
                                                          video_decoder_[RIGHT]->color_space(),
                                                          video_decoder_[RIGHT]->color_range())},
      display_{std::make_unique<Display>(config.display_number,
                                         config.display_mode,
                                         config.verbose,
                                         config.fit_window_to_usable_bounds,
                                         config.high_dpi_allowed,
                                         config.use_10_bpc,
                                         config.window_size,
                                         max_width_,
                                         max_height_,
                                         shortest_duration_,
                                         config.wheel_sensitivity,
                                         config.left.file_name,
                                         config.right.file_name)},
      timer_{std::make_unique<Timer>()},
      packet_queue_{std::make_unique<PacketQueue>(QUEUE_SIZE), std::make_unique<PacketQueue>(QUEUE_SIZE)},
      frame_queue_{std::make_unique<FrameQueue>(QUEUE_SIZE), std::make_unique<FrameQueue>(QUEUE_SIZE)} {
  auto dump_video_info = [&](const std::string& label, const Side side, const std::string& file_name) {
    const std::string dimensions = string_sprintf("%dx%d", video_decoder_[side]->width(), video_decoder_[side]->height());
    const std::string pixel_format_and_color_space =
        stringify_pixel_format(video_decoder_[side]->pixel_format(), video_decoder_[side]->color_range(), video_decoder_[side]->color_space(), video_decoder_[side]->color_primaries(), video_decoder_[side]->color_trc());

    std::cout << string_sprintf("%s %9s, %s, %s, %s, %s, %s, %s, %s, %s, %s", label.c_str(), dimensions.c_str(), format_duration(demuxer_[side]->duration() * AV_TIME_TO_SEC).c_str(),
                                stringify_frame_rate(demuxer_[side]->guess_frame_rate(), video_decoder_[side]->codec_context()->field_order).c_str(), stringify_decoder(video_decoder_[side].get()).c_str(),
                                pixel_format_and_color_space.c_str(), demuxer_[side]->format_name().c_str(), file_name.c_str(), stringify_file_size(demuxer_[side]->file_size(), 2).c_str(),
                                stringify_bit_rate(demuxer_[side]->bit_rate(), 1).c_str(), video_filterer_[side]->filter_description().c_str())
              << std::endl;
  };

  dump_video_info("Left video: ", LEFT, config.left.file_name.c_str());
  dump_video_info("Right video:", RIGHT, config.right.file_name.c_str());
}

void VideoCompare::operator()() {
  stages_.emplace_back(&VideoCompare::thread_demultiplex_left, this);
  stages_.emplace_back(&VideoCompare::thread_demultiplex_right, this);
  stages_.emplace_back(&VideoCompare::thread_decode_video_left, this);
  stages_.emplace_back(&VideoCompare::thread_decode_video_right, this);

  compare();

  for (auto& stage : stages_) {
    stage.join();
  }

  exception_holder_.rethrow_stored_exception();
}

void VideoCompare::thread_demultiplex_left() {
  demultiplex(LEFT);
}

void VideoCompare::thread_demultiplex_right() {
  demultiplex(RIGHT);
}

void VideoCompare::sleep_for_ms(const int ms) {
  std::chrono::milliseconds sleep(ms);
  std::this_thread::sleep_for(sleep);
}

void VideoCompare::demultiplex(const Side side) {
  try {
    while (keep_running()) {
      // Wait for decoder to drain
      if (seeking_ && ready_to_seek_.get(ReadyToSeek::DECODER, side)) {
        ready_to_seek_.set(ReadyToSeek::DEMULTIPLEXER, side);

        sleep_for_ms(10);
        continue;
      }
      // Sleep if we are finished for now
      if (packet_queue_[side]->is_stopped()) {
        sleep_for_ms(10);
        continue;
      }

      // Create AVPacket
      std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet{new AVPacket, [](AVPacket* p) {
                                                                         av_packet_unref(p);
                                                                         delete p;
                                                                       }};
      av_init_packet(packet.get());
      packet->data = nullptr;

      // Read frame into AVPacket
      if (!(*demuxer_[side])(*packet)) {
        // Enter wait state if EOF
        packet_queue_[side]->stop();
        continue;
      }

      // Move into queue if first video stream
      if (packet->stream_index == demuxer_[side]->video_stream_index()) {
        if (!packet_queue_[side]->push(std::move(packet))) {
          break;
        }
      }
    }
  } catch (...) {
    exception_holder_.rethrow_stored_exception();
    frame_queue_[side]->quit();
    packet_queue_[side]->quit();
  }
}

void VideoCompare::thread_decode_video_left() {
  decode_video(LEFT);
}

void VideoCompare::thread_decode_video_right() {
  decode_video(RIGHT);
}

void VideoCompare::decode_video(const Side side) {
  try {
    while (keep_running()) {
      auto flush_and_signal = [&]() {
        video_decoder_[side]->flush();

        ready_to_seek_.set(ReadyToSeek::DECODER, side);
      };

      // Sleep if we are finished for now
      if (frame_queue_[side]->is_stopped()) {
        if (seeking_) {
          flush_and_signal();
        }

        sleep_for_ms(10);
        continue;
      }

      // Create AVFrames and AVPacket
      std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_decoded{av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); }};
      std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> sw_frame_decoded{av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); }};
      std::unique_ptr<AVPacket, std::function<void(AVPacket*)>> packet{nullptr, [](AVPacket* p) {
                                                                         av_packet_unref(p);
                                                                         delete p;
                                                                       }};

      // Read packet from queue
      if (!packet_queue_[side]->pop(packet)) {
        // Flush remaining frames cached in the decoder
        while (process_packet(side, packet.get(), frame_decoded.get(), sw_frame_decoded.get())) {
          ;
        }

        // Close the filter source
        video_filterer_[side]->close_src();

        // Flush the filter graph
        filter_decoded_frame(side, nullptr);

        // Enter wait state
        frame_queue_[side]->stop();
        continue;
      }

      if (seeking_) {
        flush_and_signal();

        sleep_for_ms(10);
        continue;
      }

      // If the packet didn't send, receive more frames and try again
      while (!process_packet(side, packet.get(), frame_decoded.get(), sw_frame_decoded.get()) && !seeking_) {
        ;
      }
    }
  } catch (...) {
    exception_holder_.rethrow_stored_exception();
    frame_queue_[side]->quit();
    packet_queue_[side]->quit();
  }
}

bool VideoCompare::process_packet(const Side side, AVPacket* packet, AVFrame* frame_decoded, AVFrame* sw_frame_decoded) {
  bool sent = video_decoder_[side]->send(packet);

  // If a whole frame has been decoded, adjust time stamps and add to queue
  while (video_decoder_[side]->receive(frame_decoded, demuxer_[side].get())) {
    AVFrame* frame_for_filtering;

    if (frame_decoded->format == video_decoder_[side]->hw_pixel_format()) {
      // Transfer data from GPU to CPU
      if (av_hwframe_transfer_data(sw_frame_decoded, frame_decoded, 0) < 0) {
        throw std::runtime_error("Error transferring frame from GPU to CPU");
      }
      if (av_frame_copy_props(sw_frame_decoded, frame_decoded) < 0) {
        throw std::runtime_error("Copying SW frame properties");
      }

      frame_for_filtering = sw_frame_decoded;
    } else {
      frame_for_filtering = frame_decoded;
    }

    if (!filter_decoded_frame(side, frame_for_filtering)) {
      return sent;
    }
  }

  return sent;
}

bool VideoCompare::filter_decoded_frame(const Side side, AVFrame* frame_decoded) {
  // send decoded frame to filterer
  if (!video_filterer_[side]->send(frame_decoded)) {
    throw std::runtime_error("Error while feeding the filter graph");
  }

  std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_filtered{av_frame_alloc(), [](AVFrame* f) { av_frame_free(&f); }};

  while (true) {
    // get next filtered frame
    if (!video_filterer_[side]->receive(frame_filtered.get())) {
      break;
    }

    // scale and convert pixel format before pushing to frame queue for displaying
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_converted{av_frame_alloc(), [](AVFrame* f) {
                                                                              av_freep(&f->data[0]);
                                                                              av_frame_free(&f);
                                                                            }};

    if (av_frame_copy_props(frame_converted.get(), frame_filtered.get()) < 0) {
      throw std::runtime_error("Copying filtered frame properties");
    }
    if (av_image_alloc(frame_converted->data, frame_converted->linesize, format_converter_[side]->dest_width(), format_converter_[side]->dest_height(), format_converter_[side]->dest_pixel_format(), 64) < 0) {
      throw std::runtime_error("Allocating converted picture");
    }
    (*format_converter_[side])(frame_filtered.get(), frame_converted.get());

    if (!frame_queue_[side]->push(std::move(frame_converted))) {
      return false;
    }
  }

  return true;
}

bool VideoCompare::keep_running() {
  return !display_->get_quit() && !exception_holder_.has_exception();
}

void VideoCompare::compare() {
  try {
#ifdef _DEBUG
    std::string previous_state;
#endif

    std::deque<std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>> left_frames;
    std::deque<std::unique_ptr<AVFrame, std::function<void(AVFrame*)>>> right_frames;
    int frame_offset = 0;

    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_left{nullptr, [](AVFrame* f) { av_frame_free(&f); }};
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> frame_right{nullptr, [](AVFrame* f) { av_frame_free(&f); }};

    int64_t left_pts = 0;
    int64_t left_decoded_picture_number = 0;
    int64_t left_previous_decoded_picture_number = -1;
    int64_t delta_left_pts = 0;
    int64_t left_first_pts = 0;
    const float left_start_time = demuxer_[LEFT]->start_time() * AV_TIME_TO_SEC;

    if (left_start_time > 0) {
      std::cout << "Note: The left video has a start time of " << format_position(left_start_time, true) << " - timestamps will be shifted so they start at zero!" << std::endl;
    }

    int64_t right_pts = 0;
    int64_t right_decoded_picture_number = 0;
    int64_t right_previous_decoded_picture_number = -1;
    int64_t delta_right_pts = 0;
    int64_t right_first_pts = 0;
    const float right_start_time = demuxer_[RIGHT]->start_time() * AV_TIME_TO_SEC;

    if (right_start_time > 0) {
      std::cout << "Note: The right video has a start time of " << format_position(right_start_time, true) << " - timestamps will be shifted so they start at zero!" << std::endl;
    }

    sorted_flat_deque<int32_t> left_deque(8);
    sorted_flat_deque<int32_t> right_deque(8);

    Timer refresh_timer;
    sorted_flat_deque<int32_t> refresh_time_deque(8);

    int64_t right_time_shift = time_shift_ms_ * MILLISEC_TO_AV_TIME;
    int total_right_time_shifted = 0;

    int forward_navigate_frames = 0;

    bool auto_loop_triggered = false;

    for (uint64_t frame_number = 0;; ++frame_number) {
      std::string message;

      display_->input();

      if (!keep_running()) {
        break;
      }

      if (display_->get_tick_playback()) {
        timer_->reset();
      }

      forward_navigate_frames += display_->get_frame_navigation_delta();

      bool skip_update = false;

      if ((display_->get_seek_relative() != 0.0F) || (display_->get_shift_right_frames() != 0)) {
        total_right_time_shifted += display_->get_shift_right_frames();

        // compute effective time shift
        right_time_shift = time_shift_ms_ * MILLISEC_TO_AV_TIME + total_right_time_shifted * (delta_right_pts > 0 ? delta_right_pts : 10000);

        ready_to_seek_.reset();
        seeking_ = true;

        // drain packet and frame queues
        frame_queue_[LEFT]->empty();
        frame_queue_[RIGHT]->empty();

        while (!ready_to_seek_.all_are_empty()) {
          sleep_for_ms(10);
        }

        auto drain_queues = [&](const Side side) {
          packet_queue_[side]->stop();
          frame_queue_[side]->stop();

          packet_queue_[side]->empty();
          frame_queue_[side]->empty();
        };

        drain_queues(LEFT);
        drain_queues(RIGHT);

        // reinit filter graphs
        video_filterer_[LEFT]->reinit();
        video_filterer_[RIGHT]->reinit();

        float next_left_position, next_right_position;

        const float left_position = left_pts * AV_TIME_TO_SEC + left_start_time;
        const float right_position = left_pts * AV_TIME_TO_SEC + right_start_time;

        if (display_->get_seek_from_start()) {
          // seek from start based on the shortest stream duration in seconds
          next_left_position = shortest_duration_ * display_->get_seek_relative() + left_start_time;
          next_right_position = shortest_duration_ * display_->get_seek_relative() + right_start_time;
        } else {
          next_left_position = left_position + display_->get_seek_relative();
          next_right_position = right_position + display_->get_seek_relative();

          if (right_time_shift < 0) {
            next_right_position += (right_time_shift + delta_right_pts) * AV_TIME_TO_SEC;
          }
        }

        bool backward = (display_->get_seek_relative() < 0.0F) || (display_->get_shift_right_frames() != 0);

#ifdef _DEBUG
        std::cout << "SEEK: next_left_position=" << (int)(next_left_position * 1000) << ", next_right_position=" << (int)(next_right_position * 1000) << ", backward=" << backward << std::endl;
#endif
        if ((!demuxer_[LEFT]->seek(next_left_position, backward) && !backward) || (!demuxer_[RIGHT]->seek(next_right_position, backward) && !backward)) {
          // restore position if unable to perform forward seek
          message = "Unable to seek past end of file";

          demuxer_[LEFT]->seek(left_position, true);
          demuxer_[RIGHT]->seek(right_position, true);
        };

        seeking_ = false;

        // allow packet and frame queues to receive data again
        auto reset_queues = [&](const Side side) {
          packet_queue_[side]->restart();
          frame_queue_[side]->restart();
        };

        reset_queues(LEFT);
        reset_queues(RIGHT);

        frame_queue_[LEFT]->pop(frame_left);

        if (frame_left != nullptr) {
          left_pts = frame_left->pts;
          left_previous_decoded_picture_number = -1;
          left_decoded_picture_number = 1;

          left_frames.clear();
        }

        // round away from zero to nearest 2 ms
        if (right_time_shift > 0) {
          right_time_shift = ((right_time_shift / 1000) + 2) * 1000;
        } else if (right_time_shift < 0) {
          right_time_shift = ((right_time_shift / 1000) - 2) * 1000;
        }

        frame_queue_[RIGHT]->pop(frame_right);

        if (frame_right != nullptr) {
          right_pts = frame_right->pts - right_time_shift;
          right_previous_decoded_picture_number = -1;
          right_decoded_picture_number = 1;

          right_frames.clear();
        }

        // don't sync until the next iteration to prevent freezing when comparing an image
        skip_update = true;
      }

      bool store_frames = false;
      bool adjusting = false;

      // keep showing currently displayed frame for another iteration?
      skip_update = skip_update || (timer_->us_until_target() - refresh_time_deque.average()) > 0;
      const bool fetch_next_frame = display_->get_play() || (forward_navigate_frames > 0);

      // use the delta between current and previous PTS as the tolerance which determines whether we have to adjust
      const int64_t min_delta = compute_min_delta(delta_left_pts, delta_right_pts);

#ifdef _DEBUG
      const std::string current_state = string_sprintf("left_pts=%5d, left_is_behind=%d, right_pts=%5d, right_is_behind=%d, min_delta=%5d, right_time_shift=%5d", left_pts / 1000, is_behind(left_pts, right_pts, min_delta),
                                                       (right_pts + right_time_shift) / 1000, is_behind(right_pts, left_pts, min_delta), min_delta / 1000, right_time_shift / 1000);

      if (current_state != previous_state) {
        std::cout << current_state << std::endl;
      }

      previous_state = current_state;
#endif

      if (is_behind(left_pts, right_pts, min_delta)) {
        adjusting = true;

        if (frame_queue_[LEFT]->pop(frame_left)) {
          left_decoded_picture_number++;
        }
      }
      if (is_behind(right_pts, left_pts, min_delta)) {
        adjusting = true;

        if (frame_queue_[RIGHT]->pop(frame_right)) {
          right_decoded_picture_number++;
        }
      }

      // handle regular playback only
      if (!skip_update && display_->get_buffer_play_loop_mode() == Display::Loop::off) {
        if (!adjusting && fetch_next_frame) {
          if (!frame_queue_[LEFT]->pop(frame_left) || !frame_queue_[RIGHT]->pop(frame_right)) {
            frame_left = nullptr;
            frame_right = nullptr;

            timer_->update();
          } else {
            left_decoded_picture_number++;
            right_decoded_picture_number++;

            store_frames = true;

            // update timer for regular playback
            if (frame_number > 0) {
              const int64_t play_frame_delay = compute_frame_delay(frame_left->pts - left_pts, frame_right->pts - right_pts - right_time_shift);

              timer_->shift_target(play_frame_delay / display_->get_playback_speed_factor());
            } else {
              left_first_pts = frame_left->pts;
              right_first_pts = frame_right->pts;

              timer_->update();
            }
          }
        } else {
          timer_->reset();
        }
      }

      // for frame-accurate forward navigation, decrement counter when frame is stored in buffer
      if (store_frames && (forward_navigate_frames > 0)) {
        forward_navigate_frames--;
      }

      // 1. Update the duration of the last frame to its exact value once the next frame has been decoded
      // 2. Compute the average PTS delta in a rolling-window fashion
      // 3. Assume the duration of the current frame is approximately the value of the previous step
      if (frame_left != nullptr) {
        if ((left_decoded_picture_number - left_previous_decoded_picture_number) == 1) {
          const int64_t last_duration = frame_left->pts - left_pts;

          left_deque.push_back(last_duration);
          delta_left_pts = left_deque.average();
        }
        if (delta_left_pts > 0) {
          ffmpeg::frame_duration(frame_left.get()) = delta_left_pts;

          if (!left_frames.empty() && left_frames.back()->pts == left_first_pts) {
            // update the duration of the first stored left frame
            ffmpeg::frame_duration(left_frames.back().get()) = delta_left_pts;
          }
        } else {
          delta_left_pts = ffmpeg::frame_duration(frame_left.get());
        }

        left_pts = frame_left->pts;
        left_previous_decoded_picture_number = left_decoded_picture_number;
      }
      if (frame_right != nullptr) {
        const int64_t new_right_pts = frame_right->pts - right_time_shift;

        if ((right_decoded_picture_number - right_previous_decoded_picture_number) == 1) {
          const int64_t last_duration = new_right_pts - right_pts;

          right_deque.push_back(last_duration);
          delta_right_pts = right_deque.average();
        }
        if (delta_right_pts > 0) {
          ffmpeg::frame_duration(frame_right.get()) = delta_right_pts;

          if (!right_frames.empty() && right_frames.back()->pts == right_first_pts) {
            // update the duration of the first stored right frame
            ffmpeg::frame_duration(right_frames.back().get()) = delta_right_pts;
          }
        } else {
          delta_right_pts = ffmpeg::frame_duration(frame_right.get());
        }

        right_pts = new_right_pts;
        right_previous_decoded_picture_number = right_decoded_picture_number;
      }

      if (store_frames) {
        if (left_frames.size() >= frame_buffer_size_) {
          left_frames.pop_back();
        }
        if (right_frames.size() >= frame_buffer_size_) {
          right_frames.pop_back();
        }

        left_frames.push_front(std::move(frame_left));
        right_frames.push_front(std::move(frame_right));
      } else {
        if (frame_left != nullptr) {
          if (!left_frames.empty()) {
            left_frames.front() = std::move(frame_left);
          } else {
            left_frames.push_front(std::move(frame_left));
          }
        }
        if (frame_right != nullptr) {
          if (!right_frames.empty()) {
            right_frames.front() = std::move(frame_right);
          } else {
            right_frames.push_front(std::move(frame_right));
          }
        }
      }

      const bool no_activity = !skip_update && !adjusting && !store_frames;
      const bool end_of_file = no_activity && (frame_queue_[LEFT]->is_stopped() || frame_queue_[RIGHT]->is_stopped());
      const bool buffer_is_full = left_frames.size() == frame_buffer_size_ && right_frames.size() == frame_buffer_size_;

      const int max_left_frame_index = static_cast<int>(left_frames.size()) - 1;

      auto adjust_frame_offset = [max_left_frame_index](const int frame_offset, const int adjustment) { return std::min(std::max(0, frame_offset + adjustment), max_left_frame_index); };

      frame_offset = adjust_frame_offset(frame_offset, display_->get_frame_buffer_offset_delta());

      if (frame_offset >= 0 && !left_frames.empty() && !right_frames.empty()) {
        const bool is_playback_in_sync = is_in_sync(left_pts, right_pts, delta_left_pts, delta_right_pts);

        // reduce refresh rate to 10 Hz for faster re-syncing
        const bool skip_refresh = !is_playback_in_sync && refresh_timer.us_until_target() > -100000;

        if (!skip_refresh) {
          std::string prefix_str, suffix_str;

          // add [] to the current / total browsable string when in sync
          if (fetch_next_frame && is_playback_in_sync) {
            prefix_str = "[";
            suffix_str = "]";
          }

          const int max_digits = std::log10(frame_buffer_size_) + 1;
          const std::string frame_offset_format_str = string_sprintf("%%s%%0%dd/%%0%dd%%s", max_digits, max_digits);
          const std::string current_total_browsable = string_sprintf(frame_offset_format_str.c_str(), prefix_str.c_str(), frame_offset + 1, max_left_frame_index + 1, suffix_str.c_str());

          // refresh display
          refresh_timer.update();

          const auto& left_frames_ref = !display_->get_swap_left_right() ? left_frames : right_frames;
          const auto& right_frames_ref = !display_->get_swap_left_right() ? right_frames : left_frames;

          display_->refresh(left_frames_ref[frame_offset].get(), right_frames_ref[frame_offset].get(), current_total_browsable, message);

          refresh_time_deque.push_back(-refresh_timer.us_until_target());

          // check if sleeping is the best option for accurate playback by taking the average refresh time into account
          const int64_t time_until_final_refresh = timer_->us_until_target();

          if (time_until_final_refresh > 0 && time_until_final_refresh < refresh_time_deque.average()) {
            timer_->wait(time_until_final_refresh);
          } else if (time_until_final_refresh <= 0 && display_->get_buffer_play_loop_mode() != Display::Loop::off) {
            // auto-adjust current frame during in-buffer playback
            switch (display_->get_buffer_play_loop_mode()) {
              case Display::Loop::forwardonly:
                if (frame_offset == 0) {
                  frame_offset = max_left_frame_index;
                } else {
                  frame_offset = adjust_frame_offset(frame_offset, -1);
                }
                break;
              case Display::Loop::pingpong:
                if (max_left_frame_index >= 1 && (frame_offset == 0 || frame_offset == max_left_frame_index)) {
                  display_->toggle_buffer_play_direction();
                }
                frame_offset = adjust_frame_offset(frame_offset, display_->get_buffer_play_forward() ? -1 : 1);
                break;
              default:
                break;
            }

            // update timer for accurate in-buffer playback
            const int64_t in_buffer_frame_delay = compute_frame_delay(ffmpeg::frame_duration(left_frames[frame_offset].get()), ffmpeg::frame_duration(right_frames[frame_offset].get()));

            timer_->shift_target(in_buffer_frame_delay / display_->get_playback_speed_factor());
          }

          // enter in-buffer playback once if buffer is full or EOF reached
          if (auto_loop_mode_ != Display::Loop::off && !auto_loop_triggered && (buffer_is_full || end_of_file)) {
            display_->set_buffer_play_loop_mode(auto_loop_mode_);

            auto_loop_triggered = true;
          }
        }
      }
    }
  } catch (...) {
    exception_holder_.store_current_exception();
  }

  frame_queue_[LEFT]->quit();
  packet_queue_[LEFT]->quit();
  frame_queue_[RIGHT]->quit();
  packet_queue_[RIGHT]->quit();
}
