#include "clean_audio_filter.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
constexpr float kPi = 3.14159265358979323846f;
constexpr float kHighPassFrequencyHz = 140.0f;
constexpr float kHighPassQ = 0.70710678f;
constexpr float kPresenceFrequencyHz = 350.0f;
constexpr float kPresenceGainDb = -3.0f;
constexpr float kPresenceQ = 0.8f;
constexpr float kTransitionSeconds = 0.010f;
}  // namespace

float CleanAudioFilter::Biquad::Process(float input) {
    const float output = b0 * input + z1;
    z1 = b1 * input - a1 * output + z2;
    z2 = b2 * input - a2 * output;
    return output;
}

void CleanAudioFilter::Biquad::Reset() {
    z1 = 0.0f;
    z2 = 0.0f;
}

void CleanAudioFilter::Configure(uint32_t sample_rate) {
    if (sample_rate == sample_rate_) {
        return;
    }
    sample_rate_ = sample_rate;
    UpdateCoefficients();
    Reset();
}

void CleanAudioFilter::Reset() {
    high_pass_.Reset();
    presence_cut_.Reset();
}

void CleanAudioFilter::SetEnabled(bool enabled) {
    enabled_.store(enabled, std::memory_order_release);
}

bool CleanAudioFilter::IsEnabled() const { return enabled_.load(std::memory_order_acquire); }

void CleanAudioFilter::Process(int16_t* samples, size_t sample_count) {
    if (samples == nullptr || sample_count == 0 || sample_rate_ == 0) {
        return;
    }

    const bool target_enabled = enabled_.load(std::memory_order_acquire);
    if (target_enabled != applied_enabled_) {
        if (target_enabled && wet_mix_ == 0.0f) {
            Reset();
        }
        applied_enabled_ = target_enabled;
    }
    if (!target_enabled && wet_mix_ == 0.0f) {
        return;
    }

    const float target_mix = target_enabled ? 1.0f : 0.0f;
    const float transition_samples =
        std::max(1.0f, static_cast<float>(sample_rate_) * kTransitionSeconds);
    const float mix_step = 1.0f / transition_samples;

    for (size_t i = 0; i < sample_count; ++i) {
        const float dry = static_cast<float>(samples[i]);
        const float wet = presence_cut_.Process(high_pass_.Process(dry));

        if (wet_mix_ < target_mix) {
            wet_mix_ = std::min(target_mix, wet_mix_ + mix_step);
        } else if (wet_mix_ > target_mix) {
            wet_mix_ = std::max(target_mix, wet_mix_ - mix_step);
        }

        float output = dry + (wet - dry) * wet_mix_;
        if (!std::isfinite(output)) {
            output = 0.0f;
            Reset();
        }
        output = std::clamp(output, static_cast<float>(std::numeric_limits<int16_t>::min()),
                            static_cast<float>(std::numeric_limits<int16_t>::max()));
        samples[i] = static_cast<int16_t>(std::lrintf(output));
    }

    if (!target_enabled && wet_mix_ == 0.0f) {
        Reset();
    }
}

void CleanAudioFilter::UpdateCoefficients() {
    if (sample_rate_ <= static_cast<uint32_t>(kPresenceFrequencyHz * 2.0f)) {
        sample_rate_ = 0;
        high_pass_ = {};
        presence_cut_ = {};
        return;
    }

    const float high_pass_w0 = 2.0f * kPi * kHighPassFrequencyHz / sample_rate_;
    const float high_pass_cos = std::cos(high_pass_w0);
    const float high_pass_alpha = std::sin(high_pass_w0) / (2.0f * kHighPassQ);
    const float high_pass_a0 = 1.0f + high_pass_alpha;
    high_pass_.b0 = ((1.0f + high_pass_cos) * 0.5f) / high_pass_a0;
    high_pass_.b1 = -(1.0f + high_pass_cos) / high_pass_a0;
    high_pass_.b2 = high_pass_.b0;
    high_pass_.a1 = (-2.0f * high_pass_cos) / high_pass_a0;
    high_pass_.a2 = (1.0f - high_pass_alpha) / high_pass_a0;

    const float peak_w0 = 2.0f * kPi * kPresenceFrequencyHz / sample_rate_;
    const float peak_cos = std::cos(peak_w0);
    const float peak_alpha = std::sin(peak_w0) / (2.0f * kPresenceQ);
    const float amplitude = std::pow(10.0f, kPresenceGainDb / 40.0f);
    const float peak_a0 = 1.0f + peak_alpha / amplitude;
    presence_cut_.b0 = (1.0f + peak_alpha * amplitude) / peak_a0;
    presence_cut_.b1 = (-2.0f * peak_cos) / peak_a0;
    presence_cut_.b2 = (1.0f - peak_alpha * amplitude) / peak_a0;
    presence_cut_.a1 = presence_cut_.b1;
    presence_cut_.a2 = (1.0f - peak_alpha / amplitude) / peak_a0;
}
