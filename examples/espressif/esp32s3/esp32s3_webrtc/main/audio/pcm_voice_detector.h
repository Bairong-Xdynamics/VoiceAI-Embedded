#ifndef PCM_VOICE_DETECTOR_H
#define PCM_VOICE_DETECTOR_H

#include <cstddef>
#include <cstdint>

/** 基于采集 PCM 电平的语音检测：35 < dB < 80 且连续多帧有效 */
class PcmVoiceDetector {
public:
    static constexpr float kSpeechDbMin = 45.0f;
    static constexpr float kSpeechDbMax = 80.0f;
    static constexpr int kSpeechFramesRequired = 5;
    static constexpr int kSilenceFramesRequired = 6;

    void Reset();

    /** 计算单声道 PCM 帧电平：dB = 20 * log10(RMS)，RMS 为 int16 样本幅度 */
    static float ComputeDb(const int16_t* samples, size_t count);

    bool ProcessFrame(const int16_t* samples, size_t count);

    /** 立体声交错 PCM，仅取左声道（麦克风）计算电平 */
    bool ProcessInterleavedMicFrame(const int16_t* interleaved, size_t frames);

    bool IsSpeaking() const { return speaking_; }
    float LastDb() const { return last_db_; }

private:
    bool ApplyLevel(float db);

    bool speaking_ = false;
    float last_db_ = 0.0f;
    int consecutive_valid_frames_ = 0;
    int consecutive_invalid_frames_ = 0;
};

#endif
