#include "pcm_voice_detector.h"
#include <esp_log.h>
#include <cmath>

#define TAG "PcmVoiceDetector"

void PcmVoiceDetector::Reset() {
    speaking_ = false;
    last_db_ = 0.0f;
    consecutive_valid_frames_ = 0;
    consecutive_invalid_frames_ = 0;
}

float PcmVoiceDetector::ComputeDb(const int16_t* samples, size_t count) {
    if (count == 0) {
        return 0.0f;
    }

    double sum_sq = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double s = samples[i];
        sum_sq += s * s;
    }

    const double rms = std::sqrt(sum_sq / static_cast<double>(count));
    if (rms < 1.0) {
        return 0.0f;
    }

    return static_cast<float>(20.0 * std::log10(rms));
}

bool PcmVoiceDetector::ProcessInterleavedMicFrame(const int16_t* interleaved, size_t frames) {
    if (frames == 0) {
        return ProcessFrame(nullptr, 0);
    }

    double sum_sq = 0.0;
    for (size_t i = 0; i < frames; ++i) {
        const double s = interleaved[i * 2];
        sum_sq += s * s;
    }
    const double rms = std::sqrt(sum_sq / static_cast<double>(frames));
    last_db_ = rms < 1.0 ? 0.0f : static_cast<float>(20.0 * std::log10(rms));
    return ApplyLevel(last_db_);
}

bool PcmVoiceDetector::ApplyLevel(float db) {
    //ESP_LOGI(TAG, "PcmVoiceDetector::ApplyLevel: %f", db);
    const bool frame_valid = db > kSpeechDbMin && db < kSpeechDbMax;

    if (frame_valid) {
        consecutive_valid_frames_++;
        consecutive_invalid_frames_ = 0;
    } else {
        consecutive_invalid_frames_++;
        consecutive_valid_frames_ = 0;
    }

    const bool prev_speaking = speaking_;
    if (!speaking_ && consecutive_valid_frames_ >= kSpeechFramesRequired) {
        speaking_ = true;
    } else if (speaking_ && consecutive_invalid_frames_ >= kSilenceFramesRequired) {
        speaking_ = false;
    }

    return speaking_ != prev_speaking;
}

bool PcmVoiceDetector::ProcessFrame(const int16_t* samples, size_t count) {
    last_db_ = ComputeDb(samples, count);
    return ApplyLevel(last_db_);
}
