# Classic amp models: white-box stage research for Phase 5

- **Date:** 2026-10-28. **Issue:** #167 (P5-1). **Feeds:** #171 preamp, #172 tone stack, #173 power amp, #174 model.
- **Inputs read:** ADR-0011 (PR #180 branch), ADR-0002, ADR-0005 (OS-2, D5), `parameters.md` §2.5,
  `tone-targets-a.md`, `tone-targets-b.md`.
- **Wording rule:** "aims at a tone similar to" appears only here. No brand or artist name belongs in ids, code, presets or UI.
- **Evidence rule:** every external claim has a source in §9 (all accessed 2026-10-28). Numbers marked *unverified* are
  engineering hypotheses (textbook or recollection, not checked against a primary source in this session, or not
  measured). Papers behind a paywall or unreadable PDFs were cited from abstract pages only; that is stated per claim.

## 1. Summary (for the architect)

1. **Do not run a Koren/Dempwolf triode equation per sample.** Solve each stage's load line **offline in `prepare()`**
   into a static transfer curve; ship it as a closed-form fitted shape (default) or a table with a numerically
   integrated F1 (fallback). Cost per ADAA1 stage ~30–60 cycles/sample (*unverified*).
2. **Default shape family:** asymmetric rational soft clip `f(x)=L·u/(1+u)`, `u=a·x/L` per half-wave, with closed-form
   `F1=(L²/a)(u−ln(1+u))`; `tanh` (`F1=ln cosh`) for the power amp. Both have stable closed forms (§4).
3. **Grid conduction / bias shift** = a coupling HPF whose resistance switches from the leak R to a small grid R when
   the grid goes positive (attack τ ≈ 0.3–2 ms, release τ ≈ 10–50 ms). Two compares, two MACs per sample.
4. **Tone stack:** derive the 3rd-order transfer function of the passive TMB network **ourselves** (symbolic/MNA script),
   bilinear-transform per block, run as transposed DF-II with **double** state at the OS rate. Lighter alternative:
   3 fitted biquads (shelf/peak/shelf) with fixed insertion loss; keep as `tight`-class fallback only.
5. **Power amp:** push-pull odd-symmetric shaper (tanh or rational, knee chosen per tube class) + output-transformer
   band limits + **global NFB whose feedback path contains the presence and depth shelves**. Solve the delay-free loop
   by extracting the feedback filter's direct feedthrough `b0` and solving a scalar monotone equation (closed form for
   the rational shape, ≤3 Newton steps otherwise). A one-OS-sample delay in the loop is the fallback (§3.3).
6. **Sag** is a cheap rectified-power envelope (RC, attack 10–30 ms / release 100–300 ms) that scales the power stage
   ceiling (and a little of the preamp ceiling). No supply simulation.
7. **ADAA1 + OS-2 (Live 4× at 44.1/48 kHz, 2× at 88.2/96 kHz) gives an internal rate of 176.4–192 kHz in all Live
   cases.** Cascaded high-gain stages need a one-pole low-pass (Miller-like, 10–20 kHz) between stages: it is
   realistic and it is what keeps `lead_a` inside the D5 aliasing gates. Escalate to ADAA2 only if a gate fails.
8. **CPU (all *unverified*):** classic model ≈ 3–5% of one core at 48 kHz/64 Live (3 GHz reference), inside the 6%
   target, but HQ 8× at 96 kHz (768 kHz internal) costs 4× that. Oversampler filters are the largest single item.
9. **Licence:** use papers for ideas and formulas only. Do not copy or link any code. CAPS-derived tone stack code is
   GPL-3.0 on its repo, the author's page is "All rights reserved", Guitarix reports no licence via the API,
   chowdsp_utils is per-module. All our code is written from the equations.

## 2. Preamp triode stage

### 2.1 Static curve options

| Option | Equation / source | Per-sample cost (cycles, *unverified*) | Verdict |
|---|---|---|---|
| Koren triode, full | `E1=Vpk/Kp·ln(1+exp(Kp(1/µ+Vgk/√(Kvb+Vpk²))))`, `Ip=(E1^Ex+|E1|^Ex)/Kg1`; 12AX7: µ=100, Ex=1.4, Kg1=1060, Kp=600, Kvb=300 [S1] | 150–300 (pow, log, exp, sqrt) + implicit load-line solve | Offline only |
| Koren variant with fitted `Vct`: µ 100.8, Kg1 1890, Kp 828, Kvb 72, Vct 0.612, Ex 1.4979 [S2, snippet] | same family | same | Offline reference fit |
| Dempwolf/Zölzer triode (physically motivated, continuously differentiable, models grid current) [S3] | abstract only read, equations *unverified* | higher than Koren | Reference for grid current shape only; do not implement |
| Koren grid current: diode `DX` behind series `RGI`=2 kΩ [S1] | `Ig` from Shockley-like diode | n/a | Use `RGI` as the grid-conduction series R in §2.3 |
| Fitted rational asymmetric clip (our default) | §2.2 | ~35 (1 div, 1 log, ~6 flop; ADAA reuses F1 of previous sample) | **Default** |
| Fitted `tanh` + DC offset | `F1=ln cosh` | ~45 (exp+log) | Power amp / alt. |
| Table of the offline-solved curve (cubic Hermite, 2048 pts) + integrated F1 table | prepare() builds both; F1 via cumulative Simpson in `double` | ~30 (2 lookups) | Fallback when a fit misses the Koren curve by > 1 dB THD |

Triode orientation (output inverted, so look at the **input** polarity): positive grid swing drives the plate toward
saturation with grid current (hard, early limit); negative swing drives toward plate cutoff (softer, more compressive).
Treat asymmetry in **input** terms: the positive side limits earlier and harder; use ~10–30% ceiling difference
(*unverified*, qualitative from Koren curves [S1]). The DC offset from the
asymmetry is removed by the following coupling HPF.

### 2.2 Closed form used as the default

For input `x` (already scaled by the stage drive `k`) and per-side parameters `(a_s, L_s)`, `s∈{pos,neg}`:

```
f(x)  = L·u/(1+u),        u = a·|x|/L (sign restored)
F1(x) = (L²/a)·(u − ln(1+u))        (continuous, F1'(0)=0, C1 at 0 if a_pos=a_neg)
```
Set `a=1` (small-signal slope of the normalised stage); the stage drive `k` carries the real gain. Keep `a` equal on both
sides so `f'(0)` is continuous, and vary only `L_pos/L_neg`.

ADAA1 pitfalls (from the tutorial [S4]): when `|x[n]−x[n−1]|<TOL` use `f((x[n]+x[n−1])/2)`; keep F1 in `double`
(the difference of two large F1 values loses float precision at TOL≈1e-5); and under a slowly modulated `L` (sag),
recompute `F1(x[n−1])` with the **current** `L` (one extra log) instead of storing the old F1.

### 2.3 Coupling cap, grid conduction, bias shift, cathode bypass

| Effect | Model | Cost | Starting constants (*unverified*) |
|---|---|---|---|
| Coupling HPF | 1-pole HPF `fc=1/(2π·R·C)` | 3 MAC | `fc = 7–15 Hz` (22 nF into 0.47–1 MΩ), 70–200 Hz in `tight` (2.2–4.7 nF into 470 kΩ) |
| Grid conduction / blocking (bias shift) | HPF capacitor state `v_c`; if `x−v_c > Vg_on` (≈0 V relative to cathode bias) the charging R is `RGI`-like (small), else the leak R. Output `x−v_c` | 2 compare + 2 MAC | attack τ 0.3–2 ms, release τ 10–50 ms; `Vg_on` ≈ 0 to +0.1 knee units |
| Cathode bypass (frequency dependent gain) | shelf: gain rises from `A_unbyp` (low) to `A_byp` (above `fc=1/(2π·Rk·Ck)`) | 1 biquad or 1-pole shelf | clean: `Rk·Ck` → fc ≈ 4–10 Hz (effectively flat); lead stages 2–4: fc 150–300 Hz, shelf 6–10 dB (gives the tight low end) |
| Miller / interstage low-pass | 1-pole LP at stage input | 3 MAC | clean 20 kHz; crunch 16 kHz; lead stage 1 16 kHz falling to 8–12 kHz for stages 3–4 |

Small-signal stage gain (textbook, *unverified* in this session): bypassed `A≈µ·Rp/(rp+Rp)`, unbypassed
`A≈µ·Rp/(rp+Rp+(µ+1)·Rk)`; with µ=100, rp≈62 kΩ [S1 µ] and Rp=100 kΩ: bypassed ≈ 60 (35 dB), unbypassed with 1.5 kΩ
≈ 29 (29 dB). Use 25–35 dB as the stage gain scale.

### 2.4 Cascading 2–4 stages

Order per stage: `HPF/grid-conduction → Miller LP → drive k → ADAA shaper → cathode-bypass shelf → interstage
attenuation` (a fixed gain < 1 that models the plate-load divider and keeps stage knees at a sensible `k`). Stage
state is ~5 floats. Four stages need at most **4 ADAA slots**; each adds 0.5 OS sample of delay [ADR-0005]. Total delay
for 6 slots (4 preamp + 2 power) = 3 OS samples ≈ 0.75 base samples at 4×; `prepare()` measures it anyway (ADR-0005 D4).

## 3. Tone stack and power amp

### 3.1 Passive 3-knob tone stack

| Item | Content |
|---|---|
| Method | Yeh and Smith derive the 3rd-order transfer function of the classic treble/mid/bass network symbolically, apply the bilinear transform and implement DF-II; coefficients are closed forms in the three pot positions [S5, S6]. The page says the formulas are circuit-specific, same topology for most Fender/Marshall/Vox stacks with different component values [S6] |
| What we reuse | The **method** only. Our dsp-engineer re-derives the coefficient polynomials in `t,m,l` with a sympy/MNA script (tool in `tools/`, not shipped) so no formula or code is copied. |
| Source impedance | Add the preceding stage output R (≈38–100 kΩ, *unverified*) in series with the network input; treat the load as ideal (hypothesis, *unverified*) |
| Known stack values, '59 5F6-A style: R_treble 250 kΩ, R_bass 1 MΩ, R_mid 25 kΩ, R_slope 56 kΩ, C1 250 pF, C2 20 nF, C3 20 nF | *unverified recollection*, to be checked by #172 against [S5] and a schematic |
| Other variants | American blackface style (250 pF / 100 nF / 47 nF, 250 k / 250 k / 10 k, ~100 k slope) and British style (470 pF / 22 nF / 22 nF, 220 k / 1 M / 25 k, ~33 k slope): *unverified recollection* |
| Discretisation | Bilinear at the **OS rate** (176–384 kHz): frequency warping of `tan(ω/2)` vs `ω/2` at 10 kHz/192 kHz is 0.9% (computed). No prewarp needed above 100 Hz; prewarp the 3 poles anyway (free, per block). |
| Numerics | Pole near 50–100 Hz at 192 kHz is 0.0016–0.003 rad from z=1: use `double` state and transposed DF-II (or Gray-Markel lattice [S6 mentions]) so per-block coefficient changes do not click. Cost: 6 MAC = ~12 cycles/sample. |
| Knob smoothing | knobs smoothed 20 ms (parameters.md), polynomial recompute once per block (~60 flops), no per-sample interpolation needed at 64-sample blocks (1.3 ms) |

**Knob taper (derived, exact):** audio ("log") pot with 10% of the resistance at mid-travel:
`R(x)=(81^x−1)/80`, `x∈[0,1]` (e^k=81 gives R(0.5)=0.1). Linear pot: `R(x)=x`. Bass and volume-class pots audio;
treble/mid linear is the common arrangement (*unverified*). Provide per-variant taper flags.

**Typical response ranges (*unverified*, to be asserted by #172 tests from its own derivation):**

| Setting | Approx. effect |
|---|---|
| Insertion loss, all knobs mid | −10 to −20 dB at ~500–800 Hz, makeup gain in the next stage |
| Bass 0→10 | ±12–15 dB at ≤100 Hz relative to 1 kHz |
| Treble 0→10 | ±12–15 dB at ≥3 kHz |
| Mid 0→10 | notch/hump around 400–800 Hz, about −20 dB at 0 and −3 to −6 dB at 10 |
| Interaction | Bass and treble settings shift the mid notch; mid at max flattens the scoop |

**Lighter alternative.** Three biquads (low shelf 100 Hz, peak 500–800 Hz Q≈0.7, high shelf 3 kHz, ±12 dB; the NAM
channel EQ in parameters.md §2.5 is exactly this) with a fixed 12 dB insertion-loss pad, fitted to the full stack
over a 5×5×5 knob grid. Cost 15 MAC, no interaction. Use only if the full stack misses the budget (it should not).

### 3.2 Push-pull power amp

| Element | Model | Cost (*unverified*) |
|---|---|---|
| Phase inverter + two halves | Odd-symmetric `g(x)` for the pair (difference of two half-wave shapers). Class AB: `y = h(x−b) − h(−x−b)` with a soft rectifier `h` for crossover. | 1 ADAA shaper (or 2 if asymmetric) |
| Tube class (both are Koren pentode/beam families [S1, pentode eq.: `Ip` uses `atan(Vpk/Kvb)` knee, 6550 µ=7.9, Ex=1.35, Kg1=890, Kg2=4200, Kp=60, Kvb=24]) | beam tetrode style: sharper knee, `tanh`; pentode style: softer knee, rational shape, more compression | same |
| Output transformer | 2nd-order HPF 30–60 Hz, 1-pole+resonant LPF 8–14 kHz, optional LF-saturation (level-dependent HPF) deferred to v2 | 2 biquads |
| Speaker-load interaction | not modelled; `depth` provides the low-resonance effect (§3.3) | n/a |
| Tube references for push-pull + global NFB + output transformer in a DAFx model: Mačák and Schimmel [S7] (abstract only read). Cohen and Hélie [S8] cover a single-ended class A stage and a transformer model from datasheets (abstract only). A Mačák thesis section list shows "Simulation of Circuit with Global Feedback" and "Approximation of Implicit Nonlinear Circuit Equations" [S9, titles only] | — | — |

### 3.3 Negative feedback with presence and depth

```
x ──►[ preamp out, master ]──(+)──► A(·): PI + shaper + OT filters ──► y
                              ▲ −
                              └── β(z): feedback network incl. presence HS (2–6 kHz) and depth LS (60–150 Hz) ◄── y
```
- Closed-loop small-signal gain `A/(1+Aβ(f))`. Presence/depth are shelves **in β**: raising β at HF lowers closed-loop HF
  gain. With loop gain `N` dB, a ±10 dB knob needs `N ≥ ~12–14 dB` (so ±10 dB is reachable); use `N = 14 dB` at
  midband, `β = 10^(−14/20)` nominal, `|Gb| ≤ 10 dB` shelf (*unverified* starting values).
- Because clipping collapses loop gain, presence/depth changes sound strongest on clean and fade as the power stage
  saturates: this is the physical behaviour and an intended property of "inside the loop" (parameters.md §2.5).
- **Delay-free loop.** (a) **Preferred:** split `β(z)=b0+β̃(z)` with `β̃` strictly causal from stored states. The
  instantaneous equation `w = u − N0·(b0·y + s)`, `y = g(w)` has a scalar monotone solution: closed form for the
  rational shape (quadratic in `y`), ≤3 Newton or safeguarded secant iterations for `tanh`. Apply ADAA on the solved `w`
  sequence and feed the **ADAA output** back at the next sample (Parker et al. state the method works in feedback
  systems [S10, abstract only]; exact scheme *unverified*, #173 prototype). (b) **Fallback:** one OS-sample delay in β:
  at 192 kHz it is 5.2 µs, a 37° phase lag at 20 kHz (computed), acceptable only with a phase margin ≥ 45°.
- **Stability tests (required):** sweep presence/depth/master on a 5×5×5 grid; compute the open-loop response of the
  linearised loop (small signal) and assert gain and phase margin ≥ 45°; assert no limit cycle and no NaN under
  full-scale steps; the loop may contain at most two HPF poles plus the depth shelf (low-frequency motor-boating guard).

### 3.4 Supply sag

- Physics: a rectifier plus filter capacitor behave as a Thevenin source (≈410 V, ≈590 Ω for one 5E3-style example)
  and the plate rail drops ≈ 32 V (≈ 9%) when plate current rises 84 → 138 mA; a power chord sags the rail at
  ≈ 2.9 V/ms, the screen rail ≈ 0.5 V/ms behind an RC [S11].
- **Model:** `e = RC-envelope(|y_pa|²)`, attack 10–30 ms, release 100–300 ms (*unverified*, consistent with the ms-scale
  rates above); ceiling scaling `h = 1 − S·e/(e+e0)` with sag amount `S`; apply `h` to the power-stage ceiling and
  `0.2·h` to preamp stages 3–4. Cost: 1 abs, 2 MAC, 1 div per OS sample (or per base sample, 4× cheaper).
- A published patent describes envelope-limited output power to imitate sag [S12, listing only]. Our design is a plain
  RC envelope of the circuit physics; **#173 must not copy claim language**. Marked an open question.

## 4. ADAA first order, cost, and OS-2 interaction

| Nonlinearity | `F1` | Fallback when `Δx<TOL` | Cost (*unverified*) |
|---|---|---|---|
| Rational asymmetric (preamp default) | `(L²/a)(u−ln(1+u))` | `f(mean)` | 1 log, 2 div |
| `tanh` | `ln cosh x = |x| + ln(1+e^(−2|x|)) − ln 2` (stable) | `tanh(mean)` | 1 exp, 1 log, 1 div |
| Hard clip (reference test) | `x²/2` inside, `|x|−½` outside [S4] | `clip(mean)` | 2 compares |
| Table (fallback) | cumulative Simpson of the table in `prepare()` | table at mean | 2 Hermite lookups |
| Class-AB soft rectifier | softplus-like `h`, F1 closed form via `ln(1+e^x)` integrals (*unverified*) | mean | 2 exp/log |

- First order adds **0.5 OS sample** of delay and a mild HF roll-off (ADR-0005 §1.5); ADAA alone leaves audible aliasing
  but "ADAA1 plus ~2× OS" is the usual compromise [S4, S13]. ADR-0005 D5 gates (Live ≤ −60 dB ASR for f0≤2.5 kHz,
  ≤ −40 dB up to 8 kHz; HQ warn) are what binds us.
- **Internal rates:** Live 44.1/48 kHz 4× → 176.4/192 kHz; Live 88.2/96 kHz 2× → 176.4/192 kHz; HQ 8× → 352.8/384 kHz at
  44.1/48 kHz and **768 kHz at 96 kHz**. Per-OS-sample cost multiplies by these rates.
- **Cascades.** A gain stage's output has harmonics; if several run back-to-back with large gain, aliasing products of
  stage 2 fold again in stage 3. Realistic Miller LPs between stages (§2.3) remove HF before the next clip and make a
  4-stage lead plausible with ADAA1. Per-stage D5 tests on a 1 kHz and a 2.5 kHz sine at full drive, then the model.
- **Slowly varying parameters** (sag, bright): recompute F1(x[n−1]) with current parameters (§2.2).

## 5. v1 models (starting hypotheses, all *unverified*, to be tuned by measurement in #174)

All gains in dB small-signal; "onset" = peak input-referred level at the amp input at the quoted knob settings where
the named stage reaches ≈1% THD. Guitar DI of ≈ −18 dBFS RMS is ≈ −10 dBFS peak. Calibration (what 0 dBFS maps to) is
open question Q1.

| | `osp.amp.clean_a` | `osp.amp.crunch_a` | `osp.amp.lead_a` |
|---|---|---|---|
| Aims at a tone similar to | hi-fi American-style clean with large headroom | edge-of-breakup British-style combo to master-volume crunch | modern high-gain head with a tight low end |
| Order | S1 → TS → S2 → PA | S1 → S2 → TS → PA | S1 → S2 → TS → S3 → S4 → PA |
| Preamp stages (triode) | 2 | 2 (+ optional 3rd off by default) | 4 |
| `gain` law (pre-stage, dB) | −24 + 4.8·g (g=3 → −10) | −12 + 4.2·g | 0 + 4.5·g |
| Stage small-signal gain | S1 +26, S2 +20 (TS loss −16 recovered by S2) | S1 +28, S2 +24 | S1 +28, S2 +26, S3 +22, S4 +20 |
| Ceiling asymmetry (pos/neg) | 1.0/1.15 | 1.0/1.25 | 1.0/1.3 (S1–S2), 1.0/1.2 (S3–S4) |
| Coupling HPF | 12 Hz | 20 Hz | S1 25 Hz, S2 70 Hz, S3–S4 90–150 Hz |
| Cathode shelf | none (≈5 Hz) | S2 +4 dB above 100 Hz | S2–S4 +6…10 dB above 150–300 Hz |
| Miller LP | 20 kHz | 16 kHz | 16 → 12 → 10 → 8 kHz (S1…S4) |
| Grid-conduction | present, mild (τ 2 ms / 30 ms) | present (1 ms / 25 ms) | present, stronger (0.5 ms / 15 ms) |
| Onset at defaults (gain 3 / 5 / 6) | S1 ≥ +6 dBFS, S2 ≥ +3 dBFS (no preamp clip with DI) | S2 ≈ −14 dBFS, S1 ≈ −2 dBFS | S1 −20, S2 −26, S3 −30, S4 −34 dBFS |
| Tone stack (variant) | American blackface-style, 250 pF/100 nF/47 nF | British-style, 470 pF/22 nF/22 nF, mid at lower scoop | British-style with mid at 25 k, slope 33 k |
| Power amp | beam-tetrode knee, big headroom: onset at master 5 ≈ 0 dBFS | pentode style, onset ≈ −6 dBFS at master 5, class AB bias | pentode, onset ≈ −10 dBFS at master 6, tighter NFB |
| NFB amount / presence+depth | 14 dB / full range | 12 dB / full range | 16 dB / presence ±10 dB, depth ±8 dB |
| Output transformer | HPF 40 Hz, LPF 14 kHz | HPF 50 Hz, LPF 11 kHz | HPF 60 Hz, LPF 9 kHz |
| Sag amount `S` | 0.05 (≈ 0.5 dB) | 0.25 (≈ 2–3 dB) | 0.15 (≈ 1.5 dB) |
| `bright` voicing | high shelf +6 dB at 2.5 kHz at gain 0, linearly to 0 dB at gain 10 (models a pot treble-bleed capacitor) | +5 dB, 2 kHz, same decay | +4 dB, 3 kHz, same decay |
| `tight` voicing | pre-S1 HPF 12 → 60 Hz, 10 ms xfade | 20 → 90 Hz | 25 → 150 Hz, plus cathode shelf corner up 100 Hz |
| THD acceptance targets (at 1 kHz, gain default) | ≤ 1% at −12 dBFS, ≤ 8% at 0 dBFS | 5–10% at −12 dBFS | ≥ 30% at −12 dBFS |

Why the 3 rows: tone-targets-a finding 1 (four of five targets in clean to mid-gain territory, one high-gain), the Lead
channel in the reference set is the Crunch plus one extra triode stage, and a "Fender-like clean / master-volume rock
head / classic-rock to high-gain" split for the rhythm-oriented targets [tone-targets-a table, T4]. The depth/presence
ranges follow tone-targets-b F6 (±10 dB).

## 6. CPU estimates (all *unverified*, 3 GHz reference core, cycles/sample at the OS rate)

`% core = cycles × OS_rate / 3e9`. At 192 kHz (Live 4× at 48 kHz) one cycle/sample = 0.0064% of a core.

| Block | Cycles/OS sample | % core at 192 kHz |
|---|---|---|
| Triode stage (HPF+grid cond., LP, ADAA1 rational, bypass shelf) | 50–70 | 0.32–0.45 |
| Tone stack, full 3rd order, double state | 12–20 (+ ~60 flops / block) | 0.08–0.13 |
| Power amp: ADAA tanh + implicit NFB (≤3 Newton) + 2 OT biquads + 2 NFB shelves | 150–250 | 1.0–1.6 |
| Sag envelope | 8 | 0.05 |
| OS-2 4× IIR halfband, up + down | 60–120 per **base** sample (ADR-0005 measures) | 1.2–2.4 |
| **clean_a** | 2 stages + TS + PA + sag + OS | **≈ 2.8–4.8%** |
| **crunch_a** | same + extras | **≈ 3–5%** |
| **lead_a** | 4 stages + TS + PA + sag + OS | **≈ 3.8–6.0%** (edge of the 6% target) |

HQ 8× at 48 kHz doubles the nonlinear part and, with the FIR halfbands (ADR-0005 D5), more again; at 96 kHz (768 kHz)
`lead_a` could reach 15–25%. If `lead_a` Live exceeds 6%: merge S3+S4 cathode shelves, drop the NFB Newton step to
the closed-form rational variant, share one HQ-lite Miller LP, and SIMD the independent biquad pairs. Measured numbers
replace this table in P5-8 (#174).

## 7. Licence notes

| Reference | Kind | Licence / terms | Use |
|---|---|---|---|
| Koren, "Improved vacuum tube models for SPICE simulations" [S1] | article | no licence stated; copyright the author | equations and the public parameter tables as **facts/ideas**, cite |
| Yeh and Smith, DAFx-06 [S5]; Yeh page [S6] | paper; web page "© 2007 D. T. Yeh. All rights reserved", downloads "for archival purposes" | no code licence | method only; **no Matlab/code from the page** |
| CAPS tone stack `ToneStack.cc` (Debian mirror) [S14] | code | `moddevices/caps-lv2` reports **GPL-3.0** (GitHub API) [S15]; upstream CAPS version *unverified* | AGPLv3-compatible only if GPLv3, but copying is unnecessary: **do not copy** |
| Guitarix | code | GitHub API shows no detected licence [S16]; GPL, version *unverified* | **do not read-and-copy**; ideas via papers only |
| Dempwolf and Zölzer, DAFx-11 [S3]; Pakarinen and Yeh [S17]; Mačák and Schimmel [S7, S9]; Cohen and Hélie [S8]; Parker et al. [S10]; Bilbao et al. [S13]; Kuehnel (Amp Books) [S11] | papers/web (Amp Books © 2005–2026 all rights reserved) | copyright papers; no code | ideas and equations; cite |
| chowdsp_utils | code | LICENSE.md: "each module has its own licence", non-module code GPLv3 [S18] | **not used** here; a later dependency decision needs a per-module check (BSD-3-Clause for some modules *unverified*) |
| Patent on envelope-driven sag [S12] | patent | n/a | do not copy claim language; legal check is an open question |

Nothing here was taken from a commercial plugin. Component values from "known stacks" are common schematic facts,
but we cite the paper and re-derive them.

## 8. Open questions

| # | Question | Owner |
|---|---|---|
| Q1 | Level calibration: what internal amplitude equals 0 dBFS at the amp input and what do the "onset" values in §5 map to? Propose a normalisation table. | #174 |
| Q2 | Fit the rational/tanh stage shapes to the offline Koren load-line curves (12AX7, Rp 100 kΩ, Rk 1.5 kΩ bypassed/unbypassed) and report max THD error vs the curve; decide table fallback. | #171 |
| Q3 | Grid-conduction release time and `Vg_on`: validate against a SPICE (ngspice, BSD) run of a coupling network with the Koren model. | #171 |
| Q4 | Derive the 3rd-order coefficients with sympy, check against [S5] figures and at least two schematics; confirm component values and taper flags in §3.1. | #172 |
| Q5 | Source/load impedance model for the tone stack (series R, load ignored) vs a one-pole interaction term. | #172 |
| Q6 | Delay-free loop: closed-form quadratic (rational) or Newton (tanh) with ADAA on the solved `w`; compare with one-OS-sample delay at 44.1 and 96 kHz Live; margin tests per §3.3. | #173 |
| Q7 | Transformer LF-saturation, speaker impedance interaction, class-AB crossover: v1 or v2? | #173 |
| Q8 | Sag law and the patent concern in [S12]: confirm the RC envelope is clear, and tune `S` per model. | #173 |
| Q9 | Aliasing of `lead_a`: per-stage D5 results; whether ADAA2 on S3/S4 or a Miller LP at lower corner is needed. | #174 |
| Q10 | HQ 8× at 96 kHz = 768 kHz internal: accept the cost, or cap HQ amp at 4× above 48 kHz (ADR-0005 amendment). | #174 (architect decides) |
| Q11 | Where the tone stack sits in `lead_a` (S2 → TS → S3 or end-of-preamp): choose by measured THD vs `bass` knob and spectrum, not ears. | #174 |
| Q12 | `bright`/`tight` curves: confirm xfade 10 ms does not click with ADAA state; plan unit tests. | #174 |

## 9. Sources (all accessed 2026-10-28)

| ID | URL | Used for |
|---|---|---|
| S1 | https://www.normankoren.com/Audio/Tubemodspice_article_2.html | Koren triode/pentode equations, 12AX7 and 6550 parameters, grid diode with RGI |
| S2 | https://re.public.polimi.it/retrieve/e0c31c0a-187b-4599-e053-1705fe0aef77/DAFx07-Sarti.pdf (search snippet only) | alternate 12AX7 fit |
| S3 | https://dafx.de/paper-archive/details/MGMeIyM6_9oAzOgKCuzEqw | Dempwolf/Zölzer triode model (abstract) |
| S4 | https://ccrma.stanford.edu/~jatin/Notebooks/adaa.html | ADAA1 formula, TOL fallback, hard clip F1, ADAA+2× OS |
| S5 | https://dafx.de/paper-archive/details/bbvGpyU59H2nHvV3Iamezg | Yeh and Smith tone stack (abstract) |
| S6 | https://ccrma.stanford.edu/~dtyeh/tonestack/ | Yeh tone stack page, rights, method |
| S7 | https://dafx.de/paper-archive/details/KotNLLBUHr-Wb7CEhWR_DA and https://dafx.de/paper-archive/details/IV9AdFRgtfFewuNXM7JAfA | Mačák and Schimmel push-pull + global NFB (abstracts) |
| S8 | https://dafx.de/paper-archive/details/yZNxN3FPA2HJUN5vKmCG0A | Cohen and Hélie power amp (abstract) |
| S9 | https://ccrma.stanford.edu/~jingjiez/portfolio/gtr-amp-sim/pdfs/Real-time Digital Simulation of Guitar Amplifiers as Audio Effects.pdf | thesis section titles only |
| S10 | https://dafx.de/paper-archive/details/vem_XXF5qBbfiWOH2RVVAA | Parker et al., ADAA origin; usable in feedback |
| S11 | https://www.ampbooks.com/mobile/dsp/power-supply/ | sag physics, numbers |
| S12 | https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/10680562 | patent on envelope-driven sag (search listing only) |
| S13 | https://dblp.org/rec/journals/spl/BilbaoEPV17 (IEEE SPL 24(7):1049, DOI 10.1109/LSP.2017.2675541) | ADAA higher order, reduced OS |
| S14 | https://sources.debian.org/src/caps/0.9.23-1/ToneStack.cc/ | existence of CAPS tone stack code (not read) |
| S15 | `gh api repos/moddevices/caps-lv2/license` | GPL-3.0 detected |
| S16 | `gh api repos/brummer10/guitarix` | no detected licence |
| S17 | https://www.effectrode.com/wp-content/uploads/2018/08/a_review_of_digital_techniques_for_modeling_guitar_amplifiers.pdf (PDF unreadable here); https://research.aalto.fi/en/publications/a-review-on-digital-techniques-for-modeling-vacuum-tube-guitar-am/ | Pakarinen and Yeh review, CMJ 33(2), 2009 (bibliographic only) |
| S18 | `gh api repos/Chowdhury-DSP/chowdsp_utils/contents/LICENSE.md` | per-module licensing |
