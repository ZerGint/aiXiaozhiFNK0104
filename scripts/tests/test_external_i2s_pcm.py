import math
import unittest

from scripts.external_i2s_pcm import (GAIN_SCALE, ai_logical_volume, perceptual_gain_q15,
                                      scale_and_duplicate, sine_pcm)


class ExternalI2sPcmTests(unittest.TestCase):
    def test_known_vectors_and_lr_duplication(self):
        vectors = [0, 0, 1, -1, 100, -100, 32767, -32768, 30000, -30000]
        for volume in (100, 50, 25):
            stereo = scale_and_duplicate(vectors, volume)
            self.assertEqual(len(stereo), len(vectors) * 2)
            self.assertTrue(all(stereo[i] == stereo[i + 1]
                                for i in range(0, len(stereo), 2)))
            gain = perceptual_gain_q15(volume)
            expected = [math.trunc(value * gain / GAIN_SCALE) for value in vectors]
            self.assertEqual(stereo[::2], expected)

    def test_full_scale_boundaries_do_not_overflow(self):
        self.assertEqual(scale_and_duplicate([32767, -32768], 100),
                         [26213, 26213, -26214, -26214])
        self.assertEqual(scale_and_duplicate([32767, -32768], 50),
                         [6537, 6537, -6538, -6538])

    def test_sine_levels_and_volumes(self):
        for level_dbfs in (-20, -12, -6, -3):
            mono = sine_pcm(1000, 24000, level_dbfs, 240)
            source_peak = max(abs(value) for value in mono)
            expected_peak = round(32767 * 10 ** (level_dbfs / 20))
            self.assertLessEqual(abs(source_peak - expected_peak), 1)
            for volume in (100, 50, 25):
                stereo = scale_and_duplicate(mono, volume)
                self.assertEqual(stereo[::2], stereo[1::2])
                self.assertLessEqual(
                    abs(max(abs(value) for value in stereo[::2]) -
                        math.trunc(source_peak * perceptual_gain_q15(volume) / GAIN_SCALE)), 1)

    def test_integer_scaling_adds_only_quantization_error(self):
        mono = sine_pcm(1000, 24000, -3, 240)
        for volume in (100, 50, 25):
            converted = scale_and_duplicate(mono, volume)[::2]
            ideal = [value * perceptual_gain_q15(volume) / GAIN_SCALE for value in mono]
            error_rms = math.sqrt(sum((actual - expected) ** 2
                                      for actual, expected in zip(converted, ideal)) /
                                  len(mono))
            self.assertLess(error_rms, 1.0)

    def test_curve_targets_monotonicity_and_cap(self):
        expected_db = {5: -47, 10: -38, 20: -29, 30: -23, 40: -18,
                       50: -14, 60: -10, 80: -6, 100: 20 * math.log10(0.8)}
        gains = [perceptual_gain_q15(volume) for volume in range(101)]
        self.assertEqual(gains[0], 0)
        self.assertTrue(all(a <= b for a, b in zip(gains, gains[1:])))
        self.assertLessEqual(max(gains), round(0.8 * GAIN_SCALE))
        for volume, expected in expected_db.items():
            actual = 20 * math.log10(perceptual_gain_q15(volume) / GAIN_SCALE)
            self.assertLessEqual(abs(actual - expected), 1.0)

    def test_ai_volume_and_explicit_mute(self):
        expected_volumes = {
            100: 80, 80: 60, 60: 40, 50: 30, 40: 20,
            30: 20, 20: 20, 10: 20, 1: 20, 0: 0,
        }
        expected_gains = {
            100: 16423, 80: 10362, 60: 4125, 50: 2320, 40: 1163,
            30: 1163, 20: 1163, 10: 1163, 1: 1163, 0: 0,
        }
        for user_volume, ai_volume in expected_volumes.items():
            stored_user_volume = user_volume
            self.assertEqual(ai_logical_volume(user_volume), ai_volume)
            self.assertEqual(perceptual_gain_q15(ai_volume), expected_gains[user_volume])
            self.assertEqual(stored_user_volume, user_volume)

    def test_700ms_fade_and_retarget_without_jump(self):
        total = 33600
        target = perceptual_gain_q15(80)
        gains = [target * position // total for position in range(1, total + 1)]
        self.assertTrue(all(a <= b for a, b in zip(gains, gains[1:])))
        self.assertEqual(gains[-1], target)
        current = gains[14400 - 1]
        new_target = perceptual_gain_q15(30)
        remaining = total - 14400
        retargeted = [current + (new_target - current) * position // remaining
                      for position in range(1, remaining + 1)]
        self.assertLessEqual(abs(retargeted[0] - current), 1)
        self.assertEqual(retargeted[-1], new_target)


if __name__ == "__main__":
    unittest.main()
