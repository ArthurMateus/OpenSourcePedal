#!/usr/bin/env python3
"""Tone match front end (ADR-0008): song file or YouTube link + your DI -> OpenSourcePedal preset.

It resolves the reference, optionally isolates the guitar, and runs osp-tonematch (the C++ matcher, built with the
rest of the project). Standard library only. External tools are used only when you have installed them yourself:

  yt-dlp + ffmpeg   for --ref <URL>     https://github.com/yt-dlp/yt-dlp (Unlicense), https://ffmpeg.org
  demucs            for --stem          https://github.com/facebookresearch/demucs (MIT); model htdemucs_6s has a
                                        guitar stem. `pip install demucs`, a GPU helps but is not required.

Only download recordings you have the right to download. Downloads and stems go to --work (default
build/tonematch), which is git-ignored; nothing here is ever committed or bundled.

Examples:
  python tools/tonematch/tonematch.py --ref song.mp3 --di my-di.wav --start 32 --end 58
  python tools/tonematch/tonematch.py --ref "https://www.youtube.com/watch?v=..." --di my-di.wav --stem
  python tools/tonematch/tonematch.py --ref song.mp3 --di my-di.wav --stem --tones --render preview.wav
"""
from __future__ import annotations

import argparse
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
EXE_CANDIDATES = [
    ROOT / "build" / "release" / "tools" / "osp-tonematch.exe",
    ROOT / "build" / "release" / "tools" / "osp-tonematch",
    ROOT / "build" / "linux" / "tools" / "osp-tonematch",
]


def fail(msg: str) -> None:
    print(f"tonematch: {msg}", file=sys.stderr)
    sys.exit(1)


def find_exe(explicit: str | None) -> Path:
    if explicit:
        if not Path(explicit).exists():
            fail(f"--exe {explicit} does not exist")
        return Path(explicit)
    for p in EXE_CANDIDATES:
        if p.exists():
            return p
    found = shutil.which("osp-tonematch")
    if found:
        return Path(found)
    fail("osp-tonematch not found: build the project first (cmake --build --preset release) or pass --exe")
    raise AssertionError  # unreachable


def is_url(ref: str) -> bool:
    return ref.startswith("http://") or ref.startswith("https://")


def download(url: str, work: Path) -> Path:
    if shutil.which("yt-dlp") is None:
        fail("a link needs yt-dlp and ffmpeg on PATH (pip install yt-dlp; ffmpeg from https://ffmpeg.org)")
    key = hashlib.sha256(url.encode()).hexdigest()[:16]
    out = work / f"ref-{key}.wav"
    if out.exists():
        print(f"using cached download {out}")
        return out
    print("Only download recordings you have the right to download.")
    print(f"downloading audio with yt-dlp -> {out}")
    cmd = ["yt-dlp", "--no-playlist", "-x", "--audio-format", "wav", "-o", str(work / f"ref-{key}.%(ext)s"), url]
    if subprocess.run(cmd).returncode != 0 or not out.exists():
        fail("yt-dlp failed (is ffmpeg installed? yt-dlp needs it to convert to WAV)")
    return out


def to_wav(ref: Path, work: Path) -> Path:
    """osp-tonematch reads WAV and MP3 itself; other formats (m4a, flac, ogg, webm) go through ffmpeg."""
    if ref.suffix.lower() in (".wav", ".mp3"):
        return ref
    if shutil.which("ffmpeg") is None:
        fail(f"{ref.suffix} needs ffmpeg on PATH (or convert the file to WAV/MP3 first)")
    out = work / (ref.stem + ".wav")
    if subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-i", str(ref), str(out)]).returncode != 0:
        fail(f"ffmpeg could not convert {ref}")
    return out


def file_key(path: Path) -> str:
    """Content hash, so two different songs with the same file name never share a cached stem (#120 review)."""
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()[:16]


def demucs_command() -> list[str]:
    """Demucs as a module of this Python, or a `demucs` launcher on PATH (pipx, conda)."""
    try:
        import demucs  # noqa: F401  (only checks that it is installed)

        return [sys.executable, "-m", "demucs"]
    except ImportError:
        exe = shutil.which("demucs")
        if exe:
            return [exe]
    fail("--stem needs Demucs: pip install demucs")
    raise AssertionError  # unreachable


def separate_guitar(ref: Path, work: Path) -> Path:
    cmd = demucs_command()
    key = file_key(ref)
    out_dir = work / "stems" / key
    stem = out_dir / "htdemucs_6s" / ref.stem / "guitar.wav"
    if stem.exists():
        print(f"using cached guitar stem {stem}")
        return stem
    print("separating the guitar stem with Demucs htdemucs_6s (this can take a few minutes on a CPU; on first use")
    print("Demucs downloads its model weights into its own cache, outside this repository)")
    cmd += ["-n", "htdemucs_6s", "--two-stems", "guitar", "-o", str(out_dir), str(ref)]
    if subprocess.run(cmd).returncode != 0 or not stem.exists():
        fail("Demucs failed")
    return stem


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--ref", required=True, help="song file (wav, mp3, m4a, flac, ...) or a YouTube link")
    ap.add_argument("--di", required=True, help="your dry guitar recording (WAV), ideally the same part")
    ap.add_argument("--out", help="preset JSON to write (default: <work>/<name>.osp.json)")
    ap.add_argument("--render", help="also write your DI through the matched setting (WAV)")
    ap.add_argument("--name", default="Tone match", help="preset name")
    ap.add_argument("--start", type=float, help="reference section start, seconds")
    ap.add_argument("--end", type=float, help="reference section end, seconds")
    ap.add_argument("--source", default="auto", choices=["auto", "mono", "mid", "side", "left", "right"])
    ap.add_argument("--stem", action="store_true", help="isolate the guitar with Demucs first (recommended)")
    ap.add_argument("--no-hpss", action="store_true", help="keep percussive energy (an already clean guitar stem)")
    ap.add_argument("--tones", action="store_true",
                    help="one preset per guitar part and tone over the whole song (guitar 1 tone 1, ...); best with --stem")
    ap.add_argument("--ir-dir", help="cab IR folder (default: the bundled assets/cab)")
    ap.add_argument("--work", default=str(ROOT / "build" / "tonematch"), help="download/stem cache folder")
    ap.add_argument("--exe", help="path to osp-tonematch")
    a = ap.parse_args()

    work = Path(a.work)
    work.mkdir(parents=True, exist_ok=True)
    exe = find_exe(a.exe)

    ref = download(a.ref, work) if is_url(a.ref) else Path(a.ref)
    if not ref.exists():
        fail(f"no such file: {ref}")
    ref = to_wav(ref, work)
    if a.stem:
        ref = separate_guitar(ref, work)

    safe = "".join(c if c.isalnum() or c in "-_" else "-" for c in a.name).strip("-") or "tone-match"
    out = Path(a.out) if a.out else work / f"{safe}.osp.json"
    cmd = [str(exe), "--ref", str(ref), "--di", a.di, "--out", str(out), "--name", a.name, "--source", a.source]
    if a.start is not None:
        cmd += ["--start", str(a.start)]
    if a.end is not None:
        cmd += ["--end", str(a.end)]
    if a.ir_dir:
        cmd += ["--ir-dir", a.ir_dir]
    if a.render:
        cmd += ["--render", a.render]
    if a.no_hpss:
        cmd += ["--no-hpss"]
    if a.tones:
        cmd += ["--tones"]
    return subprocess.run(cmd).returncode


if __name__ == "__main__":
    sys.exit(main())
