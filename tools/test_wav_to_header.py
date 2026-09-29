"""wav_to_header.py の単体テスト

実行: python3 -m unittest discover -s tools -v
"""

import contextlib
import io
import os
import re
import struct
import sys
import tempfile
import unittest
import wave

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wav_to_header as w2h  # noqa: E402


def write_wav(path, samples, sr, sampwidth, channels=1):
    """テスト用の WAV を書く。8 bit は 0〜255、16 bit は符号付きの整数で渡す"""
    dtype = np.uint8 if sampwidth == 1 else np.int16
    with wave.open(path, "wb") as f:
        f.setnchannels(channels)
        f.setsampwidth(sampwidth)
        f.setframerate(sr)
        f.writeframes(np.asarray(samples, dtype=dtype).tobytes())


def parse_header(text):
    """.h の文字列から、配列名・値・長さの宣言を取り出す"""
    name = re.search(r"const uint8_t (\w+)_pcm\[\] PROGMEM", text).group(1)
    body = text[text.index("{") + 1: text.index("};")]
    values = np.array([int(v) for v in re.findall(r"\d+", body)], dtype=np.uint8)
    length = int(re.search(r"_pcm_len = (\d+);", text).group(1))
    return name, values, length


def tone(sr, seconds, freq=1000.0, amp=0.5):
    t = np.arange(int(round(sr * seconds))) / sr
    return amp * np.sin(2 * np.pi * freq * t)


def write_float_wav(path, samples, sr):
    """32 bit float（WAVE_FORMAT_IEEE_FLOAT）の WAV を書く。wave モジュールは書けないので自前で組む"""
    data = np.asarray(samples, dtype="<f4").tobytes()
    fmt = struct.pack("<HHIIHH", 3, 1, sr, sr * 4, 4, 32)
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<I", len(fmt)) + fmt)
        f.write(b"data" + struct.pack("<I", len(data)) + data)


def run_main(args):
    """main() を実行する。変換結果の表示はテストの出力に混ぜない"""
    with contextlib.redirect_stdout(io.StringIO()):
        w2h.main(args)


class ToU8Test(unittest.TestCase):
    def test_zero_maps_to_128_and_out_of_range_is_clipped(self):
        out = w2h.to_u8(np.array([0.0, 1.0, -1.0, 2.0, -2.0]))
        self.assertEqual(out.dtype, np.uint8)
        np.testing.assert_array_equal(out, [128, 255, 1, 255, 0])


class ReadWavTest(unittest.TestCase):
    def test_8bit_values_are_unchanged_after_read_and_to_u8(self):
        original = np.arange(256, dtype=np.uint8)
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "in.wav")
            write_wav(path, original, 16000, 1)
            x, sr = w2h.read_wav(path)
        self.assertEqual(sr, 16000)
        np.testing.assert_array_equal(w2h.to_u8(x), original)

    def test_16bit_stereo_is_averaged_to_mono(self):
        left = np.full(100, 16384, dtype=np.int16)
        right = np.zeros(100, dtype=np.int16)
        interleaved = np.column_stack([left, right]).ravel()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "in.wav")
            write_wav(path, interleaved, 24000, 2, channels=2)
            x, sr = w2h.read_wav(path)
        self.assertEqual(sr, 24000)
        self.assertEqual(len(x), 100)
        np.testing.assert_allclose(x, 0.25)

    def test_float_wav_is_rejected_with_a_clear_message(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "float.wav")
            write_float_wav(path, np.zeros(100), 24000)
            with self.assertRaisesRegex(ValueError, "16 bit"):
                w2h.read_wav(path)


class ResampleTest(unittest.TestCase):
    def test_24k_to_16k_keeps_duration_and_frequency(self):
        x = tone(24000, 0.5, freq=1000.0)
        y = w2h.resample(x, 24000, 16000)
        self.assertEqual(len(y), 8000)
        spectrum = np.abs(np.fft.rfft(y * np.hanning(len(y))))
        peak_hz = np.argmax(spectrum) * 16000 / len(y)
        self.assertAlmostEqual(peak_hz, 1000.0, delta=4.0)

    def test_same_rate_returns_input_unchanged(self):
        x = tone(16000, 0.1)
        np.testing.assert_array_equal(w2h.resample(x, 16000, 16000), x)

    def test_amplitude_is_kept(self):
        y = w2h.resample(tone(24000, 0.5, amp=0.5), 24000, 16000)
        middle = y[800:-800]
        self.assertAlmostEqual(float(np.max(np.abs(middle))), 0.5, delta=0.01)

    def test_components_above_8khz_are_removed(self):
        y = w2h.resample(tone(24000, 0.5, freq=10000.0, amp=0.5), 24000, 16000)
        self.assertLess(float(np.sqrt(np.mean(y ** 2))), 1e-3)

    def test_44100_to_16k_keeps_duration_and_frequency(self):
        y = w2h.resample(tone(44100, 0.5, freq=1000.0), 44100, 16000)
        self.assertEqual(len(y), 8000)
        spectrum = np.abs(np.fft.rfft(y * np.hanning(len(y))))
        self.assertAlmostEqual(np.argmax(spectrum) * 16000 / len(y), 1000.0, delta=4.0)


class CutTest(unittest.TestCase):
    def test_start_and_end_are_given_in_seconds(self):
        x = np.arange(16000, dtype=float)
        y = w2h.cut(x, 16000, start=0.25, end=0.5)
        np.testing.assert_array_equal(y, x[4000:8000])

    def test_missing_end_keeps_until_the_last_sample(self):
        x = np.arange(16000, dtype=float)
        np.testing.assert_array_equal(w2h.cut(x, 16000, start=0.5), x[8000:])

    def test_invalid_ranges_are_errors(self):
        x = np.zeros(16000)
        for start, end in [(-0.1, None), (0.5, 0.5), (0.6, 0.5), (0.0, 1.5)]:
            with self.assertRaises(ValueError, msg=f"start={start}, end={end}"):
                w2h.cut(x, 16000, start=start, end=end)


class TrimSilenceTest(unittest.TestCase):
    def test_keeps_sound_plus_head_and_tail_margins(self):
        sr = 16000
        silence = np.zeros(int(0.5 * sr))
        x = np.concatenate([silence, tone(sr, 0.2), silence])
        y = w2h.trim_silence(x, sr)
        expected = int(round((w2h.TRIM_HEAD_SEC + 0.2 + w2h.TRIM_TAIL_SEC) * sr))
        self.assertAlmostEqual(len(y), expected, delta=int(0.02 * sr))

    def test_all_silent_input_is_an_error(self):
        with self.assertRaises(ValueError):
            w2h.trim_silence(np.zeros(1600), 16000)

    def test_silence_with_dc_offset_is_still_trimmed(self):
        # 既存の .h は無音が 127（中心 128 から -1）になっている
        sr = 16000
        offset = -1.0 / 127.0
        silence = np.full(int(0.5 * sr), offset)
        x = np.concatenate([silence, tone(sr, 0.2, amp=0.2) + offset, silence])
        y = w2h.trim_silence(x, sr)
        expected = int(round((w2h.TRIM_HEAD_SEC + 0.2 + w2h.TRIM_TAIL_SEC) * sr))
        self.assertAlmostEqual(len(y), expected, delta=int(0.02 * sr))

    def test_silence_is_trimmed_when_the_voice_part_has_its_own_bias(self):
        # 声の区間の平均が無音の値からずれていても、無音を無音として扱う（do_research_16k.h で起きた）
        sr = 16000
        offset = -1.0 / 127.0
        silence = np.full(int(0.5 * sr), offset)
        voice = tone(sr, 0.2, amp=0.2) + offset + 0.05
        y = w2h.trim_silence(np.concatenate([silence, voice, silence]), sr)
        expected = int(round((w2h.TRIM_HEAD_SEC + 0.2 + w2h.TRIM_TAIL_SEC) * sr))
        self.assertAlmostEqual(len(y), expected, delta=int(0.02 * sr))


class NormalizeTest(unittest.TestCase):
    def test_removes_dc_offset_and_scales_peak_to_target(self):
        x = np.array([0.0, 0.2, -0.2, 0.0]) + 0.05
        y = w2h.normalize(x)
        np.testing.assert_allclose(y, [0.0, w2h.PEAK_TARGET, -w2h.PEAK_TARGET, 0.0], atol=1e-12)

    def test_silent_input_is_an_error(self):
        with self.assertRaises(ValueError):
            w2h.normalize(np.full(100, 0.3))

    def test_mostly_silent_clip_keeps_silence_at_zero(self):
        # 無音が 0 からずれていても、正規化後の無音は 0（8 bit で 128）になる
        x = np.concatenate([np.full(900, -0.01), np.array([0.3, 0.1, -0.1] * 30)])
        y = w2h.normalize(x)
        np.testing.assert_allclose(y[:900], 0.0, atol=1e-12)


class FadeTest(unittest.TestCase):
    def test_both_ends_become_zero_and_the_middle_is_unchanged(self):
        sr = 16000
        x = np.ones(sr // 10)
        y = w2h.fade(x, sr)
        n = int(round(w2h.FADE_SEC * sr))
        self.assertEqual(y[0], 0.0)
        self.assertEqual(y[-1], 0.0)
        np.testing.assert_array_equal(y[n:-n], x[n:-n])


class FormatHeaderTest(unittest.TestCase):
    def test_matches_the_style_of_the_existing_headers(self):
        data = np.array([128] * 17 + [127] * 3, dtype=np.uint8)
        expected = (
            "#ifndef VOICE_16K_H\n"
            "#define VOICE_16K_H\n"
            "\n"
            "#include <Arduino.h>\n"
            "\n"
            "const uint8_t voice_pcm[] PROGMEM = {\n"
            "  " + ", ".join(["128"] * 16) + ",\n"
            "  128, 127, 127, 127,\n"
            "};\n"
            "\n"
            "const unsigned int voice_pcm_len = 20;\n"
            "\n"
            "#endif\n"
        )
        self.assertEqual(w2h.format_header("voice", data), expected)

    def test_name_must_be_a_c_identifier(self):
        data = np.array([128], dtype=np.uint8)
        for bad in ["zunda-01", "1st", "ずんだ"]:
            with self.assertRaises(ValueError):
                w2h.format_header(bad, data)


class MainTest(unittest.TestCase):
    def test_24k_16bit_wav_becomes_trimmed_16k_header(self):
        sr = 24000
        silence = np.zeros(int(0.3 * sr))
        x = np.concatenate([silence, tone(sr, 0.2, amp=0.8), silence])
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.wav")
            dst = os.path.join(d, "tone.h")
            write_wav(src, np.round(x * 32767), sr, 2)
            run_main([src, dst, "--name", "tone"])
            with open(dst, encoding="utf-8") as f:
                name, values, length = parse_header(f.read())
        self.assertEqual(name, "tone")
        self.assertEqual(length, len(values))
        expected = (w2h.TRIM_HEAD_SEC + 0.2 + w2h.TRIM_TAIL_SEC) * w2h.SAMPLE_RATE
        self.assertAlmostEqual(length, expected, delta=0.02 * w2h.SAMPLE_RATE)
        self.assertEqual(values[0], 128)
        self.assertEqual(values[-1], 128)
        self.assertGreater(int(values.max()), 220)

    def test_start_end_without_trim_keeps_the_exact_range(self):
        sr = w2h.SAMPLE_RATE
        original = np.tile(np.arange(256, dtype=np.uint8), sr // 256 + 1)[:sr]
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.wav")
            dst = os.path.join(d, "part.h")
            write_wav(src, original, sr, 1)
            run_main([src, dst, "--name", "part", "--start", "0.1", "--end", "0.2",
                      "--no-trim", "--no-normalize"])
            with open(dst, encoding="utf-8") as f:
                _, values, length = parse_header(f.read())
        n = int(round(w2h.FADE_SEC * sr))
        self.assertEqual(length, 1600)
        np.testing.assert_array_equal(values[n:-n], original[1600 + n: 3200 - n])

    def run_main_on_tone(self, amp, extra_args):
        sr = 24000
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.wav")
            dst = os.path.join(d, "quiet.h")
            write_wav(src, np.round(tone(sr, 0.3, amp=amp) * 32767), sr, 2)
            run_main([src, dst, "--name", "quiet", "--no-trim"] + extra_args)
            with open(dst, encoding="utf-8") as f:
                _, values, _ = parse_header(f.read())
        return int(np.max(np.abs(values.astype(int) - 128)))

    def test_quiet_wav_is_normalized_to_peak_124_by_default(self):
        self.assertEqual(self.run_main_on_tone(0.1, []), 124)

    def test_no_normalize_keeps_the_original_level(self):
        self.assertEqual(self.run_main_on_tone(0.1, ["--no-normalize"]), 13)

    def test_invalid_range_stops_with_a_usage_error(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "in.wav")
            write_wav(src, np.full(16000, 128), 16000, 1)
            args = [src, os.path.join(d, "x.h"), "--name", "x", "--start", "0.6", "--end", "0.5"]
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as cm:
                run_main(args)
        self.assertEqual(cm.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
