"""WAV を M5StickC 用の再生データ（.h）に変換する

出力は 16 kHz・8 bit（符号なし、無音は 128）で、既存の .h と同じ書式にする。
前後の無音を削り、最大振幅を ±124 に合わせ（ピーク正規化）、両端に 10 ms のフェードを付ける。

使い方:
  python3 tools/wav_to_header.py 入力.wav 出力.h --name 配列名 [--start 秒] [--end 秒] [--no-trim] [--no-normalize]
"""

import argparse
import re
import wave

import numpy as np

SAMPLE_RATE = 16000        # M5StickC 側の SAMPLE_RATE と同じ値
TRIM_THRESHOLD_DB = -40.0  # 最大振幅との差がこれより小さい 10 ms 区間を無音とみなす
TRIM_HEAD_SEC = 0.03       # 音の始まりの前に残す長さ
TRIM_TAIL_SEC = 0.08       # 音の終わりの後に残す長さ
FADE_SEC = 0.01            # 両端のフェードの長さ
PEAK_TARGET = 124 / 127    # ピーク正規化後の最大振幅（8 bit で ±124）
VALUES_PER_LINE = 16


def read_wav(path):
    """WAV を読み、モノラルの float 配列とサンプリング周波数を返す"""
    try:
        with wave.open(path, "rb") as f:
            channels = f.getnchannels()
            width = f.getsampwidth()
            sr = f.getframerate()
            raw = f.readframes(f.getnframes())
    except wave.Error as e:
        raise ValueError(f"8 bit または 16 bit の整数 PCM の WAV が必要である（{e}）") from e

    if width == 1:
        x = (np.frombuffer(raw, dtype=np.uint8).astype(float) - 128.0) / 127.0
    elif width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(float) / 32768.0
    else:
        raise ValueError(f"8 bit または 16 bit の WAV が必要である（入力は {width * 8} bit）")

    return x.reshape(-1, channels).mean(axis=1), sr


def cut(x, sr, start=None, end=None):
    """start 秒から end 秒までを切り出す。省略した側は端まで残す"""
    duration = len(x) / sr
    s = 0.0 if start is None else start
    e = duration if end is None else end
    if not (0.0 <= s < e <= duration + 1e-9):
        raise ValueError(f"切り出し範囲は 0 ≤ start < end ≤ {duration:.2f} 秒が必要である（start={s}, end={e}）")

    return x[int(round(s * sr)): int(round(e * sr))]


def resample(x, sr_in, sr_out):
    """FFT で周波数を変換する。sr_out の半分より高い成分は落とす"""
    if sr_in == sr_out:
        return x
    n_out = int(round(len(x) * sr_out / sr_in))
    spectrum = np.fft.rfft(x)
    bins = n_out // 2 + 1
    resized = np.zeros(bins, dtype=complex)
    m = min(len(spectrum), bins)
    resized[:m] = spectrum[:m]
    return np.fft.irfft(resized, n_out) * (n_out / len(x))


def trim_silence(x, sr):
    """前後の無音を削る。音の前に TRIM_HEAD_SEC、後に TRIM_TAIL_SEC を残す"""
    # 無音が 128 からずれた .h（既存のものは 127）でも判定できるように、中央値を引いてから測る
    # （平均は声の区間の偏りに引きずられ、無音が 0 にならないことがある）
    centered = x - np.median(x) if len(x) else x
    peak = float(np.max(np.abs(centered))) if len(x) else 0.0
    if peak == 0.0:
        raise ValueError("音が入っていない（すべて無音）")

    frame = max(1, int(0.01 * sr))
    n = len(x) // frame
    rms = np.sqrt(np.mean(centered[: n * frame].reshape(n, frame) ** 2, axis=1))
    loud = np.where(rms > peak * 10 ** (TRIM_THRESHOLD_DB / 20))[0]
    if len(loud) == 0:
        return x

    start = max(0, loud[0] * frame - int(round(TRIM_HEAD_SEC * sr)))
    end = min(len(x), (loud[-1] + 1) * frame + int(round(TRIM_TAIL_SEC * sr)))
    return x[start:end]


def normalize(x):
    """中央値（無音の値）を引き、最大振幅を PEAK_TARGET に合わせる"""
    y = x - np.median(x)
    peak = float(np.max(np.abs(y))) if len(y) else 0.0
    if peak == 0.0:
        raise ValueError("音が入っていない（すべて無音）")
    return y * (PEAK_TARGET / peak)


def fade(x, sr):
    """両端に FADE_SEC の直線フェードを付ける"""
    n = min(int(round(FADE_SEC * sr)), len(x) // 2)
    y = x.astype(float).copy()
    if n > 0:
        ramp = np.linspace(0.0, 1.0, n)
        y[:n] *= ramp
        y[-n:] *= ramp[::-1]
    return y


def to_u8(x):
    """-1〜1 の float を 8 bit 符号なし（無音は 128）に変換する"""
    return np.clip(np.round(x * 127.0) + 128.0, 0, 255).astype(np.uint8)


def format_header(name, data):
    """既存の .h と同じ書式の文字列を作る"""
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", name):
        raise ValueError(f"配列名は英字または _ で始まる英数字と _ が必要である（{name}）")

    guard = f"{name.upper()}_16K_H"
    lines = []
    for i in range(0, len(data), VALUES_PER_LINE):
        chunk = data[i: i + VALUES_PER_LINE]
        lines.append("  " + ", ".join(str(int(v)) for v in chunk) + ",")

    return (
        f"#ifndef {guard}\n"
        f"#define {guard}\n"
        "\n"
        "#include <Arduino.h>\n"
        "\n"
        f"const uint8_t {name}_pcm[] PROGMEM = {{\n"
        + "\n".join(lines) + "\n"
        "};\n"
        "\n"
        f"const unsigned int {name}_pcm_len = {len(data)};\n"
        "\n"
        "#endif\n"
    )


def main(argv=None):
    parser = argparse.ArgumentParser(description="WAV を M5StickC 用の .h に変換する")
    parser.add_argument("input", help="入力する WAV")
    parser.add_argument("output", help="出力する .h")
    parser.add_argument("--name", required=True, help="配列名（英数字と _）")
    parser.add_argument("--start", type=float, help="切り出しの開始（秒）")
    parser.add_argument("--end", type=float, help="切り出しの終了（秒）")
    parser.add_argument("--no-trim", action="store_true", help="前後の無音を削らない")
    parser.add_argument("--no-normalize", action="store_true", help="最大振幅を合わせない")
    args = parser.parse_args(argv)

    try:
        x, sr = read_wav(args.input)
        x = cut(x, sr, args.start, args.end)
        x = resample(x, sr, SAMPLE_RATE)
        if not args.no_trim:
            x = trim_silence(x, SAMPLE_RATE)
        if not args.no_normalize:
            x = normalize(x)
        data = to_u8(fade(x, SAMPLE_RATE))
        text = format_header(args.name, data)
    except ValueError as e:
        parser.error(str(e))

    with open(args.output, "w", encoding="utf-8") as f:
        f.write(text)
    print(f"{args.output}: {len(data)} samples ({len(data) / SAMPLE_RATE:.2f} s)")


if __name__ == "__main__":
    main()
