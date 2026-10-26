#!/usr/bin/env python3
"""Synthetic test matrix for osp-tonematch (ADR-0008): where does the tone match break down?

Every reference is made here with numpy/scipy rigs that share no code with dsp-core (different clippers, staging and
filters), so the matcher has to approximate tones it cannot reproduce exactly, as with a real record:

  rigs     clean, edge, crunch (TS-style mid push), lead (2-stage amp), fuzz (asymmetric), metal (2-stage + scoop)
  EQs      flat, mid (+6 dB at 800 Hz), dark (-8 dB shelf above 3 kHz); applied after the cab
  cabs     the 7 bundled CC0 IRs, cycled over the runs (the matcher searches the same 7)
  guitars  A = bundled DI (assets/di, 44.1 kHz), B = Karplus-Strong DI (tools/testsignals, 48 kHz)
  modes    same    reference = rig(guitar), DI = the same guitar
           cross   reference = rig(A), DI = B and the other way round: the "your guitar" case
           mix     stereo mix: rig(A) double-tracked L/R + bass, kick, snare, hats, a centred synthetic vocal

The score compares the matched render with the oracle: the player's own DI through the hidden rig, the best any
match could do. Both are measured here with an independent LTAS / whitened-flatness implementation (no HPSS, since
both are solo guitar).

  python tools/tonematch/matrix.py --exe build/release/tools/osp-tonematch.exe --di-b <ks_di_48000.wav> --out <dir>
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import re
import subprocess
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np
from scipy import signal
from scipy.io import wavfile

ROOT = Path(__file__).resolve().parents[2]
CABS = sorted((ROOT / "assets" / "cab").glob("*.wav"))

# --- I/O ----------------------------------------------------------------------------------------------------------


def read(path: Path) -> tuple[np.ndarray, int]:
    fs, x = wavfile.read(path)
    if x.dtype == np.int16:
        x = x / 32768.0
    elif x.dtype == np.int32:
        x = x / 2147483648.0
    x = np.asarray(x, dtype=np.float64)
    return x, fs


def mono(x: np.ndarray) -> np.ndarray:
    return x if x.ndim == 1 else x[:, 0]


def write(path: Path, x: np.ndarray, fs: int) -> None:
    wavfile.write(path, fs, np.asarray(x, dtype=np.float32))


# --- Reference rigs (independent of dsp-core) ---------------------------------------------------------------------


def hp1(x, fc, fs):
    b, a = signal.butter(1, fc / (fs / 2), "highpass")
    return signal.lfilter(b, a, x)


def lp1(x, fc, fs):
    b, a = signal.butter(1, fc / (fs / 2), "lowpass")
    return signal.lfilter(b, a, x)


def peak(x, fc, gain_db, q, fs):
    """RBJ cookbook peaking EQ."""
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * fc / fs
    al = np.sin(w) / (2 * q)
    b = [1 + al * A, -2 * np.cos(w), 1 - al * A]
    a = [1 + al / A, -2 * np.cos(w), 1 - al / A]
    return signal.lfilter(b, a, x)


def high_shelf(x, fc, gain_db, fs):
    """RBJ cookbook high shelf, S = 1."""
    A = 10 ** (gain_db / 40)
    w = 2 * np.pi * fc / fs
    al = np.sin(w) / 2 * np.sqrt(2)
    c = np.cos(w)
    b = [A * ((A + 1) + (A - 1) * c + 2 * np.sqrt(A) * al), -2 * A * ((A - 1) + (A + 1) * c),
         A * ((A + 1) + (A - 1) * c - 2 * np.sqrt(A) * al)]
    a = [(A + 1) - (A - 1) * c + 2 * np.sqrt(A) * al, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - 2 * np.sqrt(A) * al]
    return signal.lfilter(b, a, x)


def amp_stage(x, fs, rig):
    x = x / (np.abs(x).max() + 1e-12) * 0.25  # -12 dBFS peak, like the matcher's DI normalisation
    if rig == "clean":
        return 2.0 * x
    if rig == "edge":
        return np.tanh(3.0 * x)
    if rig == "crunch":  # mid push before a soft clipper
        return lp1(np.tanh(12.0 * (x + 2.0 * hp1(x, 720, fs))), 5000, fs)
    if rig == "lead":  # preamp stage into a softer power stage
        y = lp1(np.tanh(25.0 * hp1(x, 150, fs)), 7000, fs)
        return np.tanh(3.0 * y)
    if rig == "fuzz":
        return lp1(np.clip(60.0 * x + 0.3, -1.0, 0.6) - 0.3, 4000, fs)
    if rig == "metal":  # tight, very high gain, scooped tone stack, power stage
        y = lp1(np.tanh(60.0 * hp1(hp1(x, 200, fs), 200, fs)), 8000, fs)
        y = peak(y, 650, -9.0, 0.8, fs)
        return np.tanh(2.0 * y)
    raise ValueError(rig)


def cab(x, fs, idx):
    ir, irfs = read(CABS[idx])
    ir = mono(ir)
    if irfs != fs:
        g = np.gcd(int(irfs), int(fs))
        ir = signal.resample_poly(ir, fs // g, irfs // g)
    return signal.fftconvolve(x, ir[: fs // 2])[: len(x)]


def post_eq(x, fs, eq):
    if eq == "mid":
        return peak(x, 800, 6.0, 1.0, fs)
    if eq == "dark":
        return high_shelf(x, 3000, -8.0, fs)
    return x


def rig(x, fs, r, c, eq):
    return post_eq(cab(amp_stage(x, fs, r), fs, c), fs, eq)


# --- Synthetic mix --------------------------------------------------------------------------------------------------


def band_noise(n, lo, hi, fs, rng):
    b, a = signal.butter(2, [lo / (fs / 2), hi / (fs / 2)], "bandpass")
    return signal.lfilter(b, a, rng.standard_normal(n))


def mix(di, fs, r, c, eq):
    rng = np.random.default_rng(7)
    n = len(di)
    left = rig(di, fs, r, c, eq)
    right = rig(np.concatenate([np.zeros(int(0.009 * fs)), di])[:n] * 0.95, fs, r, c, eq)  # second "take"
    g = 1.0 / max(np.abs(left).max(), 1e-9)
    left, right = left * g, right * g
    t = np.arange(n) / fs
    beat = (t * 2.0) % 1.0  # 120 bpm, 2 beats per second
    notes = np.where((t % 2.0) < 1.0, 41.2, 55.0)
    bass = 0.5 * np.sin(2 * np.pi * np.cumsum(notes) / fs) + 0.15 * np.sin(4 * np.pi * np.cumsum(notes) / fs)
    kick = np.zeros(n)
    snare = np.zeros(n)
    hats = np.zeros(n)
    for k in range(0, n, fs // 2):
        m = min(3000, n - k)
        env = np.exp(-np.arange(m) / (0.02 * fs))
        kick[k : k + m] += np.sin(2 * np.pi * 55 * np.arange(m) / fs) * env * 0.9
        s = k + fs // 4
        if s < n:
            m2 = min(4000, n - s)
            snare[s : s + m2] += band_noise(m2, 200, 6000, fs, rng) * np.exp(-np.arange(m2) / (0.05 * fs)) * 0.6
    for k in range(0, n, fs // 8):
        m = min(800, n - k)
        hats[k : k + m] += band_noise(m, 7000, min(16000, fs / 2 - 100), fs, rng) * np.exp(-np.arange(m) / (0.008 * fs)) * 0.3
    # A centred "vocal": a sawtooth melody through two formant band passes, with vibrato and gaps.
    f0 = np.where((t % 4.0) < 2.0, 220.0, 262.0) * (1 + 0.01 * np.sin(2 * np.pi * 5.5 * t))
    saw = signal.sawtooth(2 * np.pi * np.cumsum(f0) / fs)
    vocal = sum(signal.lfilter(*signal.butter(2, [lo / (fs / 2), hi / (fs / 2)], "bandpass"), saw)
                for lo, hi in ((500, 900), (1100, 1600)))
    vocal *= ((t % 4.0) < 3.2) * 0.5
    centre = 0.7 * bass + 0.6 * (kick + snare) + 0.4 * hats + 0.5 * vocal
    out = np.stack([left + centre, right + centre], axis=1)
    return out / (np.abs(out).max() * 1.05)


# --- Independent measurement ---------------------------------------------------------------------------------------

BAND_HZ = 50.0 * 2 ** (np.arange(34) / 4)
WEIGHT = np.where((BAND_HZ >= 120) & (BAND_HZ <= 6000), 1.0, np.where((BAND_HZ >= 80) & (BAND_HZ <= 10000), 0.5, 0.0))


def bands_db(x, fs):
    f, p = signal.welch(x, fs, nperseg=8192)
    out = np.empty(34)
    for k, fc in enumerate(BAND_HZ):
        sel = (f >= fc * 2 ** -0.125) & (f < fc * 2 ** 0.125)
        out[k] = 10 * np.log10(max(p[sel].mean() if sel.any() else p[np.argmin(abs(f - fc))], 1e-20))
    return out


def residual(a, b):
    d = a - b
    d = d - np.sum(WEIGHT * d) / WEIGHT.sum()
    return float(np.sqrt(np.sum(WEIGHT * d * d) / WEIGHT.sum()))


def flatness(x, fs):
    n = 4096 if fs < 66000 else 8192
    f, _, z = signal.stft(x, fs, nperseg=n, noverlap=3 * n // 4, boundary=None, padded=False)
    p = np.abs(z) ** 2
    e = p.sum(0)
    keep = e > np.percentile(e, 95) * 10 ** -4.5
    p = p[:, keep]
    ltas = p.mean(1, keepdims=True)
    r = np.maximum(p / np.maximum(ltas, 1e-30), 1e-12)
    out = []
    for lo in (500, 1000, 2000, 4000):
        sel = (f >= lo) & (f <= 2 * lo)
        q = r[sel]
        out.append(np.mean(10 * np.log10(np.exp(np.log(q).mean(0)) / q.mean(0))))
    return np.array(out)


# --- Runs ------------------------------------------------------------------------------------------------------------


@dataclass
class Run:
    name: str
    mode: str
    rig: str
    eq: str
    cab: int
    ref_guitar: str
    di_guitar: str
    result: dict = field(default_factory=dict)


def parse(stdout: str) -> dict:
    out = {}
    m = re.search(r"^\s+cab\s+(\S+)$", stdout, re.M)
    out["cab_found"] = m.group(1) if m else "?"
    m = re.search(r"drive1\s+(.+)", stdout)
    out["drive"] = m.group(1).strip() if m else "?"
    m = re.search(r"spectral residual: ([\d.]+) dB \(.*?: ([\d.]+) dB\)", stdout)
    if m:
        out["cli_residual"], out["cli_unmatched"] = float(m.group(1)), float(m.group(2))
    m = re.search(r"source (\w+)", stdout)
    out["source"] = m.group(1) if m else "?"
    return out


def execute(run: Run, guitars, exe: Path, work: Path) -> Run:
    d = work / run.name
    d.mkdir(parents=True, exist_ok=True)
    ref_x, ref_fs = guitars[run.ref_guitar]
    di_x, di_fs = guitars[run.di_guitar]
    if run.mode == "mix":
        ref = mix(ref_x, ref_fs, run.rig, run.cab, run.eq)
    else:
        ref = rig(ref_x, ref_fs, run.rig, run.cab, run.eq)
        ref = ref / (np.abs(ref).max() * 1.05)
    write(d / "ref.wav", ref, ref_fs)
    write(d / "di.wav", di_x, di_fs)
    oracle = rig(di_x, di_fs, run.rig, run.cab, run.eq)
    cmd = [str(exe), "--ref", str(d / "ref.wav"), "--di", str(d / "di.wav"), "--out", str(d / "p.osp.json"),
           "--render", str(d / "matched.wav"), "--name", run.name]
    res = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    (d / "stdout.txt").write_text(res.stdout + res.stderr, encoding="utf-8")
    if res.returncode != 0:
        run.result = {"error": res.stderr.strip()[-300:]}
        return run
    got, _ = read(d / "matched.wav")
    got = mono(got)
    r = parse(res.stdout)
    r["cab_hidden"] = CABS[run.cab].stem
    r["tone_residual"] = residual(bands_db(got, di_fs), bands_db(oracle, di_fs))
    r["unmatched_residual"] = residual(bands_db(di_x, di_fs), bands_db(oracle, di_fs))
    r["flatness_diff"] = float(np.sqrt(np.mean((flatness(got, di_fs) - flatness(oracle, di_fs)) ** 2)))
    r["flatness_oracle"] = [round(v, 2) for v in flatness(oracle, di_fs)]
    r["flatness_got"] = [round(v, 2) for v in flatness(got, di_fs)]
    run.result = r
    return run


def grade(r: dict) -> str:
    if "error" in r:
        return "ERROR"
    if r["tone_residual"] <= 1.5 and r["flatness_diff"] <= 1.0:
        return "good"
    if r["tone_residual"] <= 3.0 and r["flatness_diff"] <= 2.0:
        return "fair"
    return "BREAKS"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--exe", required=True)
    ap.add_argument("--di-b", required=True, help="second guitar DI (tools/testsignals ks_di_48000.wav)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--only", help="regex on run names")
    a = ap.parse_args()

    guitars = {"A": tuple(read(ROOT / "assets" / "di" / "strum-arp-01.wav")), "B": tuple(read(Path(a.di_b)))}
    guitars = {k: (mono(x), fs) for k, (x, fs) in guitars.items()}
    rigs = ["clean", "edge", "crunch", "lead", "fuzz", "metal"]
    runs: list[Run] = []
    i = 0
    for g in ("A", "B"):
        for r in rigs:
            for eq in ("flat", "mid", "dark"):
                runs.append(Run(f"same-{g}-{r}-{eq}", "same", r, eq, i % len(CABS), g, g))
                i += 1
    for r in rigs:
        for ref_g, di_g in (("A", "B"), ("B", "A")):
            runs.append(Run(f"cross-{ref_g}to{di_g}-{r}", "cross", r, "flat", i % len(CABS), ref_g, di_g))
            i += 1
    for r in rigs:
        runs.append(Run(f"mix-A-{r}", "mix", r, "flat", i % len(CABS), "A", "A"))
        i += 1
    if a.only:
        runs = [r for r in runs if re.search(a.only, r.name)]

    work = Path(a.out)
    work.mkdir(parents=True, exist_ok=True)
    with cf.ThreadPoolExecutor(a.jobs) as pool:
        done = list(pool.map(lambda r: execute(r, guitars, Path(a.exe), work), runs))

    rows = []
    for r in done:
        rows.append({"name": r.name, "mode": r.mode, "rig": r.rig, "eq": r.eq, "grade": grade(r.result), **r.result})
    (work / "results.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")
    print(f"{'run':28} {'grade':6} {'tone':>5} {'unm':>5} {'flat':>5}  cab hidden/found      drive")
    for row in rows:
        if "error" in row:
            print(f"{row['name']:28} ERROR  {row['error']}")
            continue
        hit = "=" if row["cab_hidden"] == row["cab_found"] else "x"
        print(f"{row['name']:28} {row['grade']:6} {row['tone_residual']:5.2f} {row['unmatched_residual']:5.2f} "
              f"{row['flatness_diff']:5.2f}  {row['cab_hidden']:>6} {hit} {row['cab_found']:<8} {row['drive']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
