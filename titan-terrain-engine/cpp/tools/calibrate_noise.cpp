// Calibration + quality probe for the simplex gradient set.
//
// SimplexNoise::Sample() scales its output by a constant tied to the gradient
// table. Change the table and that constant must be re-measured, or the field
// comes out flat (or clips). This also reports directional anisotropy, which
// is the property an axis-biased gradient set degrades — it shows up as faint
// grid-aligned structure in ridged and high-octave terrain.
//
//   clang++ -std=c++20 -O2 -I ../libTitanCore/include \
//     ../libTitanCore/src/TitanNoise.cpp calibrate_noise.cpp -o calibrate
//
// Calibrate on standard deviation, not peak: peaks are rare outliers, sd is
// what governs how the terrain reads. Reference values for the current
// 8-direction unit-length table at scale 81.4:
//   sd 0.4414   max|sample| 0.8205   anisotropy 1.0101
// The previous 12-entry table (the 3D gradient set flattened to 2D, which drew
// axis-aligned directions twice as often) measured sd 0.4413 at scale 70, with
// anisotropy 1.0134 — so amplitude is preserved and the bias is reduced.

#include "TitanNoise.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Titan;

int main() {
    // Keep in step with the constant in TitanNoise.cpp::Sample.
    const float kScale = 81.4f;

    // --- output range -> scale constant ------------------------------------
    float mx = 0.0f;
    double s2 = 0.0;
    long n = 0;
    for (uint64_t seed : {1ull, 7ull, 42ull, 1234ull}) {
        SimplexNoise noise(seed);
        for (int i = 0; i < 400; ++i) {
            for (int j = 0; j < 400; ++j) {
                const float v = noise.Sample(i * 0.137f + 0.01f, j * 0.113f + 0.007f);
                mx = std::max(mx, std::fabs(v));
                s2 += static_cast<double>(v) * v;
                ++n;
            }
        }
    }
    std::printf("max|sample| = %.6f   rms = %.6f   (%ld samples)\n",
                mx, std::sqrt(s2 / n), n);
    const double sd = std::sqrt(s2 / n);
    std::printf("=> scale to match the reference sd of 0.4413: %.1f   (currently %.1f)\n\n",
                kScale * 0.4413 / sd, kScale);

    // --- directional anisotropy --------------------------------------------
    const float pi = 3.14159265f;
    double energy[8] = {};
    long m = 0;
    for (uint64_t seed : {1ull, 7ull, 42ull, 1234ull, 99ull}) {
        SimplexNoise noise(seed);
        for (int i = 0; i < 300; ++i) {
            for (int j = 0; j < 300; ++j) {
                const float x = i * 0.31f + 0.013f;
                const float y = j * 0.29f + 0.017f;
                const float c = noise.Sample(x, y);
                for (int d = 0; d < 8; ++d) {
                    const float a = d * pi / 4.0f;
                    const float h = 0.05f;
                    energy[d] += std::fabs(
                        noise.Sample(x + std::cos(a) * h, y + std::sin(a) * h) - c);
                }
                ++m;
            }
        }
    }

    double lo = 1e9, hi = 0.0;
    std::printf("directional energy (E, NE, N, NW, W, SW, S, SE):\n  ");
    for (int d = 0; d < 8; ++d) {
        energy[d] /= static_cast<double>(m);
        lo = std::min(lo, energy[d]);
        hi = std::max(hi, energy[d]);
        std::printf("%.5f ", energy[d]);
    }
    std::printf("\n  anisotropy (max/min) = %.4f   (1.0000 = isotropic)\n", hi / lo);
    return 0;
}
