"""Host model of the FNK0104S external-I2S sample conversion."""

import math

GAIN_SCALE = 32768
CURVE = ((0, 0), (5, 146), (10, 413), (20, 1163), (30, 2320),
         (40, 4125), (50, 6538), (60, 10362), (80, 16423), (100, 26214))


def perceptual_gain_q15(volume):
    volume = max(0, min(100, volume))
    for (low_volume, low_gain), (high_volume, high_gain) in zip(CURVE, CURVE[1:]):
        if volume <= high_volume:
            return low_gain + ((high_gain - low_gain) * (volume - low_volume) //
                               (high_volume - low_volume))
    return CURVE[-1][1]


def ai_logical_volume(user_volume):
    return 0 if user_volume == 0 else max(20, user_volume - 20)


def scale_and_duplicate(samples, volume):
    gain = perceptual_gain_q15(volume)
    output = []
    for value in samples:
        scaled = math.trunc(value * gain / GAIN_SCALE)
        scaled = max(-32768, min(32767, scaled))
        output.extend((scaled, scaled))
    return output


def sine_pcm(frequency, sample_rate, level_dbfs, frames):
    amplitude = 32767.0 * 10.0 ** (level_dbfs / 20.0)
    return [
        round(amplitude * math.sin(2.0 * math.pi * frequency * index / sample_rate))
        for index in range(frames)
    ]
