/*
 * Copyright (C) 2016 Jolla Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Authored by: Mohammed Hassan <mohammed.hassan@jolla.com>
 */

#include <utils/Condition.h>
#include <media/stagefright/CameraSource.h>
#include <media/stagefright/MetaData.h>
#include "droidmediarecorder.h"
#include "private.h"
#if (ANDROID_MAJOR == 4 && ANDROID_MINOR < 4)
#include <gui/Surface.h>
#endif
#if ANDROID_MAJOR >=5
#include <media/stagefright/foundation/ALooper.h>
#endif

namespace android {
  class CameraSourceListener {
  public:
    static int32_t getColorFormat(android::sp<android::CameraSource> src, android::CameraParameters p) {
      return src->isCameraColorFormatSupported (p) == android::NO_ERROR ? src->mColorFormat : -1;
    }
  };
};

struct _DroidMediaRecorder {
  _DroidMediaRecorder() :
#if ANDROID_MAJOR < 5
    m_running(false),
#endif
  m_cb_data(0)

  {
    memset(&m_cb, 0x0, sizeof(m_cb));
  }

  android::status_t tick() {
    android::MediaBuffer *buffer;
    android::status_t err = m_codec->read(&buffer);

    if (buffer) {
      DroidMediaCodecData data;
      data.data.data = (uint8_t *)buffer->data() + buffer->range_offset();
      data.data.size = buffer->range_length();
      data.ts = 0;
      data.decoding_ts = 0;

      if (!buffer->meta_data()->findInt64(android::kKeyTime, &data.ts)) {
	// I really don't know what to do here and I doubt we will reach that anyway.
	ALOGE("DroidMediaRecorder: Received a buffer without a timestamp!");
      } else {
	// Convert timestamp from useconds to nseconds
	data.ts *= 1000;
      }

      buffer->meta_data()->findInt64(android::kKeyDecodingTime, &data.decoding_ts);
      if (data.decoding_ts) {
	// Convert from usec to nsec.
	data.decoding_ts *= 1000;
      }

      int32_t sync = 0;
      data.sync = false;
      buffer->meta_data()->findInt32(android::kKeyIsSyncFrame, &sync);
      if (sync) {
	data.sync = true;
      }

      int32_t codecConfig = 0;
      data.codec_config = false;
      if (buffer->meta_data()->findInt32(android::kKeyIsCodecConfig, &codecConfig)
	  && codecConfig) {
	data.codec_config = true;
      }

      m_cb.data_available (m_cb_data, &data);
	    
      buffer->release();
    }
    
    return err;
  }

  void *run() {
    android::status_t err = android::OK;
    while (m_running && err == android::OK) {
      err = tick();
    }
    return NULL;
  }

  static void *ThreadWrapper(void *that) {
    return static_cast<DroidMediaRecorder *>(that)->run();
  }

  DroidMediaCamera *m_cam;
  DroidMediaCodecDataCallbacks m_cb;
  void *m_cb_data;
  android::sp<android::CameraSource> m_src;
  android::sp<android::MediaSource> m_codec;
#if ANDROID_MAJOR >= 5
  android::sp<android::ALooper> m_looper = NULL;
#endif
  pthread_t m_thread;
  bool m_running;
};

extern "C" {
DroidMediaRecorder *droid_media_recorder_create(DroidMediaCamera *camera, DroidMediaCodecEncoderMetaData *meta) {

  android::Size size(meta->parent.width, meta->parent.height);
  DroidMediaRecorder *recorder = new DroidMediaRecorder;
  recorder->m_cam = camera;
  android::sp<android::Camera> cam(droid_media_camera_get_camera (camera));
  recorder->m_src = android::CameraSource::CreateFromCamera(cam->remote(),
							    cam->getRecordingProxy(), // proxy
							    -1, // cameraId
#if (ANDROID_MAJOR == 4 && ANDROID_MINOR == 4) || ANDROID_MAJOR >= 5
							    android::String16("droidmedia"), // clientName
								android::Camera::USE_CALLING_UID, // clientUid
#if ANDROID_MAJOR >= 7
								android::Camera::USE_CALLING_PID, //clientPid
#endif
#endif
							    size,  // videoSize
							    meta->parent.fps, // frameRate
							    NULL, // surface
								meta->meta_data // storeMetaDataInVideoBuffers
							    );
  // fetch the colour format
  recorder->m_src->getFormat()->findInt32(android::kKeyColorFormat, &meta->color_format);
  
  // Now the encoder:
#if ANDROID_MAJOR >= 5
  recorder->m_codec = (droid_media_codec_create_encoder_raw(meta, recorder->m_looper, recorder->m_src));
#else
  recorder->m_codec = (droid_media_codec_create_encoder_raw(meta, recorder->m_src));
#endif

  return recorder;

}

void droid_media_recorder_destroy(DroidMediaRecorder *recorder) {
  recorder->m_codec.clear();
  recorder->m_src.clear();

  delete recorder;
}

bool droid_media_recorder_start(DroidMediaRecorder *recorder) {

	recorder->m_running = true;

	int err = recorder->m_codec->start();

  if (err != android::OK) {
    ALOGE("DroidMediaRecorder: error 0x%x starting codec", -err);
    return false;
  }

  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
  pthread_create(&recorder->m_thread, &attr, DroidMediaRecorder::ThreadWrapper, recorder);
  pthread_attr_destroy(&attr);
#if ANDROID_MAJOR >= 5
  recorder->m_looper->start();
#endif

  return true;
}

void droid_media_recorder_stop(DroidMediaRecorder *recorder) {

  recorder->m_running = false;
  void *dummy;
  pthread_join(recorder->m_thread, &dummy);
#if ANDROID_MAJOR >= 5
  recorder->m_looper->stop();
#endif

  int err = recorder->m_codec->stop();
  if (err != android::OK) {
      ALOGE("DroidMediaRecorder: error 0x%x stopping codec", -err);
  }
}

void droid_media_recorder_set_data_callbacks(DroidMediaRecorder *recorder,
					     DroidMediaCodecDataCallbacks *cb, void *data) {
  memcpy(&recorder->m_cb, cb, sizeof(recorder->m_cb));
  recorder->m_cb_data = data;
}

};
