#!/usr/bin/env python3
"""Deterministic synthetic test-signal generator for OpenSourcePedal (Issue #48).

Generates licence-clean (CC0 / project-owned) 32-bit IEEE float WAV test stimuli across all
supported sample rates (44.1, 48, 88.2, 96 kHz; see AGENTS.md section 4 and ADR-0007):
  1. ess_<sr>.wav & ess_inv_<sr>.wav: Farina (2000) Exponential Sine Sweep (20 Hz - 20 kHz, 10 s)
     plus its matched -6 dB/oct time-reversed deconvolution inverse filter.
  2. impulse_<sr>.wav: Unit impulse (x[0] = 1.0, 1.0 s duration) for IR & latency verification.
  3. multitone_m12dbfs_<sr>.wav & multitone_m18dbfs_<sr>.wav: Coherent odd-bin log-spaced multitone
     (31 tones, Newman phases) normalized to -12 dBFS and -18 dBFS peak.
  4. ks_di_<sr>.wav: Deterministic Karplus-Strong synthetic guitar DI phrase (palm-muted chugs,
     strummed chords, arpeggio, and sustained lead note), normalized to -12.0 dBFS peak.

Usage:
  python tools/testsignals/generate.py --out-dir build/testsignals
  python tools/testsignals/generate.py --out-dir build/testsignals --rates 48000 --ess-seconds 2.0 --verify
"""
from __future__ import annotations

import argparse
import math
import struct
import sys
import wave
from pathlib import Path
import numpy as np

SUPPORTED_RATES = (44100, 48000, 88200, 96000)


def write_wav_float32(path: Path, sample_rate: int, samples: np.ndarray) -> None:
    """Write a mono 1D numpy array as an IEEE float32 WAV file (format tag 3)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    data = np.asarray(samples, dtype=np.float32).reshape(-1)
    raw_bytes = data.tobytes()
    num_samples = data.size
    byte_rate = sample_rate * 4
    block_align = 4
    fmt_chunk = struct.pack("<4sIHHIIHH", b"fmt ", 16, 3, 1, sample_rate, byte_rate, block_align, 32)
    data_header = struct.pack("<4sI", b"data", len(raw_bytes))
    riff_size = 4 + len(fmt_chunk) + len(data_header) + len(raw_bytes)
    with path.open("wb") as f:
        f.write(struct.pack("<4sI4s", b"RIFF", riff_size, b"WAVE"))
        f.write(fmt_chunk)
        f.write(data_header)
        f.write(raw_bytes)


def read_wav_float32(path: Path) -> tuple[int, np.ndarray]:
    """Read a mono IEEE float32 WAV file written by write_wav_float32."""
    raw = path.read_bytes()
    if raw[:4] != b"RIFF" or raw[8:12] != b"WAVE":
        raise ValueError(f"{path}: not a valid RIFF/WAVE file")
    fmt_tag, channels, sr = struct.unpack("<HHI", raw[20:28])
    if fmt_tag != 3 or channels != 1:
        raise ValueError(f"{path}: expected mono float32 WAV (got tag={fmt_tag}, ch={channels})")
    data_size = struct.unpack("<I", raw[40:44])[0]
    samples = np.frombuffer(raw[44 : 44 + data_size], dtype=np.float32).copy()
    return sr, samples


def dbfs_to_linear(dbfs: float) -> float:
    return 10.0 ** (dbfs / 20.0)


def generate_impulse(sample_rate: int, duration_s: float = 1.0) -> np.ndarray:
    """Unit impulse x[0] = 1.0 followed by silence."""
    n = max(1, int(round(sample_rate * duration_s)))
    sig = np.zeros(n, dtype=np.float64)
    sig[0] = 1.0
    return sig.astype(np.float32)


def generate_ess(
    sample_rate: int,
    f1: float = 20.0,
    f2: float = 20000.0,
    duration_s: float = 10.0,
    peak_dbfs: float = -1.0,
) -> tuple[np.ndarray, np.ndarray]:
    """Farina (2000) Exponential Sine Sweep and its time-reversed -6 dB/oct inverse filter."""
    f2_clamped = min(f2, 0.49 * sample_rate)
    n = int(round(sample_rate * duration_s))
    t = np.arange(n, dtype=np.float64) / float(sample_rate)
    ln_ratio = math.log(f2_clamped / f1)
    k = (2.0 * math.pi * f1 * duration_s) / ln_ratio
    sweep = np.sin(k * (np.exp(t * ln_ratio / duration_s) - 1.0))

    # Apply short 10 ms raised-cosine fade-in/out to prevent start/stop click leakage
    fade_len = min(int(round(0.010 * sample_rate)), n // 10)
    if fade_len > 1:
        window = 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_len) / fade_len))
        sweep[:fade_len] *= window
        sweep[-fade_len:] *= window[::-1]

    amp = dbfs_to_linear(peak_dbfs)
    sweep_scaled = sweep * amp

    # Farina inverse filter: time-reversed sweep with exponential -6 dB/octave decay envelope
    env = np.exp(-t * ln_ratio / duration_s)
    inv = sweep[::-1] * env
    # Normalize inverse filter energy so convolving sweep with inv yields unit-peak impulse
    norm = float(np.sum(sweep_scaled * sweep * env)) / float(n)
    if norm > 0.0:
        inv /= norm * float(n)
    return sweep_scaled.astype(np.float32), inv.astype(np.float32)


def generate_multitone(
    sample_rate: int,
    peak_dbfs: float = -12.0,
    n_fft: int = 65536,
    num_tones: int = 31,
    f_min: float = 25.0,
    f_max: float = 18000.0,
) -> np.ndarray:
    """Coherent odd-bin multitone with Newman phases, normalized to peak_dbfs."""
    f_high = min(f_max, 0.45 * sample_rate)
    freqs_target = np.geomspace(f_min, f_high, num_tones)
    bins: list[int] = []
    for f in freqs_target:
        k = int(round(f * n_fft / sample_rate))
        k = max(1, k)
        if k % 2 == 0:
            k += 1
        if k < n_fft // 2 and k not in bins:
            bins.append(k)

    t = np.arange(n_fft, dtype=np.float64) / float(sample_rate)
    sig = np.zeros(n_fft, dtype=np.float64)
    m_total = len(bins)
    for idx, k in enumerate(bins):
        # Newman phase formula minimizes crest factor for flat multitone
        phi = (math.pi * (idx ** 2)) / float(m_total)
        freq = k * float(sample_rate) / float(n_fft)
        sig += np.cos(2.0 * math.pi * freq * t + phi)

    peak = float(np.max(np.abs(sig)))
    if peak > 0.0:
        sig = sig * (dbfs_to_linear(peak_dbfs) / peak)
    return sig.astype(np.float32)


def _karplus_strong_note(
    sample_rate: int,
    freq_hz: float,
    duration_s: float,
    damping: float,
    brightness: float,
    pick_pos: float,
    rng: np.random.Generator,
) -> np.ndarray:
    """Synthesize a single plucked guitar string note using extended Karplus-Strong."""
    n_samples = int(round(sample_rate * duration_s))
    delay_len = max(2, int(round(sample_rate / freq_hz)))

    # Initial excitation: bandlimited pick burst + comb filter for pick position
    burst = rng.uniform(-1.0, 1.0, size=delay_len).astype(np.float64)
    burst -= np.mean(burst)
    pick_Comb = max(1, int(round(pick_pos * delay_len)))
    buf = burst.copy()
    for i in range(pick_Comb, delay_len):
        buf[i] -= burst[i - pick_Comb]

    # One-pole lowpass in excitation for pickup brightness
    for i in range(1, delay_len):
        buf[i] = brightness * buf[i] + (1.0 - brightness) * buf[i - 1]

    out = np.zeros(n_samples, dtype=np.float64)
    prev = 0.0
    for i in range(n_samples):
        idx = i % delay_len
        cur = buf[idx]
        out[i] = cur
        # 2-point average loop filter with decay factor (damping)
        averaged = 0.5 * (cur + prev)
        buf[idx] = damping * ((1.0 - brightness) * cur + brightness * averaged)
        prev = cur
    return out


def generate_karplus_strong_di(
    sample_rate: int,
    peak_dbfs: float = -12.0,
    seed: int = 42,
) -> np.ndarray:
    """Deterministic synthetic guitar DI phrase (~4.0 s) covering chugs, chords, and lead."""
    rng = np.random.default_rng(seed)
    total_s = 4.0
    total_n = int(round(sample_rate * total_s))
    mix = np.zeros(total_n, dtype=np.float64)

    def add_note(start_s: float, freq: float, dur_s: float, damp: float, bright: float, gain: float) -> None:
        start_idx = int(round(start_s * sample_rate))
        if start_idx >= total_n:
            return
        note = _karplus_strong_note(sample_rate, freq, dur_s, damp, bright, 0.18, rng) * gain
        end_idx = min(total_n, start_idx + note.size)
        mix[start_idx:end_idx] += note[: end_idx - start_idx]

    # Section 1 (0.0 - 1.0 s): Palm-muted E2 / B2 power-chord 16th chugs (heavy damping)
    for step, t0 in enumerate(np.linspace(0.02, 0.86, 7)):
        damp = 0.965 if step % 2 == 0 else 0.992
        add_note(float(t0), 82.41, 0.22, damp, 0.45, 1.0)   # E2
        add_note(float(t0) + 0.003, 123.47, 0.22, damp, 0.45, 0.8)  # B2

    # Section 2 (1.0 - 2.4 s): Clean/crunch arpeggiated E9 / Asus2 chord
    arp_notes = [
        (1.05, 82.41, 1.3),   # E2
        (1.20, 123.47, 1.15), # B2
        (1.35, 185.00, 1.00), # F#3
        (1.50, 207.65, 0.85), # G#3
        (1.65, 246.94, 0.70), # B3
        (1.80, 329.63, 0.55), # E4
    ]
    for t0, freq, dur in arp_notes:
        add_note(t0, freq, dur, 0.9965, 0.30, 0.75)

    # Section 3 (2.4 - 4.0 s): Sustained high lead notes (B4 -> E5)
    add_note(2.40, 493.88, 0.75, 0.9985, 0.25, 0.95)  # B4
    add_note(3.10, 659.25, 0.88, 0.9988, 0.22, 1.00)  # E5

    # Remove any residual DC and normalize peak to peak_dbfs (-12.0 dBFS reference)
    mix -= np.mean(mix)
    peak = float(np.max(np.abs(mix)))
    if peak > 0.0:
        mix *= dbfs_to_linear(peak_dbfs) / peak
    return mix.astype(np.float32)


def generate_all(out_dir: Path, rates: tuple[int, ...], ess_seconds: float, verify: bool) -> int:
    for sr in rates:
        imp = generate_impulse(sr, duration_s=1.0)
        imp_path = out_dir / f"impulse_{sr}.wav"
        write_wav_float32(imp_path, sr, imp)

        ess, ess_inv = generate_ess(sr, duration_s=ess_seconds, peak_dbfs=-1.0)
        ess_path = out_dir / f"ess_{sr}.wav"
        inv_path = out_dir / f"ess_inv_{sr}.wav"
        write_wav_float32(ess_path, sr, ess)
        write_wav_float32(inv_path, sr, ess_inv)

        mt12 = generate_multitone(sr, peak_dbfs=-12.0)
        mt18 = generate_multitone(sr, peak_dbfs=-18.0)
        mt12_path = out_dir / f"multitone_m12dbfs_{sr}.wav"
        mt18_path = out_dir / f"multitone_m18dbfs_{sr}.wav"
        write_wav_float32(mt12_path, sr, mt12)
        write_wav_float32(mt18_path, sr, mt18)

        ks = generate_karplus_strong_di(sr, peak_dbfs=-12.0)
        ks_path = out_dir / f"ks_di_{sr}.wav"
        write_wav_float32(ks_path, sr, ks)

        if verify:
            r_sr, r_imp = read_wav_float32(imp_path)
            assert r_sr == sr and r_imp[0] == 1.0 and float(np.sum(np.abs(r_imp[1:]))) == 0.0
            _, r_ks = read_wav_float32(ks_path)
            peak_db = 20.0 * math.log10(float(np.max(np.abs(r_ks))))
            assert abs(peak_db - (-12.0)) < 0.01, f"KS DI peak mismatch: {peak_db} dBFS"
            _, r_mt12 = read_wav_float32(mt12_path)
            mt12_db = 20.0 * math.log10(float(np.max(np.abs(r_mt12))))
            assert abs(mt12_db - (-12.0)) < 0.01, f"Multitone -12 peak mismatch: {mt12_db} dBFS"
            _, r_mt18 = read_wav_float32(mt18_path)
            mt18_db = 20.0 * math.log10(float(np.max(np.abs(r_mt18))))
            assert abs(mt18_db - (-18.0)) < 0.01, f"Multitone -18 peak mismatch: {mt18_db} dBFS"

    print(f"testsignals: generated 6 stimuli x {len(rates)} sample rate(s) in {out_dir}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="Generate deterministic DSP test signals.")
    parser.add_argument("--out-dir", type=Path, required=True, help="Output directory for generated WAVs.")
    parser.add_argument(
        "--rates",
        type=int,
        nargs="+",
        default=list(SUPPORTED_RATES),
        help="Sample rates in Hz (default: 44100 48000 88200 96000).",
    )
    parser.add_argument("--ess-seconds", type=float, default=10.0, help="ESS sweep duration in seconds (default: 10.0).")
    parser.add_argument("--verify", action="store_true", help="Read back and verify peak levels and impulse accuracy.")
    args = parser.parse_args()
    return generate_all(args.out_dir, tuple(args.rates), args.ess_seconds, args.verify)


if __name__ == "__main__":
    sys.exit(main())
