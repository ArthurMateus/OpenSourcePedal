# `tools/testsignals`: Synthetic DSP Test-Signal Generator

Deterministic, licence-clean (**CC0 1.0** output, **AGPL-3.0-or-later** code) test-signal generator for OpenSourcePedal unit tests, RT checks, and golden-render regressions (Issue #48, [`docs/research/redistributable-assets.md`](../../docs/research/redistributable-assets.md) §3.5, and [`docs/adr/0007-test-strategy.md`](../../docs/adr/0007-test-strategy.md)).

## Generated Stimuli (per sample rate: 44.1, 48, 88.2, 96 kHz)

All files are written as mono **32-bit IEEE float** `.wav` (`FORMAT_IEEE_FLOAT = 3`):

| File pattern | Description | Peak Level | Default Duration / Length |
|---|---|---|---|
| `impulse_<sr>.wav` | Unit impulse ($x[0] = 1.0$, zeros elsewhere) for IR and `getLatencySamples()` tests | $0.0\text{ dBFS}$ | $1.0\text{ s}$ |
| `ess_<sr>.wav` | Farina (2000) Exponential Sine Sweep ($20\text{ Hz} \to \min(20\text{ kHz}, 0.49 f_s)$) with $10\text{ ms}$ raised-cosine fade-in/out | $-1.0\text{ dBFS}$ | $10.0\text{ s}$ (configurable via `--ess-seconds`) |
| `ess_inv_<sr>.wav` | Matched time-reversed, $-6\text{ dB/oct}$ energy-normalized deconvolution inverse filter for `ess_<sr>.wav` | Energy-normalized | Matches `ess_<sr>.wav` |
| `multitone_m12dbfs_<sr>.wav` | Coherent odd-bin ($N_{\text{FFT}} = 65\,536$) 31-tone log-spaced multitone ($25\text{ Hz}\text{–}18\text{ kHz}$) with Newman crest-minimizing phases | $-12.0\text{ dBFS}$ | $65\,536\text{ samples}$ |
| `multitone_m18dbfs_<sr>.wav` | Same coherent 31-tone odd-bin multitone normalized to $-18.0\text{ dBFS}$ peak | $-18.0\text{ dBFS}$ | $65\,536\text{ samples}$ |
| `ks_di_<sr>.wav` | Deterministic extended Karplus-Strong synthetic guitar DI phrase (palm-muted E5 chugs, arpeggiated chord, and sustained high lead notes) | $-12.0\text{ dBFS}$ | $4.0\text{ s}$ |

## Usage

```powershell
# Generate all 6 stimuli across 44.1, 48, 88.2, and 96 kHz and verify output levels
python tools/testsignals/generate.py --out-dir build/testsignals --verify

# Fast 2-second sweep at 48 kHz only
python tools/testsignals/generate.py --out-dir build/testsignals --rates 48000 --ess-seconds 2.0 --verify
```
