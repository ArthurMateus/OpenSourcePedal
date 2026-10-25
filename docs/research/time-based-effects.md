# Research: Reverb, Shimmer, Delay, and Modulation Algorithms

- **Issue:** #11
- **Author:** `gemini/researcher` (Corvid)
- **Date:** 2026-10-25 (all sources accessed 2026-10-25)

---

## 1. Summary

1. **Reverb architecture (`dsp-core`):** Implement two complementary, zero-allocation C++20 topologies inside a unified Reverb block:
   - **Room & Hall modes:** An **8-channel Feedback Delay Network (FDN)** (Jot 1991; Luff ADC 2021) preceded by a 4-stage multi-channel allpass diffuser (Hadamard + polarity flips) and a short Moorer-style early-reflection tap network for Room mode, using a $O(N)$ **Householder** feedback matrix ($I - \frac{2}{N}\mathbf{1}\mathbf{1}^T$) and per-line 1st-order shelving filters for independent low/high $RT_{60}(\omega)$ control.
   - **Plate mode:** The classic **Dattorro (1997) figure-eight plate** topology (4 input allpass diffusers + cross-coupled modulated tank with 2 modulated allpasses, 4 delay lines, and 1-pole damping).
2. **Shimmer / "Dreamscape" mode:** Place a **dual-grain Hann-windowed pitch shifter** (+12 semitones octave-up, plus optional -12st sub-octave or +7st fifth blend) followed by a 2-pole SVF low-pass ($2.5\text{–}6\text{ kHz}$), a $20\text{ Hz}$ DC blocker, $\tanh$ soft limiter, and 2 modulated allpass diffusers **inside the FDN feedback loop** (inspired by Mutable Instruments *Oliverb/Clouds*, **MIT**, and `ValdemarOrn/CloudSeed`, **MIT**). This avoids FFT framing overhead and eliminates metallic build-up.
3. **Delay architecture (`dsp-core`):** Implement a single stereo Delay block with three voicing modes (**Digital**, **Analog BBD**, **Tape**), **Tempo Sync** ($1/1 \dots 1/16$ + dotted/triplet), and **Input-Envelope Ducking**:
   - **Digital:** Clean stereo / ping-pong with feedback HPF/LPF and dual-head crossfade on large delay jumps.
   - **Analog (BBD):** 2nd-order SVF anti-aliasing/reconstruction low-pass ($f_c \approx 1.8\text{–}3.5\text{ kHz}$, scaling inversely with delay time), soft asymmetrical saturation, and gentle triangle clock modulation.
   - **Tape:** Dual-LFO + filtered pink-noise **Wow ($0.5\text{–}2.5\text{ Hz}$) & Flutter ($6\text{–}15\text{ Hz}$)**, $\tanh$ feedback saturation, and a resonant **head-bump** peaking filter ($80\text{ Hz}$, $+3\text{ dB}$, $Q=1.2$).
4. **Delay-line interpolation:** Use **4-point Cubic Hermite (Catmull-Rom) / 3rd-order Lagrange** interpolation (`Signalsmith-Audio/dsp`, **MIT**, or in-tree ~15 LOC) for all continuously modulated delay taps (Chorus, Flanger, Analog/Tape Delay, modulated FDN taps), and **1st-order Thiran allpass** interpolation only where flat Nyquist magnitude is required on static/slowly varying tank lengths.
5. **Modulation block (`dsp-core`):** Provide **Chorus** (stereo quadrature 2/4-voice modulated delay, $7\text{–}20\text{ ms}$), **Flanger** ($0.2\text{–}8\text{ ms}$ with optional $2.5\text{ ms}$ dry-reference offset for **Through-Zero Flanging**), **Phaser** (cascade of $4/6/8$ Zero-Delay-Feedback TPT 1st-order allpass stages swept exponentially from $120\text{ Hz}$ to $3.2\text{ kHz}$), and **Tremolo / Vibrato** (bias + Linkwitz-Riley harmonic tremolo).
6. **CPU budget & licensing:** Combined Modulation + Delay + Reverb (including Shimmer) is estimated at **$< 2.5\%$ of one core** at $48\text{ kHz}$ / 64 samples (*unverified* until `osp-bench` runs in Phase 4). All cited open-source references used for design or porting (`Signalsmith-Audio/dsp` MIT, `ValdemarOrn/CloudSeed` MIT, `pichenettes/eurorack` MIT, `airwindows` MIT, `surge-synthesizer/sst-effects` GPLv3, `Chowdhury-DSP/chowdsp_utils` GPLv3) are AGPLv3-compatible; note that `Signalsmith-Audio/reverb-example-code` has no license file and must be treated as literature only.

---

## 2. Findings

### 2.1 Reverb Topologies: FDN, Dattorro Plate, Schroeder/Moorer, Freeverb

| Topology | Core Structure | Echo Density & Sound Quality | Est. CPU ($48\text{ kHz}$) | Strengths / Weaknesses | Primary Sources |
|---|---|---|---|---|---|
| **Jot / Signalsmith 8-ch FDN** | 4-stage multi-ch diffuser $\rightarrow$ 8 delay lines + $8\times 8$ Householder/Hadamard matrix + per-line shelving damping + modulated read taps | High, isotropic, no metallic ringing when modulated | $\sim 0.3\%\text{–}0.6\%$ (*unverified*) | Exact $RT_{60}(\omega)$ parameterisation for **Room**, **Hall**, and **Shimmer/Dreamscape**; scales cleanly to any sample rate. | [Jot 1991 (AES)](https://www.aes.org/e-lib/browse.cfm?elib=5661); [Smith PASP FDN](https://ccrma.stanford.edu/~jos/pasp/Feedback_Delay_Networks_FDN.html); [Luff ADC 2021](https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/) |
| **Dattorro (1997) Plate** | Pre-delay + LPF + 4 series allpass diffusers ($\gamma = 0.75, 0.625$) $\rightarrow$ figure-eight tank (2 modulated allpasses, 4 delays, 2 1-pole dampers, 2 allpasses) | Immediately dense, lush 2D plate wash | $\sim 0.2\%\text{–}0.4\%$ (*unverified*) | Gold standard for **Plate** guitar lead tones; fixed reference sample rate ($29\,761\text{ Hz}$) delay lengths must be scaled by $f_s / 29761$. | [Dattorro 1997 (JAES PDF)](https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf) |
| **Moorer (1979)** | 6–19 tap FIR early-reflection delay line $\rightarrow$ 6 parallel lowpass-feedback combs + series allpass | Realistic early room geometry, dated comb tail | $\sim 0.15\%$ (*unverified*) | Early-reflection FIR tap structure is ideal in front of an 8-ch FDN for **Room** mode; comb tail alone rings on palm-muted guitar chugs. | [Moorer 1979 (JAES)](https://www.aes.org/e-lib/browse.cfm?elib=3190); [Smith PASP Moorer](https://ccrma.stanford.edu/~jos/pasp/Moorer_Reverberator.html) |
| **Schroeder (1962) / Freeverb (2000)** | 8 parallel lowpass-feedback comb filters + 4 series allpass filters per stereo channel | Low initial density, audible flutter on transients | $\sim 0.1\%$ (*unverified*) | Very simple (Public Domain), but inferior transient diffusion and metallic coloration on guitar transients. Not recommended for main reverb. | [Schroeder 1962](https://pubs.aip.org/asa/jasa/article/34/8B/1097/755992); [Smith PASP Freeverb](https://ccrma.stanford.edu/~jos/pasp/Freeverb.html) |

**Room / Hall / Plate Parameterisation:**
- **Jot FDN Decay Gain ($RT_{60}$):** Each delay line $i$ of length $D_i$ samples applies attenuation $g_i(\omega) = 10^{\frac{-3 \cdot D_i}{f_s \cdot RT_{60}(\omega)}}$, realized via a 1st-order low/high-shelf filter so high-frequency decay $RT_{60,\text{hi}} = \alpha_{\text{damp}} \cdot RT_{60,\text{mid}}$ is independent of delay length $D_i$ ([Jot 1991](https://www.aes.org/e-lib/browse.cfm?elib=5661); [Smith PASP](https://ccrma.stanford.edu/~jos/pasp/FDN_Reverberation_Time.html)).
- **Mode Scaling:**
  - **Room:** Early reflection 8-tap cluster ($5\text{–}35\text{ ms}$) + FDN delay lengths $D_i \in [15\text{ ms}, 45\text{ ms}]$ (coprime primes scaled to $f_s$), $RT_{60} \in [0.2\text{ s}, 2.5\text{ s}]$, lower diffusion ($\gamma = 0.55$).
  - **Hall:** 4-stage multi-channel diffuser ($10\text{–}80\text{ ms}$) + FDN delay lengths $D_i \in [35\text{ ms}, 110\text{ ms}]$, $RT_{60} \in [0.8\text{ s}, 8.0\text{ s}]$, high diffusion ($\gamma = 0.70$), slow LFO tap modulation ($0.4\text{ Hz}$, $\pm 12$ samples).
  - **Plate:** Dattorro figure-eight tank with delay lengths scaled by $f_s / 29761$, input diffusion $\gamma_1 = 0.75, \gamma_2 = 0.625$, tank modulation $1.0\text{ Hz}$ ($\pm 16$ samples at $48\text{ kHz}$).

---

### 2.2 Shimmer / "Dreamscape" Ambient Reverb

Modern artist-signature ambient guitar tones ("dreamscape" / shimmer) combine long-decay modulated FDN/plate tails with **pitch-shifted regeneration** (+12 semitones octave up, optionally blended with -12 semitones octave down or +7 semitones fifth).

| Shimmer Pitch-Shift Method | Latency & CPU | Artefact Profile | How It Works in the Loop | Reference & License |
|---|---|---|---|---|
| **Dual-Grain Hann-Windowed Delay Pitch Shifter** | Zero framing latency; $\sim 0.1\%\text{–}0.2\%$ CPU (*unverified*) | Warm, organic when placed inside diffused loop; grain splices are masked by allpass diffusion | Two overlapping delay taps read at phasor speed $r = 2^{\Delta s / 12} - 1$ ($r = +1.0$ for +12st, $r = -0.5$ for -12st) across a $35\text{–}50\text{ ms}$ window with complementary $\sin^2(\pi\phi)$ crossfade. | Mutable Instruments `eurorack/clouds/dsp/fx/reverb.h` (**MIT**, [GitHub](https://github.com/pichenettes/eurorack)); [ValdemarOrn/CloudSeed](https://github.com/ValdemarOrn/CloudSeed) (**MIT**) |
| **Phase Vocoder / STFT Spectral Shifter** | $20\text{–}40\text{ ms}$ FFT latency; $\sim 1.0\%\text{–}2.0\%$ CPU (*unverified*) | Phase-coherent on sustained chords, but smears pick transients and adds phase-vocoder "phasiness" on recirculation | Multi-channel STFT peak-locking phase vocoder placed in the pre-tank or feedback path. | [`Signalsmith-Audio/signalsmith-stretch`](https://github.com/Signalsmith-Audio/signalsmith-stretch) (**MIT**) |
| **Time-Domain PSOLA** | Pitch-detection latency ($15\text{–}30\text{ ms}$); monophonic | Fails or glitches on polyphonic guitar chords and dense reverb tails | Requires reliable fundamental $f_0$ tracking; unsuitable for polyphonic reverb feedback. | [Charpentier & Stella 1986](https://ieeexplore.ieee.org/document/1169179) |

**Four Rules to Eliminate Metallic & Harsh Shimmer Artefacts:**
1. **Steep HF Roll-Off in the Shimmer Feedback Path:** Every recirculation through a +12st shifter doubles frequencies; without a **12 dB/oct (2-pole SVF) low-pass filter at $2.5\text{–}5.5\text{ kHz}$** after the pitch shifter, high-frequency harmonics pile up near Nyquist and alias ([Mutable Instruments `reverb.h`](https://github.com/pichenettes/eurorack/blob/master/clouds/dsp/fx/reverb.h)).
2. **Post-Shifter Allpass Diffusion:** Place 2 series modulated allpass diffusers ($7\text{ ms}, 11\text{ ms}$, $\gamma = 0.65$) immediately *after* the dual-grain pitch shifter so grain crossfade boundaries are smeared before entering the FDN matrix.
3. **Stereo De-Correlation & Modulated FDN Lines:** Run slightly detuned left/right shimmer grains ($\pm 4\text{ cents}$) and sinusoidal modulation ($0.3\text{–}0.9\text{ Hz}$, phase-staggered by $2\pi k / 8$) on all 8 FDN delay lines so standing-wave comb peaks never lock ([Luff ADC 2021](https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/)).
4. **Loop Energy Bounding (DC Blocker + Soft Clipper):** Insert a 1-pole $20\text{ Hz}$ high-pass DC blocker and a subtle $\tanh(x)$ saturator inside the tank feedback path so $95\%+$ "infinite freeze" settings remain unconditionally stable.

---

### 2.3 Delays: Digital, Analog (BBD), Tape, Tempo Sync, Ducking & Interpolation

| Mode | Signal Path & Nonlinearities | Modulation (Time-Varying Read Tap) | Reference Literature / Code |
|---|---|---|---|
| **Digital** | Clean fractional delay line (stereo independent L/R or Ping-Pong cross-feedback); 1-pole HPF ($20\text{–}400\text{ Hz}$) + LPF ($1\text{–}20\text{ kHz}$) in feedback. | Optional subtle chorus LFO ($0.1\text{–}2\text{ Hz}$). Dual-tap crossfade ($20\text{ ms}$) on preset/tempo jumps to prevent pitch zip. | [Smith PASP Delay Lines](https://ccrma.stanford.edu/~jos/pasp/Delay_Lines.html) |
| **Analog (BBD)** | Models MN3005/MN3205 Bucket Brigade Device ($N_{\text{stages}} = 4096$): input compressor $\rightarrow$ 2nd-order SVF anti-aliasing LPF ($f_c = \text{clamp}(0.35 \cdot \frac{N_{\text{stages}}}{2 T_d}, 1.2\text{ kHz}, 4.0\text{ kHz})$) $\rightarrow$ soft polynomial/even-harmonic saturation ($y = x - 0.15 x^2 - 0.1 x^3$) + DC blocker $\rightarrow$ reconstruction LPF $\rightarrow$ expander. | Slow triangle/sine clock LFO ($0.3\text{–}2.0\text{ Hz}$, depth $0\text{–}1.5\text{ ms}$). Smooth 1-pole time glide ($\tau = 80\text{ ms}$) on knob changes for classic BBD pitch-warp. | [Raffel & Smith DAFx-10 BBD](https://www.dafx.de/paper-archive/2010/DAFx10/RaffelSmith_DAFx10_P42.pdf); `Chowdhury-DSP/chowdsp_utils` (`chowdsp_bbd`, **GPLv3**) |
| **Tape** | Models magnetic tape echo: feedback path includes $\tanh(g \cdot x)$ tape saturation, a **head-bump** peaking biquad at $f_{\text{bump}} = 80\text{ Hz}$ ($+3\text{ dB}$, $Q = 1.2$), high-end loss LPF ($2.0\text{–}5.0\text{ kHz}$), and bass-tightening HPF ($70\text{ Hz}$). | **Wow** ($0.5\text{–}2.0\text{ Hz}$ sine) + **Flutter** ($6\text{–}14\text{ Hz}$ sine + 2-pole bandpass-filtered white noise at $10\text{ Hz}$) modulating delay time. | [Arnardottir et al. DAFx-08 Tape](https://www.dafx.de/paper-archive/2008/papers/dafx08_46.pdf); [`Chowdhury-DSP/ChowTapeModel`](https://github.com/Chowdhury-DSP/ChowTapeModel) (**GPLv3**) |

**Tempo Sync & Dynamic Ducking:**
- **Tempo Sync:** When `Sync = On`, compute $T_{\text{delay}} = \frac{60}{\text{BPM}} \cdot M_{\text{subdiv}}$ where $M_{\text{subdiv}} \in \{4, 2, 1, 1/2, 1/4, 1/8\} \times \{1.0 \text{ (straight)}, 1.5 \text{ (dotted)}, 2/3 \text{ (triplet)}\}$, clamped to $[1\text{ ms}, 2000\text{ ms}]$ (preallocate delay buffer for $2.5\text{ s}$ at `maxSampleRate` in `prepare()`).
- **Ducking:** Envelope detector on dry input $e[n] = \alpha_a |x[n]| + (1-\alpha_a) e[n-1]$ ($t_a = 5\text{ ms}, t_r = 200\text{ ms}$) computes gain reduction $G_{\text{duck}} = \text{clamp}(1.0 - k_{\text{duck}} \max(0, e_{\text{dB}} - \theta_{\text{dB}}), -18\text{ dB}, 0\text{ dB})$ applied to the wet output so fast lead picking stays articulate and delay swells in between phrases.

**Fractional Delay Interpolation Comparison ([Laakso et al. 1996](https://ieeexplore.ieee.org/document/482137); [Smith PASP](https://ccrma.stanford.edu/~jos/pasp/Delay_Line_Interpolation.html)):**
- **Linear (2-point):** Worst-case $-3.0\text{ dB}$ attenuation at $f_s / 4$ and $-\infty\text{ dB}$ at $f_s / 2$ when fractional delay $\eta = 0.5$; dulls high-feedback digital delays and flangers.
- **1st-Order Thiran Allpass:** $y[n] = \eta x[n] + x[n-1] - \eta y[n-1]$ ($\eta = \frac{1-d}{1+d}$, $d \in [0.5, 1.5]$). Exact unity magnitude across all frequencies, but recursive state causes clicks if delay time changes rapidly.
- **4-Point Cubic Hermite (Catmull-Rom) / 3rd-Order Lagrange:** Non-recursive 4-tap polynomial; $< 0.1\text{ dB}$ droop up to $f_s / 4$, click-free under fast LFO modulation, and only 4 multiplies + 6 adds per sample. **Recommended default for all modulated delay taps.**

---

### 2.4 Modulation: Chorus, Flanger (with Through-Zero), Phaser, Tremolo/Vibrato

| Effect | Algorithm & Topology | Key Mathematical Details | Sources |
|---|---|---|---|
| **Chorus / Vibrato** | 2-voice stereo quadrature ($\Delta\phi = 90^\circ$) or 4-voice modulated fractional delay (Cubic Hermite); Vibrato sets `Mix = 100%` and shorter base delay ($3\text{ ms}$). | Delay $D_L(t) = D_0 + M \sin(2\pi f_{\text{LFO}} t)$, $D_R(t) = D_0 + M \cos(2\pi f_{\text{LFO}} t)$, with $D_0 \in [7, 20]\text{ ms}$, $M \in [0, 6]\text{ ms}$, $f_{\text{LFO}} \in [0.1, 5.0]\text{ Hz}$, plus wet HPF ($100\text{ Hz}$). | [Dattorro 1997 Part 2 (JAES)](https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf) |
| **Flanger & Through-Zero (TZF)** | Short modulated delay ($0.2\text{–}8.0\text{ ms}$) with bipolar feedback ($-90\%\text{ to }+90\%$). In **TZF** mode, insert a fixed $D_{\text{dry}} = 2.5\text{ ms}$ delay on the dry path so the wet tap sweeps across $D_{\text{wet}}(t) - D_{\text{dry}} = 0$. | Comb notches at $f_k = \frac{2k+1}{2|\Delta\tau|}$. At $\Delta\tau = 0$ with inverted wet mix, total cancellation occurs ("jet plane" through-zero null). Report $0$ latency normally, or $\text{round}(0.0025 f_s)$ if TZF lookahead is used. | [Smith PASP Flanging](https://ccrma.stanford.edu/~jos/pasp/Flanging.html); [Dattorro 1997 Part 2](https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf) |
| **Phaser** | Cascade of $N \in \{4, 6, 8\}$ 1st-order Zero-Delay-Feedback (TPT) allpass filters with global feedback ($0\text{–}85\%$) and $50\%$ dry/wet sum (producing $N/2$ deep notches). | Each TPT allpass stage has pole/zero break frequency $\omega_c(t) = \omega_{\min} (\omega_{\max}/\omega_{\min})^{\frac{1}{2}(1 + \sin(2\pi f_{\text{LFO}} t))}$, $g = \tan(\omega_c / (2 f_s))$, $G = g/(1+g)$, $y = 2(G(x - s) + s) - x$. | [Zavalishin 2018 (VA Filter Design)](https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf); [Smith PASP Phasing](https://ccrma.stanford.edu/~jos/pasp/Phasing.html) |
| **Tremolo (Standard & Harmonic)** | **Standard:** Sinusoidal/triangle/bias-shaped gain $G(t) = 1 - d \cdot \frac{1 + \text{LFO}(t)}{2}$.<br>**Harmonic:** 2nd-order Linkwitz-Riley crossover at $600\text{ Hz}$; low and high bands modulated $180^\circ$ out of phase. | Smooths LFO waveform corners to avoid clicks; stereo pan option offsets L/R LFO phase by $0^\circ \dots 180^\circ$. | [Zölzer DAFX 2nd Ed.](https://www.wiley.com/en-us/DAFX%3A+Digital+Audio+Effects%2C+2nd+Edition-p-9780470665992) |

---

### 2.5 Open-Source Implementations & License Verdict

Every repository cited below was verified via the GitHub API on 2026-10-25:

| Repository / Project | License (Verified 2026-10-25) | AGPLv3-Compatible? | Pure C++ (No JUCE)? | How OpenSourcePedal Should Use It | Source URL |
|---|---|---|---|---|---|
| **`Signalsmith-Audio/dsp`** | **MIT** | Yes | Yes (C++11 headers) | Delay lines, Cubic/Kaiser interpolation, FFT, and biquad helpers; may be vendored or adapted in `dsp-core/`. | [github.com/Signalsmith-Audio/dsp](https://github.com/Signalsmith-Audio/dsp) |
| **`Signalsmith-Audio/signalsmith-stretch`** | **MIT** | Yes | Yes (C++11 headers) | High-quality STFT pitch-stretcher; candidate if a clean polyphonic pitch-shifter pedal block is added later. | [github.com/Signalsmith-Audio/signalsmith-stretch](https://github.com/Signalsmith-Audio/signalsmith-stretch) |
| **`Signalsmith-Audio/reverb-example-code`** | **None (`null`)** | **No** (no license file) | Yes | **Do NOT copy code.** Use only the mathematical FDN/diffuser structure taught in the ADC 2021 article. | [github.com/Signalsmith-Audio/reverb-example-code](https://github.com/Signalsmith-Audio/reverb-example-code) |
| **`ValdemarOrn/CloudSeed`** | **MIT** | Yes | Yes (C++ core) | Modulated diffuser + FDN/delay-tank ambient/shimmer architecture reference. | [github.com/ValdemarOrn/CloudSeed](https://github.com/ValdemarOrn/CloudSeed) |
| **`pichenettes/eurorack` (`clouds/dsp/fx/`)** | **MIT** | Yes | Yes (C++) | Dual-grain Hann-windowed pitch shifter inside tank (`reverb.h` / `oliverb.h`) for low-CPU Shimmer. | [github.com/pichenettes/eurorack](https://github.com/pichenettes/eurorack) |
| **`airwindows/airwindows`** | **MIT** | Yes | Yes (C++) | Reference for plate/ambient reverbs (`kPlateA`, `Galactic`, `Verbity`) and tape delay (`TapeDelay2`). | [github.com/airwindows/airwindows](https://github.com/airwindows/airwindows) |
| **`surge-synthesizer/sst-effects`** | **GPL-3.0-or-later** | Yes (GPLv3 $\rightarrow$ AGPLv3 §13) | Yes (SIMD C++) | Reference implementations for Chorus, Flanger, Phaser, Delay, and Reverb2/Nimbus. | [github.com/surge-synthesizer/sst-effects](https://github.com/surge-synthesizer/sst-effects) |
| **`Chowdhury-DSP/chowdsp_utils`** | **GPLv3** (per module header) | Yes (GPLv3 $\rightarrow$ AGPLv3 §13) | No (uses JUCE module headers) | Mathematical/algorithmic reference for BBD delay (`chowdsp_bbd`), SVF filters, and FDN utilities; write pure C++20 in `dsp-core/`. | [github.com/Chowdhury-DSP/chowdsp_utils](https://github.com/Chowdhury-DSP/chowdsp_utils) |
| **`michaelwillis/dragonfly-reverb`** | **GPL-3.0-or-later** | Yes | Uses Distrho/Freeverb3 | Reference for Moorer early reflections + late hall/plate parameter ranges. | [github.com/michaelwillis/dragonfly-reverb](https://github.com/michaelwillis/dragonfly-reverb) |

---

## 3. Concrete Recommendations & Parameter Ranges per Block

### 3.1 Modulation Block (`dsp-core/Modulation`)

| Parameter | ID (suggested) | Range | Default | Unit / Scale | Smoothing | Notes |
|---|---|---|---|---|---|---|
| **Mode** | `mod.mode` | `{Chorus, Flanger, Phaser, Tremolo, Vibrato}` | `Chorus` | Discrete | Crossfade ($15\text{ ms}$) | Selects active sub-algorithm (or run single active path). |
| **Rate** | `mod.rate` | $0.05 \dots 10.0$ | $0.8$ | $\text{Hz}$ (log skew) | $20\text{ ms}$ | LFO frequency (or tempo-synced in Phase 6). |
| **Depth** | `mod.depth` | $0 \dots 100$ | $50$ | $\%$ | $20\text{ ms}$ | Scales delay excursion (Chorus/Flanger), sweep span (Phaser), or AM depth (Tremolo). |
| **Feedback / Resonance** | `mod.feedback` | $-90 \dots +90$ | $20$ | $\%$ | $20\text{ ms}$ | Active in Flanger (bipolar) and Phaser ($0 \dots 85\%$); acts as Tone/Warmth in Chorus/Tremolo. |
| **Stereo Width / Phase** | `mod.width` | $0 \dots 100$ | $100$ | $\%$ | $20\text{ ms}$ | Maps L/R LFO phase offset $0^\circ \dots 180^\circ$ ($90^\circ$ quadrature at $50\%$). |
| **Mix** | `mod.mix` | $0 \dots 100$ | $50$ | $\%$ | $10\text{ ms}$ | Equal-power or linear dry/wet ($100\%$ forced in Vibrato/Tremolo). |

### 3.2 Delay Block (`dsp-core/Delay`)

| Parameter | ID (suggested) | Range | Default | Unit / Scale | Smoothing | Notes |
|---|---|---|---|---|---|---|
| **Mode** | `delay.mode` | `{Digital, AnalogBBD, Tape}` | `Digital` | Discrete | $20\text{ ms}$ | Selects feedback filter, saturation, and modulation model. |
| **Time (L / Master)** | `delay.time_ms` | $1.0 \dots 2000.0$ | $375.0$ | $\text{ms}$ (log skew) | $80\text{ ms}$ 1-pole (or dual-tap crossfade) | Preallocate $2.5\text{ s}$ ring buffer in `prepare(sr, maxBlock)`. |
| **Tempo Sync & Subdiv** | `delay.sync`, `delay.subdiv` | `Off/On`; `{1/16T..1/1D}` | `Off`, `1/8D` | Discrete | $20\text{ ms}$ | Overrides `time_ms` using host BPM. |
| **Ping-Pong / Stereo Offset** | `delay.ping_pong`, `delay.stereo_ratio` | `Off/On`; $50 \dots 150$ | `Off`, $100$ | $\%$ | $20\text{ ms}$ | $R$ delay time $= \text{ratio} \times L$ time (e.g. $75\%$ for dotted-eighth / quarter stereo). |
| **Feedback** | `delay.feedback` | $0 \dots 110$ | $35$ | $\%$ | $20\text{ ms}$ | Above $100\%$ engages self-oscillation bounded by $\tanh$ soft clipper. |
| **Low Cut / High Cut** | `delay.low_cut_hz`, `delay.high_cut_hz` | $20 \dots 800$ / $1000 \dots 20000$ | $80$ / $8000$ | $\text{Hz}$ (log skew) | $20\text{ ms}$ | 1-pole / 2-pole SVF filters inside the feedback loop. |
| **Modulation (Wow/Flutter)** | `delay.mod_depth` | $0 \dots 100$ | $20$ | $\%$ | $20\text{ ms}$ | Controls BBD triangle LFO or Tape Wow+Flutter excursion. |
| **Ducking** | `delay.ducking` | $0 \dots 100$ | $0$ | $\%$ (maps to $0 \dots -18\text{ dB}$) | $10\text{ ms}$ | Attenuates wet output while dry input envelope exceeds $-30\text{ dBFS}$. |
| **Mix** | `delay.mix` | $0 \dots 100$ | $25$ | $\%$ | $10\text{ ms}$ | Dry stays unity (or equal-power); wet scales up. |

### 3.3 Reverb Block (`dsp-core/Reverb`)

| Parameter | ID (suggested) | Range | Default | Unit / Scale | Smoothing | Notes |
|---|---|---|---|---|---|---|
| **Mode** | `reverb.mode` | `{Room, Hall, Plate, Shimmer}` | `Hall` | Discrete | $30\text{ ms}$ | `Room`/`Hall`/`Shimmer` use 8-ch Householder FDN; `Plate` uses Dattorro tank. |
| **Pre-Delay** | `reverb.predelay_ms` | $0.0 \dots 150.0$ | $15.0$ | $\text{ms}$ | $30\text{ ms}$ | Single delay line before input diffusers. |
| **Decay ($RT_{60}$)** | `reverb.decay_s` | $0.2 \dots 30.0$ | $2.2$ | $\text{s}$ (log skew) | $20\text{ ms}$ | Sets per-line Jot attenuation $g_i = 10^{-3 D_i / (f_s \cdot RT_{60})}$. |
| **Damping (High Decay)** | `reverb.damping` | $0 \dots 100$ | $45$ | $\%$ | $20\text{ ms}$ | Scales HF $RT_{60,\text{hi}} / RT_{60,\text{mid}}$ from $1.0$ ($0\%$) down to $0.15$ ($100\%$). |
| **Low Cut / High Cut** | `reverb.low_cut_hz`, `reverb.high_cut_hz` | $20 \dots 500$ / $1500 \dots 20000$ | $100$ / $7500$ | $\text{Hz}$ (log skew) | $20\text{ ms}$ | Applied to wet tank output (and post-shifter in Shimmer mode). |
| **Modulation** | `reverb.mod_depth` | $0 \dots 100$ | $35$ | $\%$ | $20\text{ ms}$ | Controls tank LFO excursion ($0 \dots 16$ samples at $48\text{ kHz}$). |
| **Shimmer Amount** | `reverb.shimmer` | $0 \dots 100$ | $0$ ($40$ in Shimmer mode) | $\%$ | $20\text{ ms}$ | Blends dual-grain +12st pitch shifter into the FDN feedback loop. |
| **Mix** | `reverb.mix` | $0 \dots 100$ | $20$ | $\%$ | $10\text{ ms}$ | Dry/wet blend. |

---

## 4. Open Questions for the Architect

1. **Stereo vs Mono-to-Stereo entry point for time-based blocks:** In the signal chain (`cab → EQ → modulation → delay → reverb`), does the cab or modulation block transition the signal graph from mono to stereo? *(Recommendation: run `gate → comp → drive → amp` in mono, allow `cab` to output mono or stereo, and run `modulation → delay → reverb` as true stereo 2-in/2-out).*
2. **Through-Zero Flanger latency policy:** Should TZF be an optional toggle inside Flanger that adds $2.5\text{ ms}$ ($120\text{ samples}$ at $48\text{ kHz}$) to `getLatencySamples()`, or should v1 stick to zero-latency standard flanging ($0.2\text{–}8\text{ ms}$) so toggling modulation never changes host PDC latency? *(Recommendation: zero-latency flanger by default in v1 so enabling/bypassing the pedal never glitches host PDC).*
3. **Shimmer pitch-interval selector:** Should Shimmer mode expose an interval parameter (`+12st`, `+12st / -12st`, `+12st / +7st`) or keep the UI minimal with a single `Shimmer` amount knob fixed at $+12\text{st}$ (with subtle $-12\text{st}$ foundation)?
4. **Spillover (tails on bypass):** When the user bypasses Delay or Reverb on the pedalboard UI, should `dsp-core` mute the *input send* to the delay/reverb tank while letting existing tails ring out ("soft bypass / trails"), while still offering a hard bypass mode for unit null tests (`AGENTS.md` §4)? *(Recommendation: support both `setSoftBypass(bool)` for musical trails and hard `setBypassed(bool)` for the $-120\text{ dBFS}$ null test).*

---

## 5. Sources

- Jean-Marc Jot & Antoine Chaigne (1991), *Digital delay networks for designing artificial reverberators*, AES 90th Convention Paper 3030: https://www.aes.org/e-lib/browse.cfm?elib=5661 (accessed 2026-10-25)
- Julius O. Smith III, *Physical Audio Signal Processing* (CCRMA online book — FDN, Moorer, Freeverb, Flanging, Phasing, Delay Interpolation): https://ccrma.stanford.edu/~jos/pasp/ (accessed 2026-10-25)
- Jon Dattorro (1997), *Effect Design, Part 1: Reverberator and Other Filters*, JAES Vol. 45, No. 9: https://ccrma.stanford.edu/~dattorro/EffectDesignPart1.pdf (accessed 2026-10-25)
- Jon Dattorro (1997), *Effect Design, Part 2: Delay-Line Modulation and Chorus*, JAES Vol. 45, No. 10: https://ccrma.stanford.edu/~dattorro/EffectDesignPart2.pdf (accessed 2026-10-25)
- Geraint Luff / Signalsmith Audio (2021), *Let's Write A Reverb* (ADC 2021 article): https://signalsmith-audio.co.uk/writing/2021/lets-write-a-reverb/ (accessed 2026-10-25)
- `Signalsmith-Audio/dsp` (MIT License): https://github.com/Signalsmith-Audio/dsp (accessed 2026-10-25)
- `Signalsmith-Audio/signalsmith-stretch` (MIT License): https://github.com/Signalsmith-Audio/signalsmith-stretch (accessed 2026-10-25)
- `Signalsmith-Audio/reverb-example-code` (No license file — reference article only): https://github.com/Signalsmith-Audio/reverb-example-code (accessed 2026-10-25)
- `ValdemarOrn/CloudSeed` (MIT License): https://github.com/ValdemarOrn/CloudSeed (accessed 2026-10-25)
- Émilie Gillet / Mutable Instruments `eurorack` (`clouds/dsp/fx/reverb.h`, MIT License): https://github.com/pichenettes/eurorack (accessed 2026-10-25)
- `airwindows/airwindows` (MIT License): https://github.com/airwindows/airwindows (accessed 2026-10-25)
- `surge-synthesizer/sst-effects` (GPL-3.0-or-later License): https://github.com/surge-synthesizer/sst-effects (accessed 2026-10-25)
- `Chowdhury-DSP/chowdsp_utils` (GPLv3 License per module header): https://github.com/Chowdhury-DSP/chowdsp_utils (accessed 2026-10-25)
- `Chowdhury-DSP/ChowTapeModel` (GPLv3 License): https://github.com/Chowdhury-DSP/ChowTapeModel (accessed 2026-10-25)
- `michaelwillis/dragonfly-reverb` (GPL-3.0-or-later License): https://github.com/michaelwillis/dragonfly-reverb (accessed 2026-10-25)
- Colin Raffel & Julius O. Smith III (2010), *Practical Modeling of Bucket-Brigade Device Circuits*, DAFx-10: https://www.dafx.de/paper-archive/2010/DAFx10/RaffelSmith_DAFx10_P42.pdf (accessed 2026-10-25)
- S. Arnardottir, J. S. Abel, J. O. Smith (2008), *A Digital Model of the Echoplexoge Tape Delay*, DAFx-08: https://www.dafx.de/paper-archive/2008/papers/dafx08_46.pdf (accessed 2026-10-25)
- T. I. Laakso, V. Välimäki, M. Karjalainen, U. K. Laine (1996), *Splitting the unit delay [FIR/all pass filters design]*, IEEE Signal Processing Magazine: https://ieeexplore.ieee.org/document/482137 (accessed 2026-10-25)
- Vadim Zavalishin (2018), *The Art of VA Filter Design* (v2.1.0): https://www.native-instruments.com/fileadmin/ni_media/downloads/pdf/VAFilterDesign_2.1.0.pdf (accessed 2026-10-25)
- James A. Moorer (1979), *About This Reverberation Business*, Computer Music Journal / JAES: https://www.aes.org/e-lib/browse.cfm?elib=3190 (accessed 2026-10-25)
- Manfred R. Schroeder (1962), *Natural Sounding Artificial Reverberation*, JAES / JASA: https://pubs.aip.org/asa/jasa/article/34/8B/1097/755992 (accessed 2026-10-25)
- F. Charpentier & M. Stella (1986), *Diphone synthesis using an overlap-add technique for speech waveforms concatenation (PSOLA)*, IEEE ICASSP: https://ieeexplore.ieee.org/document/1169179 (accessed 2026-10-25)
- Udo Zölzer (2011), *DAFX: Digital Audio Effects*, 2nd Edition, Wiley: https://www.wiley.com/en-us/DAFX%3A+Digital+Audio+Effects%2C+2nd+Edition-p-9780470665992 (accessed 2026-10-25)
