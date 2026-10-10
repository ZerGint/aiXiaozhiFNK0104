#ifndef CLEAN_AUDIO_FILTER_H
#define CLEAN_AUDIO_FILTER_H

#include <atomic>
#include <cstddef>
#include <cstdint>

class CleanAudioFilter {
public:
    void Configure(uint32_t sample_rate);
    void Reset();
    void SetEnabled(bool enabled);
    bool IsEnabled() const;
    void Process(int16_t* samples, size_t sample_count);

private:
    struct Biquad {
        float b0 = 1.0f;
        float b1 = 0.0f;
        float b2 = 0.0f;
        float a1 = 0.0f;
        float a2 = 0.0f;
        float z1 = 0.0f;
        float z2 = 0.0f;

        float Process(float input);
        void Reset();
    };

    void UpdateCoefficients();

    std::atomic<bool> enabled_{false};
    uint32_t sample_rate_ = 0;
    float wet_mix_ = 0.0f;
    bool applied_enabled_ = false;
    Biquad high_pass_;
    Biquad presence_cut_;
};

#endif
