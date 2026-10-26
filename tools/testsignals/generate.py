#!/usr/bin/env python3
"""Deterministic synthetic test-signal generator for OpenSourcePedal (Issue #48, ADR-0007).

Generates licence-clean (CC0 / project-owned) 32-bit IEEE float WAV test stimuli across all
supported sample rates (44.1, 48, 88.2, 96 kHz; see AGENTS.md section 4 and ADR-0007):
  1. impulse_<sr>.wav: Unit impulse (x[0] = 1.0, 1.0 s duration) for IR & latency verification.
  2. ess_<sr>.wav & ess_inv_<sr>.wav: Farina (2000) Exponential Sine Sweep (20 Hz - 20 kHz, 10 s)
     plus its matched -6 dB/oct time-reversed deconvolution inverse filter.
  3. coherent_<f0>hz_<level>_<sr>.wav: ADR-0007 REQ-TEST-002 single-tone coherent stimuli for
     nonlinear THD and Aliasing-to-Signal Ratio (ASR) measurement:
       - f0 in {110, 440, 1000, 2500, 5000, 8000} Hz mapped to odd FFT bin k of N_FFT = 65536;
       - rendered at -6.0 dBFS ('m6dbfs') and 0.0 dBFS ('0dbfs');
       - length = 8192 (warm-up) + 65536 (rectangular-window FFT window) = 73728 samples.
  4. multitone_m12dbfs_<sr>.wav & multitone_m18dbfs_<sr>.wav: 31-tone Newman-phase multitone
     normalized to -12 dBFS and -18 dBFS peak (for broadband level / LUFS / linear FR checks ONLY;
     do NOT use for nonlinear THD/ASR, which uses coherent_<f0>hz_*).
  5. ks_di_<sr>.wav: Deterministic Karplus-Strong synthetic guitar DI phrase (palm-muted E5
     power-chord [E2+B2] chugs, arpeggiated chord, and sustained lead notes) at -12.0 dBFS peak.

Usage:
  python tools/testsignals/generate.py --out-dir build/testsignals --verify
  python tools/testsignals/generate.py --out-dir build/testsignals --rates 48000 --ess-seconds 2.0 --verify
"""
from __future__ import annotations

import argparse
import hashlib
import math
import struct
import sys
from pathlib import Path
import numpy as np

SUPPORTED_RATES = (44100, 48000, 88200, 96000)
COHERENT_NOMINAL_FREQS_HZ = (110, 440, 1000, 2500, 5000, 8000)
COHERENT_LEVELS = (("m6dbfs", -6.0), ("0dbfs", 0.0))
COHERENT_WARMUP_SAMPLES = 8192
COHERENT_N_FFT = 65536


def _require(condition: bool, message: str) -> None:
    """Verification check that is never stripped by `python -O`."""
    if not condition:
        raise SystemExit(f"testsignals verify failed: {message}")


def write_wav_float32(path: Path, sample_rate: int, samples: np.ndarray) -> None:
    """Write a mono 1D numpy array as an IEEE float32 WAV file (44-byte header, format tag 3)."""
    path.parent.mkdir(parents=True, exist_ok=True)
    data = np.asarray(samples, dtype="<f4").reshape(-1)
    raw_bytes = data.tobytes()
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
    """Read a mono IEEE float32 WAV file written by `write_wav_float32` (44-byte header only)."""
    raw = path.read_bytes()
    _require(raw[:4] == b"RIFF" and raw[8:12] == b"WAVE", f"{path}: not a valid RIFF/WAVE file")
    fmt_tag, channels, sr = struct.unpack("<HHI", raw[20:28])
    _require(fmt_tag == 3 and channels == 1, f"{path}: expected mono float32 WAV (got tag={fmt_tag}, ch={channels})")
    data_size = struct.unpack("<I", raw[40:44])[0]
    samples = np.frombuffer(raw[44 : 44 + data_size], dtype="<f4").copy()
    return sr, samples


def dbfs_to_linear(dbfs: float) -> float:
    return 10.0 ** (dbfs / 20.0)


def odd_coherent_bin(f0_hz: float, sample_rate: int, n_fft: int = COHERENT_N_FFT) -> int:
    """Return the odd FFT bin k closest to f0_hz (coprime with power-of-two n_fft, ADR-0007)."""
    k = max(1, int(round(f0_hz * n_fft / float(sample_rate))))
    if k % 2 == 0:
        k += 1
    return k


def generate_coherent_tone(
    sample_rate: int,
    f0_hz: float,
    peak_dbfs: float,
    n_warmup: int = COHERENT_WARMUP_SAMPLES,
    n_fft: int = COHERENT_N_FFT,
) -> tuple[int, np.ndarray]:
    """Generate ADR-0007 REQ-TEST-002 coherent odd-bin sine of length n_warmup + n_fft."""
    k = odd_coherent_bin(f0_hz, sample_rate, n_fft)
    total_n = n_warmup + n_fft
    # Align phase so the post-warmup analysis window [n_warmup : n_warmup + n_fft] starts at phase 0
    idx = np.arange(total_n, dtype=np.float64) - float(n_warmup)
    amp = dbfs_to_linear(peak_dbfs)
    sig = amp * np.sin((2.0 * math.pi * float(k) / float(n_fft)) * idx)
    return k, sig.astype(np.float32)


def generate_impulse(sample_rate: int, duration_s: float = 1.0) -> np.ndarray:
    """Unit impulse x[0] = 1.0 followed by silence."""
    n = max(1, int(round(sample_rate * duration_s)))
    sig = np.zeros(n, dtype=np.float32)
    sig[0] = 1.0
    return sig


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

    fade_len = min(int(round(0.010 * sample_rate)), n // 10)
    if fade_len > 1:
        window = 0.5 * (1.0 - np.cos(np.pi * np.arange(fade_len) / fade_len))
        sweep[:fade_len] *= window
        sweep[-fade_len:] *= window[::-1]

    amp = dbfs_to_linear(peak_dbfs)
    sweep_scaled = sweep * amp

    env = np.exp(-t * ln_ratio / duration_s)
    inv = sweep[::-1] * env
    norm = float(np.sum(sweep_scaled * sweep * env)) / float(n)
    if norm > 0.0:
        inv /= norm * float(n)
    return sweep_scaled.astype(np.float32), inv.astype(np.float32)


def generate_multitone(
    sample_rate: int,
    peak_dbfs: float = -12.0,
    n_fft: int = COHERENT_N_FFT,
    num_tones: int = 31,
    f_min: float = 25.0,
    f_max: float = 18000.0,
) -> np.ndarray:
    """Broadband 31-tone Newman-phase multitone (for level/LUFS/linear FR only, NOT THD/ASR)."""
    f_high = min(f_max, 0.45 * sample_rate)
    freqs_target = np.geomspace(f_min, f_high, num_tones)
    bins: list[int] = []
    for f in freqs_target:
        k = odd_coherent_bin(float(f), sample_rate, n_fft)
        if k < n_fft // 2 and k not in bins:
            bins.append(k)

    idx_arr = np.arange(n_fft, dtype=np.float64)
    sig = np.zeros(n_fft, dtype=np.float64)
    m_total = len(bins)
    for tone_i, k in enumerate(bins):
        phi = (math.pi * (tone_i ** 2)) / float(m_total)
        sig += np.cos((2.0 * math.pi * float(k) / float(n_fft)) * idx_arr + phi)

    peak = float(np.max(np.abs(sig)))
    if peak > 0.0:
        sig = sig * (dbfs_to_linear(peak_dbfs) / peak)
    return sig.astype(np.float32)


def _splitmix64_uniform(count: int, seed: int) -> tuple[np.ndarray, int]:
    """NumPy-version-independent deterministic PRNG returning values in [-1.0, 1.0)."""
    out = np.empty(count, dtype=np.float64)
    state = seed & 0xFFFFFFFFFFFFFFFF
    for i in range(count):
        state = (state + 0x9E3779B97F4A7C15) & 0xFFFFFFFFFFFFFFFF
        z = state
        z = ((z ^ (z >> 30)) * 0xBF58476D1CE4E5B9) & 0xFFFFFFFFFFFFFFFF
        z = ((z ^ (z >> 27)) * 0x94D049BB133111EB) & 0xFFFFFFFFFFFFFFFF
        z = (z ^ (z >> 31)) & 0xFFFFFFFFFFFFFFFF
        unit = (z >> 11) * (1.0 / (1 << 53))
        out[i] = 2.0 * unit - 1.0
    return out, state


def _karplus_strong_note(
    sample_rate: int,
    freq_hz: float,
    duration_s: float,
    damping: float,
    brightness: float,
    pick_pos: float,
    seed_state: int,
) -> tuple[np.ndarray, int]:
    """Synthesize a single plucked guitar string note using extended Karplus-Strong."""
    n_samples = int(round(sample_rate * duration_s))
    delay_len = max(2, int(round(sample_rate / freq_hz)))

    burst, next_seed = _splitmix64_uniform(delay_len, seed_state)
    burst -= np.mean(burst)
    pick_comb = max(1, int(round(pick_pos * delay_len)))
    buf = burst.copy()
    for i in range(pick_comb, delay_len):
        buf[i] -= burst[i - pick_comb]

    for i in range(1, delay_len):
        buf[i] = brightness * buf[i] + (1.0 - brightness) * buf[i - 1]

    out = np.zeros(n_samples, dtype=np.float64)
    prev = 0.0
    for i in range(n_samples):
        idx = i % delay_len
        cur = buf[idx]
        out[i] = cur
        averaged = 0.5 * (cur + prev)
        buf[idx] = damping * ((1.0 - brightness) * cur + brightness * averaged)
        prev = cur
    return out, next_seed


def generate_karplus_strong_di(
    sample_rate: int,
    peak_dbfs: float = -12.0,
    seed: int = 42,
) -> np.ndarray:
    """Deterministic synthetic guitar DI phrase (~4.0 s) covering chugs, chords, and lead."""
    seed_state = seed
    total_s = 4.0
    total_n = int(round(sample_rate * total_s))
    mix = np.zeros(total_n, dtype=np.float64)

    def add_note(start_s: float, freq: float, dur_s: float, damp: float, bright: float, gain: float) -> None:
        nonlocal seed_state
        start_idx = int(round(start_s * sample_rate))
        if start_idx >= total_n:
            return
        note, seed_state = _karplus_strong_note(sample_rate, freq, dur_s, damp, bright, 0.18, seed_state)
        note *= gain
        end_idx = min(total_n, start_idx + note.size)
        mix[start_idx:end_idx] += note[: end_idx - start_idx]

    # Section 1 (0.0 - 1.0 s): Palm-muted E5 power-chord (E2 + B2) 16th-note chugs
    for step, t0 in enumerate(np.linspace(0.02, 0.86, 7)):
        damp = 0.965 if step % 2 == 0 else 0.992
        add_note(float(t0), 82.41, 0.22, damp, 0.45, 1.0)
        add_note(float(t0) + 0.003, 123.47, 0.22, damp, 0.45, 0.8)

    # Section 2 (1.0 - 2.4 s): Clean/crunch arpeggiated E9 / Asus2 chord
    arp_notes = [
        (1.05, 82.41, 1.3),
        (1.20, 123.47, 1.15),
        (1.35, 185.00, 1.00),
        (1.50, 207.65, 0.85),
        (1.65, 246.94, 0.70),
        (1.80, 329.63, 0.55),
    ]
    for t0, freq, dur in arp_notes:
        add_note(t0, freq, dur, 0.9965, 0.30, 0.75)

    # Section 3 (2.4 - 4.0 s): Sustained high lead notes (B4 -> E5)
    add_note(2.40, 493.88, 0.75, 0.9985, 0.25, 0.95)
    add_note(3.10, 659.25, 0.88, 0.9988, 0.22, 1.00)

    mix -= np.mean(mix)
    peak = float(np.max(np.abs(mix)))
    if peak > 0.0:
        mix *= dbfs_to_linear(peak_dbfs) / peak
    return mix.astype(np.float32)


def generate_all(
    out_dir: Path,
    rates: tuple[int, ...],
    ess_seconds: float,
    verify: bool,
    manifest_out: Path | None = None,
    manifest_check: Path | None = None,
) -> int:
    generated_files: list[Path] = []
    for sr in rates:
        imp = generate_impulse(sr, duration_s=1.0)
        imp_path = out_dir / f"impulse_{sr}.wav"
        write_wav_float32(imp_path, sr, imp)
        generated_files.append(imp_path)

        ess, ess_inv = generate_ess(sr, duration_s=ess_seconds, peak_dbfs=-1.0)
        ess_path = out_dir / f"ess_{sr}.wav"
        inv_path = out_dir / f"ess_inv_{sr}.wav"
        write_wav_float32(ess_path, sr, ess)
        write_wav_float32(inv_path, sr, ess_inv)
        generated_files.extend([ess_path, inv_path])

        for f0 in COHERENT_NOMINAL_FREQS_HZ:
            for level_tag, level_db in COHERENT_LEVELS:
                k_bin, coh = generate_coherent_tone(sr, float(f0), level_db)
                coh_path = out_dir / f"coherent_{f0}hz_{level_tag}_{sr}.wav"
                write_wav_float32(coh_path, sr, coh)
                generated_files.append(coh_path)
                if verify:
                    r_sr, r_coh = read_wav_float32(coh_path)
                    _require(r_sr == sr, f"{coh_path}: sr mismatch {r_sr} != {sr}")
                    _require(
                        r_coh.size == COHERENT_WARMUP_SAMPLES + COHERENT_N_FFT,
                        f"{coh_path}: length {r_coh.size} != {COHERENT_WARMUP_SAMPLES + COHERENT_N_FFT}",
                    )
                    _require(k_bin % 2 == 1, f"{coh_path}: bin {k_bin} is not odd")
                    # Check rectangular-window FFT concentration on bin k_bin in the post-warmup slice
                    win_slice = r_coh[COHERENT_WARMUP_SAMPLES:].astype(np.float64)
                    spec = np.abs(np.fft.rfft(win_slice))
                    fund_mag = float(spec[k_bin])
                    spec[k_bin] = 0.0
                    max_spur_db = 20.0 * math.log10(max(float(np.max(spec)), 1e-20) / fund_mag)
                    _require(max_spur_db <= -130.0, f"{coh_path}: spur {max_spur_db:.1f} dBc > -130 dBc")

        mt12 = generate_multitone(sr, peak_dbfs=-12.0)
        mt18 = generate_multitone(sr, peak_dbfs=-18.0)
        mt12_path = out_dir / f"multitone_m12dbfs_{sr}.wav"
        mt18_path = out_dir / f"multitone_m18dbfs_{sr}.wav"
        write_wav_float32(mt12_path, sr, mt12)
        write_wav_float32(mt18_path, sr, mt18)
        generated_files.extend([mt12_path, mt18_path])

        ks = generate_karplus_strong_di(sr, peak_dbfs=-12.0)
        ks_path = out_dir / f"ks_di_{sr}.wav"
        write_wav_float32(ks_path, sr, ks)
        generated_files.append(ks_path)

        if verify:
            r_sr, r_imp = read_wav_float32(imp_path)
            _require(r_sr == sr and r_imp[0] == 1.0 and float(np.sum(np.abs(r_imp[1:]))) == 0.0, "impulse check")
            _, r_ks = read_wav_float32(ks_path)
            peak_db = 20.0 * math.log10(float(np.max(np.abs(r_ks))))
            _require(abs(peak_db - (-12.0)) < 0.01, f"KS DI peak mismatch: {peak_db} dBFS")
            _, r_mt12 = read_wav_float32(mt12_path)
            mt12_db = 20.0 * math.log10(float(np.max(np.abs(r_mt12))))
            _require(abs(mt12_db - (-12.0)) < 0.01, f"Multitone -12 peak mismatch: {mt12_db} dBFS")
            _, r_mt18 = read_wav_float32(mt18_path)
            mt18_db = 20.0 * math.log10(float(np.max(np.abs(r_mt18))))
            _require(abs(mt18_db - (-18.0)) < 0.01, f"Multitone -18 peak mismatch: {mt18_db} dBFS")

    hashes = {p.name: hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(generated_files)}
    if manifest_out is not None:
        manifest_out.parent.mkdir(parents=True, exist_ok=True)
        lines = [f"{sha}  {name}\n" for name, sha in sorted(hashes.items())]
        manifest_out.write_text("".join(lines), encoding="utf-8")

    if manifest_check is not None:
        expected: dict[str, str] = {}
        for line in manifest_check.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line and not line.startswith("#"):
                sha, name = line.split(None, 1)
                expected[name.strip()] = sha.strip()
        for name, exp_sha in expected.items():
            actual_sha = hashes.get(name)
            _require(actual_sha == exp_sha, f"manifest hash mismatch for {name}: got {actual_sha}, expected {exp_sha}")

    print(f"testsignals: generated {len(generated_files)} files across {len(rates)} sample rate(s) in {out_dir}")
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
    parser.add_argument("--verify", action="store_true", help="Read back and verify peak levels, odd bins, and impulse.")
    parser.add_argument("--write-manifest", type=Path, default=None, help="Write SHA-256 manifest of generated WAVs.")
    parser.add_argument("--verify-manifest", type=Path, default=None, help="Verify SHA-256 hashes against manifest file.")
    args = parser.parse_args()
    return generate_all(
        args.out_dir,
        tuple(args.rates),
        args.ess_seconds,
        args.verify,
        args.write_manifest,
        args.verify_manifest,
    )


if __name__ == "__main__":
    sys.exit(main())
