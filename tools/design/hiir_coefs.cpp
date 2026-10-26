// Prints the HIIR 1.50 halfband coefficients for the ADR-0005 D3 IIR (Live) specs. The output is pasted into
// dsp-core/src/oversampling/Oversampler.cpp. Stage 0 is the fs <-> 2fs stage; transition is relative to the stage's
// higher rate. Later stages: +10 dB stopband per stage and twice the stage-0 transition (JUCE max-quality defaults).

#include "hiir/PolyphaseIir2Designer.h"

#include <cstdio>

int main()
{
    struct Spec {
        const char* name;
        double attenuationDb, transition;
    };
    constexpr Spec specs[] = {{"kUp0", 90.0, 0.05},   {"kDown0", 75.0, 0.06}, {"kUp1", 100.0, 0.10},
                              {"kDown1", 85.0, 0.12}, {"kUp2", 110.0, 0.10},  {"kDown2", 95.0, 0.12}};
    for (const auto& s : specs) {
        double coefs[64];
        const int n = hiir::PolyphaseIir2Designer::compute_coefs(coefs, s.attenuationDb, s.transition);
        std::printf("constexpr double %s[] = {", s.name);
        for (int i = 0; i < n; ++i)
            std::printf("%.17g%s", coefs[i], i + 1 < n ? ", " : "};");
        std::printf(" // %.0f dB, %.2f\n", s.attenuationDb, s.transition);
    }
    return 0;
}
