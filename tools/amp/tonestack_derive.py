"""Derives the s-domain transfer function of the passive 3-knob tone stack in dsp-core amp::ToneStack (#172).

Own derivation by nodal analysis (Kirchhoff's current law at every node), solved with sympy. Method idea after Yeh and
Smith, DAFx-06 (symbolic analysis, then a per-block discretisation); no formula, coefficient or code was taken from that
paper, its web page or any other implementation. Offline tool, not shipped. Needs numpy and sympy (sympy is not in
tools/requirements.txt: pip install sympy).

  python tools/amp/tonestack_derive.py              print b0..b3, a0..a3 grouped by knob monomial t^i l^j m^k
  python tools/amp/tonestack_derive.py --bilinear   max error of a plain bilinear transform (why ToneStack uses
                                                    matched poles and a magnitude-fitted numerator instead)
"""

import sys

import numpy as np
import sympy as sp

s, t, l, m = sp.symbols("s t l m")
Rs, R1, R2, R3, R4, RL, C1, C2, C3 = sp.symbols("Rs R1 R2 R3 R4 RL C1 C2 C3", positive=True)
Ra, Rb, Rbass, Rmid = sp.symbols("Ra Rb Rbass Rmid", positive=True)  # (1-t)R1, tR1, lR2, mR3
V0, V1, Vo, V2, V3, V4 = sp.symbols("V0 V1 Vo V2 V3 V4")

# Netlist (as in ToneStack.h): Vin -Rs- V0; V0 -C1- V1; V1 -(1-t)R1- Vo -tR1- V2; V0 -R4- V3; V3 -C2- V2;
# V3 -C3- V4; V2 -lR2- V4; V4 -mR3- gnd; Vo -RL- gnd. KCL at V0, V1, Vo, V2, V3, V4 with Vin = 1.
eqs = [
    (1 - V0) / Rs - (V0 - V1) * s * C1 - (V0 - V3) / R4,
    (V0 - V1) * s * C1 - (V1 - Vo) / Ra,
    (V1 - Vo) / Ra - (Vo - V2) / Rb - Vo / RL,
    (Vo - V2) / Rb + (V3 - V2) * s * C2 - (V2 - V4) / Rbass,
    (V0 - V3) / R4 - (V3 - V2) * s * C2 - (V3 - V4) * s * C3,
    (V2 - V4) / Rbass + (V3 - V4) * s * C3 - V4 / Rmid,
]
H = sp.together(sp.solve(eqs, [V0, V1, Vo, V2, V3, V4], dict=True)[0][Vo])
num, den = sp.fraction(sp.factor(H))
# tests/dsp/test_amp_tonestack.cpp checks the transcribed result against an independent complex nodal solve (1e-9).
# Substituting the pot legs after clearing the denominators keeps a polynomial valid at the pot ends (t, l, m = 0, 1).
pots = {Ra: (1 - t) * R1, Rb: t * R1, Rbass: l * R2, Rmid: m * R3}
num, den = sp.expand(num.subs(pots)), sp.expand(den.subs(pots))
g = sp.gcd(num, den)
num, den = sp.expand(sp.cancel(num / g)), sp.expand(sp.cancel(den / g))
B = [sp.expand(sp.Poly(num, s).coeff_monomial(s**k)) for k in range(4)]
A = [sp.expand(sp.Poly(den, s).coeff_monomial(s**k)) for k in range(4)]

VARIANTS = {  # Rs, R1, R2, R3, R4, RL, C1, C2, C3 (kToneStackA/B/C)
    "a": (38e3, 250e3, 250e3, 10e3, 100e3, 1e6, 250e-12, 100e-9, 47e-9),
    "b": (1e3, 220e3, 1e6, 25e3, 33e3, 1e6, 470e-12, 22e-9, 22e-9),
    "c": (1e3, 250e3, 1e6, 25e3, 56e3, 1e6, 250e-12, 20e-9, 20e-9),
}
SYMS = (Rs, R1, R2, R3, R4, RL, C1, C2, C3)
fb = sp.lambdify((*SYMS, t, l, m), B, "numpy")
fa = sp.lambdify((*SYMS, t, l, m), A, "numpy")


def response(vals, tv, lv, mv, hz):
    sv = 2j * np.pi * np.asarray(hz)
    b, a = fb(*vals, tv, lv, mv), fa(*vals, tv, lv, mv)
    return sum(bk * sv**k for k, bk in enumerate(b)) / sum(ak * sv**k for k, ak in enumerate(a))


def audio(x):
    return (81.0**x - 1.0) / 80.0


if "--bilinear" in sys.argv:
    f = np.geomspace(20, 16000, 400)
    grid = (0, 0.25, 0.5, 0.75, 1)
    for fs in (44100, 48000, 88200, 96000, 176400, 192000):
        warped = fs / np.pi * np.tan(np.pi * f / fs)  # the analog frequency a bilinear filter shows at f
        worst = 0.0
        for vals in VARIANTS.values():
            for tv in grid:
                for x in grid:
                    for mv in grid:
                        lv, mm = 1e-3 + (1 - 1e-3) * audio(x), 1e-3 + (1 - 1e-3) * mv
                        e = 20 * np.log10(np.abs(response(vals, tv, lv, mm, warped) / response(vals, tv, lv, mm, f)))
                        worst = max(worst, np.abs(e).max())
        print(f"bilinear, fs = {fs} Hz: max error {worst:.3f} dB over 20 Hz..16 kHz (5x5x5 grid, 3 variants)")
    sys.exit(0)

for name, coeffs in (("b", B), ("a", A)):
    for k, c in enumerate(coeffs):
        print(f"{name}{k}:")
        for mon, cc in sp.Poly(c, t, l, m).terms():
            print(f"    t^{mon[0]} l^{mon[1]} m^{mon[2]}:  {sp.ccode(sp.factor(cc))}")
