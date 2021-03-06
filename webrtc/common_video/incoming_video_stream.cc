/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/common_video/include/incoming_video_stream.h"

#include <assert.h>

#if defined(_WIN32)
#include <windows.h>
#elif defined(WEBRTC_LINUX)
#include <sys/time.h>
#include <time.h>
#else
#include <sys/time.h>
#endif

#include "webrtc/base/platform_thread.h"
#include "webrtc/common_video/libyuv/include/webrtc_libyuv.h"
#include "webrtc/common_video/video_render_frames.h"
#include "webrtc/system_wrappers/include/critical_section_wrapper.h"
#include "webrtc/system_wrappers/include/event_wrapper.h"
#include "webrtc/system_wrappers/include/tick_util.h"
#include "webrtc/system_wrappers/include/trace.h"

namespace webrtc {

IncomingVideoStream::IncomingVideoStream(uint32_t stream_id,
                                         bool disable_prerenderer_smoothing)
    : stream_id_(stream_id),
      disable_prerenderer_smoothing_(disable_prerenderer_smoothing),
      incoming_render_thread_(),
      deliver_buffer_event_(EventTimerWrapper::Create()),
      running_(false),
      external_callback_(nullptr),
      render_callback_(nullptr),
      render_buffers_(new VideoRenderFrames()),
      incoming_rate_(0),
      last_rate_calculation_time_ms_(0),
      num_frames_since_last_calculation_(0) {}

IncomingVideoStream::~IncomingVideoStream() {
  Stop();
}

VideoRenderCallback* IncomingVideoStream::ModuleCallback() {
  return this;
}

int32_t IncomingVideoStream::RenderFrame(const uint32_t stream_id,
                                         const VideoFrame& video_frame) {
  rtc::CritScope csS(&stream_critsect_);

  if (!running_) {
    return -1;
  }

  // Rate statistics.
  num_frames_since_last_calculation_++;
  int64_t now_ms = TickTime::MillisecondTimestamp();
  if (now_ms >= last_rate_calculation_time_ms_ + kFrameRatePeriodMs) {
    incoming_rate_ =
        static_cast<uint32_t>(1000 * num_frames_since_last_calculation_ /
                              (now_ms - last_rate_calculation_time_ms_));
    num_frames_since_last_calculation_ = 0;
    last_rate_calculation_time_ms_ = now_ms;
  }

  // Hand over or insert frame.
  if (disable_prerenderer_smoothing_) {
    DeliverFrame(video_frame);
  } else {
    rtc::CritScope csB(&buffer_critsect_);
    if (render_buffers_->AddFrame(video_frame) == 1) {
      deliver_buffer_event_->Set();
    }
  }
  return 0;
}

void IncomingVideoStream::SetRenderCallback(
    VideoRenderCallback* render_callback) {
  rtc::CritScope cs(&thread_critsect_);
  render_callback_ = render_callback;
}

int32_t IncomingVideoStream::SetExpectedRenderDelay(
    int32_t delay_ms) {
  rtc::CritScope csS(&stream_critsect_);
  if (running_) {
    return -1;
  }
  rtc::CritScope cs(&buffer_critsect_);
  return render_buffers_->SetRenderDelay(delay_ms);
}

void IncomingVideoStream::SetExternalCallback(
    VideoRenderCallback* external_callback) {
  rtc::CritScope cs(&thread_critsect_);
  external_callback_ = external_callback;
}

int32_t IncomingVideoStream::Start() {
  rtc::CritScope csS(&stream_critsect_);
  if (running_) {
    return 0;
  }

  if (!disable_prerenderer_smoothing_) {
    rtc::CritScope csT(&thread_critsect_);
    assert(incoming_render_thread_ == NULL);

    incoming_render_thread_.reset(new rtc::PlatformThread(
        IncomingVideoStreamThreadFun, this, "IncomingVideoStreamThread"));
    if (!incoming_render_thread_) {
      return -1;
    }

    incoming_render_thread_->Start();
    incoming_render_thread_->SetPriority(rtc::kRealtimePriority);
    deliver_buffer_event_->StartTimer(false, kEventStartupTimeMs);
  }

  running_ = true;
  return 0;
}

int32_t IncomingVideoStream::Stop() {
  rtc::CritScope cs_stream(&stream_critsect_);

  if (!running_) {
    return 0;
  }

  rtc::PlatformThread* thread = NULL;
  {
    rtc::CritScope cs_thread(&thread_critsect_);
    if (incoming_render_thread_) {
      // Setting the incoming render thread to NULL marks that we're performing
      // a shutdown and will make IncomingVideoStreamProcess abort after wakeup.
      thread = incoming_render_thread_.release();
      deliver_buffer_event_->StopTimer();
      // Set the event to allow the thread to wake up and shut down without
      // waiting for a timeout.
      deliver_buffer_event_->Set();
    }
  }
  if (thread) {
    thread->Stop();
    delete thread;
  }
  running_ = false;
  return 0;
}

int32_t IncomingVideoStream::Reset() {
  rtc::CritScope cs_buffer(&buffer_critsect_);
  render_buffers_->ReleaseAllFrames();
  return 0;
}

uint32_t IncomingVideoStream::StreamId() const {
  return stream_id_;
}

uint32_t IncomingVideoStream::IncomingRate() const {
  rtc::CritScope cs(&stream_critsect_);
  return incoming_rate_;
}

bool IncomingVideoStream::IncomingVideoStreamThreadFun(void* obj) {
  return static_cast<IncomingVideoStream*>(obj)->IncomingVideoStreamProcess();
}

bool IncomingVideoStream::IncomingVideoStreamProcess() {
  if (kEventError != deliver_buffer_event_->Wait(kEventMaxWaitTimeMs)) {
    rtc::CritScope cs(&thread_critsect_);
    if (incoming_render_thread_ == NULL) {
      // Terminating
      return false;
    }

    // Get a new frame to render and the time for the frame after this one.
    VideoFrame frame_to_render;
    uint32_t wait_time;
    {
      rtc::CritScope cs(&buffer_critsect_);
      frame_to_render = render_buffers_->FrameToRender();
      wait_time = render_buffers_->TimeToNextFrameRelease();
    }

    // Set timer for next frame to render.
    if (wait_time > kEventMaxWaitTimeMs) {
      wait_time = kEventMaxWaitTimeMs;
    }
    deliver_buffer_event_->StartTimer(false, wait_time);

    DeliverFrame(frame_to_render);
  }
  return true;
}

void IncomingVideoStream::DeliverFrame(const VideoFrame& video_frame) {
  rtc::CritScope cs(&thread_critsect_);
  if (video_frame.IsZeroSize()) {
    // No frame.
    return;
  }

  // Send frame for rendering.
  if (external_callback_) {
    external_callback_->RenderFrame(stream_id_, video_frame);
  } else if (render_callback_) {
    render_callback_->RenderFrame(stream_id_, video_frame);
  }
}

}  // namespace webrtc
