#ifndef _FNK_EXTERNAL_I2S_AUDIO_CODEC_H_
#define _FNK_EXTERNAL_I2S_AUDIO_CODEC_H_

#include "codecs/es8311_audio_codec.h"

#if CONFIG_FNK_EXTERNAL_I2S_SPEAKER

class FnkExternalI2sAudioCodec : public Es8311AudioCodec {
public:
    FnkExternalI2sAudioCodec(void* i2c_master_handle, i2c_port_t i2c_port);
    ~FnkExternalI2sAudioCodec() override;

    void SetOutputVolume(int volume) override;
    void EnableOutput(bool enable) override;
    void SetOutputSource(AudioOutputSource source, bool stream_start = false) override;

private:
    static constexpr size_t kConversionFrames = 120;
    i2s_chan_handle_t external_tx_handle_ = nullptr;
    static constexpr int32_t kGainScale = 32768;
    static constexpr uint32_t kFadeSamples = 33600;
    AudioOutputSource output_source_ = AudioOutputSource::kSystem;
    int32_t current_gain_ = 0;
    int32_t fade_start_gain_ = 0;
    int32_t target_gain_ = 0;
    uint32_t fade_position_ = 0;
    uint32_t fade_length_ = 0;

    static int32_t PerceptualGain(int volume);
    void SetImmediateGain(int32_t gain);
    void RetargetFade(int32_t gain);
    int32_t NextGain();

#if CONFIG_FNK_EXTERNAL_I2S_DIAGNOSTICS
    struct PcmDiagnostics {
        int16_t input_min = INT16_MAX;
        int16_t input_max = INT16_MIN;
        uint32_t input_peak = 0;
        uint32_t output_peak = 0;
        uint64_t input_square_sum = 0;
        uint64_t sample_count = 0;
        uint32_t input_full_scale = 0;
        uint32_t above_90_percent = 0;
        uint32_t above_95_percent = 0;
        uint32_t above_99_percent = 0;
        uint32_t output_saturated = 0;
        int64_t last_log_us = 0;
    } diagnostics_;

    void UpdateDiagnostics(const int16_t* input, const int16_t* output, int samples, int volume);
#endif

    bool UsesEs8311Output() const override { return false; }
    int Write(const int16_t* data, int samples) override;
};

#endif
#endif
