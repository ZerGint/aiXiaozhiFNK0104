#ifndef MEDIA_AUDIO_OUTPUT_H
#define MEDIA_AUDIO_OUTPUT_H

#include <cstddef>
#include <cstdint>

class AudioCodec;

void PushMediaPcm(AudioCodec* codec, const int16_t* pcm_data, size_t num_samples, uint32_t channels,
                  uint32_t sample_rate, uint32_t target_rate, bool is_radio = false);

void BeginMediaPcmStream();

void InitializeMediaRateConverterEarly(uint32_t source_rate = 44100, uint32_t target_rate = 48000);

void EnsureMp3DecoderRegistered();

#endif
