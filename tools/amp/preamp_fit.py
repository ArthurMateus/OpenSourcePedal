#!/usr/bin/env python3
"""Offline Q2 check for the preamp stage (#171; docs/research/classic-amp-models.md §2.1, §8 Q2).

1. Solves the load line of one common-cathode triode stage (cathode AC-bypassed) with the Koren triode equations,
   including a static grid clamp, exactly as PreampStage::prepare() does in C++.
2. Fits the research default, the asymmetric rational soft clip y = g·v/(1 + |v|/L±), and reports its error. The
   research rule is: fall back to a table when the fit misses the curve by more than 1 dB THD.
3. Prints reference values of the normalised curve that tests/dsp/test_amp_preamp.cpp checks the C++ table against.

Koren triode (N. Koren, "Improved vacuum tube models for SPICE simulations", accessed 2026-10-28,
https://www.normankoren.com/Audio/Tubemodspice_article_2.html), the article's 12AX7 constants:
    E1 = Vpk/Kp · ln(1 + exp(Kp·(1/mu + Vgk/sqrt(Kvb + Vpk²)))),  Ip = (E1^Ex + |E1|^Ex) / Kg1.
Grid current (the article's diode behind RGI = 2 kΩ) as a static clamp: past Vgk = 0 the grid moves by
k = RGI/(Rs + RGI) of the input, with a quadratic knee of half-width W volts.
Circuit (textbook values, unverified in this session): B+ 250 V, Rp 100 kΩ, Rk 1.5 kΩ bypassed, source Rs 68 kΩ.

Usage: python tools/amp/preamp_fit.py   (prints a report; writes nothing)
"""

import numpy as np
from scipy.optimize import brentq, least_squares

MU, EX, KG1, KP, KVB = 100.0, 1.4, 1060.0, 600.0, 300.0
B_PLUS, RP, RK, RS, RGI, W = 250.0, 100e3, 1.5e3, 68e3, 2e3, 0.1


def koren_ip(vgk, vpk):
    vpk = max(vpk, 1e-6)
    e1 = vpk / KP * np.logaddexp(0.0, KP * (1.0 / MU + vgk / np.sqrt(KVB + vpk * vpk)))
    return (e1**EX + abs(e1) ** EX) / KG1


def clamp(vgk):
    k = RGI / (RS + RGI)
    ramp = 0.0 if vgk <= -W else (vgk if vgk >= W else (vgk + W) ** 2 / (4.0 * W))
    return vgk - (1.0 - k) * ramp


def bias_point():
    ip = brentq(lambda i: i - koren_ip(-i * RK, B_PLUS - i * (RP + RK)), 0.0, B_PLUS / RP, xtol=1e-15)
    return ip, ip * RK  # Ip0, Vk0


def plate_swing(vin, vk0):
    """Plate swing for a grid input vin around the bias point, sign flipped so that it follows vin."""
    vgk = clamp(vin - vk0)
    ip = brentq(lambda i: i - koren_ip(vgk, B_PLUS - i * RP - vk0), 0.0, B_PLUS / RP, xtol=1e-15)
    return ip * RP  # minus the plate voltage, up to a constant


def thd(curve, amp, n=4096):
    x = amp * np.sin(2.0 * np.pi * np.arange(n) / n)
    spec = np.abs(np.fft.rfft(curve(x) - np.mean(curve(x))))
    return np.sqrt(np.sum(spec[2:64] ** 2)) / spec[1]


def main():
    ip0, vk0 = bias_point()
    y0 = plate_swing(0.0, vk0)
    swing = lambda v: plate_swing(v, vk0) - y0  # noqa: E731
    slope = (swing(1e-4 * vk0) - swing(-1e-4 * vk0)) / (2e-4 * vk0)
    print(f"bias Ip0 {ip0 * 1e3:.4f} mA, Vk0 {vk0:.4f} V; small-signal gain {slope:.2f} ({20 * np.log10(slope):.2f} dB)")

    # Normalised curve: x in units of Vk0 (x = 1 is Vgk = 0, the grid-current onset), slope 1 at x = 0.
    f = lambda x: swing(x * vk0) / (slope * vk0)  # noqa: E731
    xs = np.linspace(-4.0, 4.0, 1601)
    ys = np.array([f(x) for x in xs])
    print("reference points (x, f(x)):")
    for x in (-3.0, -2.0, -1.5, -1.0, -0.5, -0.1, 0.1, 0.5, 0.9, 1.0, 1.2, 2.0, 4.0, 16.0, 64.0):
        print(f"    {{{x}, {f(x):.6f}}},")

    def rational(p, x):
        g, lp, ln = p
        return g * x / (1.0 + np.abs(x) / np.where(x >= 0.0, lp, ln))

    fit = least_squares(lambda p: rational(p, xs) - ys, x0=[1.0, 1.0, 1.0], bounds=([0.1, 1e-3, 1e-3], [10, 50, 50]))
    curve = lambda x: np.interp(x, xs, ys)  # noqa: E731
    fitted = lambda x: rational(fit.x, x)  # noqa: E731
    worst = max(abs(20 * np.log10(thd(fitted, a) / thd(curve, a))) for a in np.linspace(0.1, 2.0, 20))
    print(f"rational fit g {fit.x[0]:.3f}, L+ {fit.x[1]:.3f}, L- {fit.x[2]:.3f} (L-/L+ {fit.x[2] / fit.x[1]:.3f}):")
    print(f"    max |error| {np.max(np.abs(rational(fit.x, xs) - ys)):.4f} (normalised), THD error up to {worst:.1f} dB")
    print("THD of the curve vs a sine of peak x (normalised):")
    for a in (0.1, 0.25, 0.5, 1.0, 2.0):
        print(f"    peak {a:4.2f}: curve {100 * thd(curve, a):6.2f} %, rational fit {100 * thd(fitted, a):6.2f} %")


if __name__ == "__main__":
    main()
