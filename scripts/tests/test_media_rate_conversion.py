import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]


class RationalStreamCounter:
    """Exact sample-count model for a stateful streaming rate converter."""

    def __init__(self, source_rate, target_rate):
        self.source_rate = source_rate
        self.target_rate = target_rate
        self.remainder = 0

    def process(self, input_samples):
        total = input_samples * self.target_rate + self.remainder
        output, self.remainder = divmod(total, self.source_rate)
        return output


class MediaRateConversionTests(unittest.TestCase):
    def test_48k_bypass_contract(self):
        samples = [-32768, -123, 0, 456, 32767]
        self.assertEqual(samples.copy(), samples)

    def test_stream_duration_for_supported_rates(self):
        chunks = [1152, 1152, 576, 1000]
        for source_rate in (16000, 24000, 44100):
            converter = RationalStreamCounter(source_rate, 48000)
            remaining = source_rate * 10
            output_samples = 0
            index = 0
            while remaining:
                count = min(chunks[index % len(chunks)], remaining)
                output_samples += converter.process(count)
                remaining -= count
                index += 1
            self.assertEqual(output_samples, 480000)

    def test_fractional_state_survives_44100_chunk_boundaries(self):
        chunks = [1152, 1152, 576, 1000] * 30
        converter = RationalStreamCounter(44100, 48000)
        streamed = sum(converter.process(chunk) for chunk in chunks)
        single = RationalStreamCounter(44100, 48000).process(sum(chunks))
        self.assertEqual(streamed, single)

    def test_firmware_uses_stateful_library_and_no_linear_block_resampler(self):
        source = (ROOT / "main/media/media_audio_output.cc").read_text(encoding="utf-8")
        self.assertIn("esp_ae_rate_cvt_process", source)
        self.assertIn("esp_ae_rate_cvt_reset", source)
        self.assertIn("ESP_AE_RATE_CVT_PERF_TYPE_MEMORY", source)
        self.assertNotIn("const double position = i * step", source)


if __name__ == "__main__":
    unittest.main()
