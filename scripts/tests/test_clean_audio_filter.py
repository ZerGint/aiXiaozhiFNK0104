import cmath
import math
import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


def high_pass(sample_rate, frequency=140.0, q=math.sqrt(0.5)):
    w0 = 2.0 * math.pi * frequency / sample_rate
    alpha = math.sin(w0) / (2.0 * q)
    a0 = 1.0 + alpha
    return (
        (1.0 + math.cos(w0)) * 0.5 / a0,
        -(1.0 + math.cos(w0)) / a0,
        (1.0 + math.cos(w0)) * 0.5 / a0,
        -2.0 * math.cos(w0) / a0,
        (1.0 - alpha) / a0,
    )


def peaking(sample_rate, frequency=350.0, gain_db=-3.0, q=0.8):
    w0 = 2.0 * math.pi * frequency / sample_rate
    alpha = math.sin(w0) / (2.0 * q)
    amplitude = 10.0 ** (gain_db / 40.0)
    a0 = 1.0 + alpha / amplitude
    return (
        (1.0 + alpha * amplitude) / a0,
        -2.0 * math.cos(w0) / a0,
        (1.0 - alpha * amplitude) / a0,
        -2.0 * math.cos(w0) / a0,
        (1.0 - alpha / amplitude) / a0,
    )


def magnitude_db(coefficients, frequency, sample_rate):
    b0, b1, b2, a1, a2 = coefficients
    z1 = cmath.exp(-2j * math.pi * frequency / sample_rate)
    response = (b0 + b1 * z1 + b2 * z1 * z1) / (1.0 + a1 * z1 + a2 * z1 * z1)
    return 20.0 * math.log10(abs(response))


class CleanAudioFilterTest(unittest.TestCase):
    def combined_gain_db(self, frequency, sample_rate=48000):
        return magnitude_db(high_pass(sample_rate), frequency, sample_rate) + magnitude_db(
            peaking(sample_rate), frequency, sample_rate
        )

    def test_frequency_response(self):
        self.assertLess(self.combined_gain_db(70), -12.0)
        self.assertAlmostEqual(self.combined_gain_db(350), -3.0, delta=0.35)
        self.assertAlmostEqual(self.combined_gain_db(1000), 0.0, delta=0.75)
        self.assertAlmostEqual(self.combined_gain_db(5000), 0.0, delta=0.1)

    def test_coefficients_are_finite_at_supported_rates(self):
        for sample_rate in (16000, 24000, 44100, 48000):
            coefficients = high_pass(sample_rate) + peaking(sample_rate)
            self.assertTrue(all(math.isfinite(value) for value in coefficients))

    def test_only_music_tasks_are_filtered(self):
        source = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        self.assertIn("if (task->is_music) {", source)
        self.assertIn("clean_audio_filter_.Process(task->GetMutablePcmData()", source)

    def test_default_is_off_and_transition_is_short(self):
        service_source = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        filter_source = (ROOT / "main/audio/clean_audio_filter.cc").read_text(encoding="utf-8")
        self.assertIn('GetBool("clean_sound", false)', service_source)
        self.assertIn("constexpr float kTransitionSeconds = 0.010f;", filter_source)

    def test_fnk_quick_settings_has_eq_toggle(self):
        display_source = (ROOT / "main/display/lcd_display.cc").read_text(encoding="utf-8")
        self.assertIn('clean_sound_enabled ? "EQ ON" : "EQ OFF"', display_source)
        self.assertIn("display->ToggleCleanSound();", display_source)
        self.assertIn("audio_service.SetCleanSoundEnabled(enabled);", display_source)


if __name__ == "__main__":
    unittest.main()
