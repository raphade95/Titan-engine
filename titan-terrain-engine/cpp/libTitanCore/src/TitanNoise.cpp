#include "TitanNoise.h"
#include "TitanRandom.h"

#include <algorithm>
#include <cmath>

namespace Titan {

namespace {

// Eight unit gradients at 45-degree increments.
//
// This replaces the classic 12-entry table, which is the *3D* gradient set
// flattened to 2D and is directionally biased as a result: {1,0} and {-1,0}
// each appear twice, as do {0,1} and {0,-1}, while the four diagonals appear
// once, so axis-aligned directions are drawn twice as often. That bias shows
// up as faint grid-aligned N/S/E/W structure in ridged and high-octave output
// — exactly the artefact a terrain tool gets judged on. Twelve entries also
// meant `% 12` over a 0..255 permutation value, which skews the first four
// entries a little further.
//
// Eight equal-length, evenly spaced directions have no preferred axis, and
// indexing with `& 7` is unbiased and cheaper than a modulo.
constexpr float kR2 = 0.70710678f; // sqrt(2)/2
const float kGrad2[8][2] = {
    { 1.0f,  0.0f}, { kR2,   kR2},
    { 0.0f,  1.0f}, {-kR2,   kR2},
    {-1.0f,  0.0f}, {-kR2,  -kR2},
    { 0.0f, -1.0f}, { kR2,  -kR2}
};

inline float Dot2(const float g[2], float x, float y) {
    return g[0] * x + g[1] * y;
}

constexpr float kF2 = 0.36602540378f; // (sqrt(3) - 1) / 2
constexpr float kG2 = 0.21132486540f; // (3 - sqrt(3)) / 6

} // namespace

SimplexNoise::SimplexNoise(uint64_t seed) {
    uint8_t table[256];
    for (int i = 0; i < 256; ++i) table[i] = static_cast<uint8_t>(i);

    // Fisher-Yates with PCG32 — this is the fix for the degenerate
    // permutation tables the old prototype produced. MakeRng pins down the
    // seeding order; see TitanRandom.h.
    Pcg32 rng = MakeRng(seed);
    for (int i = 255; i > 0; --i) {
        uint32_t j = rng.NextRange(static_cast<uint32_t>(i + 1));
        std::swap(table[i], table[j]);
    }

    for (int i = 0; i < 512; ++i) m_Perm[i] = table[i & 255];
}

float SimplexNoise::Sample(float x, float y) const {
    // Skew input space to determine the containing simplex cell.
    float s = (x + y) * kF2;
    int i = static_cast<int>(std::floor(x + s));
    int j = static_cast<int>(std::floor(y + s));

    float t = static_cast<float>(i + j) * kG2;
    float x0 = x - (static_cast<float>(i) - t);
    float y0 = y - (static_cast<float>(j) - t);

    int i1 = (x0 > y0) ? 1 : 0;
    int j1 = 1 - i1;

    float x1 = x0 - static_cast<float>(i1) + kG2;
    float y1 = y0 - static_cast<float>(j1) + kG2;
    float x2 = x0 - 1.0f + 2.0f * kG2;
    float y2 = y0 - 1.0f + 2.0f * kG2;

    int ii = i & 255;
    int jj = j & 255;
    int gi0 = m_Perm[ii + m_Perm[jj]] & 7;
    int gi1 = m_Perm[ii + i1 + m_Perm[jj + j1]] & 7;
    int gi2 = m_Perm[ii + 1 + m_Perm[jj + 1]] & 7;

    float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f;

    float t0 = 0.5f - x0 * x0 - y0 * y0;
    if (t0 > 0.0f) {
        t0 *= t0;
        n0 = t0 * t0 * Dot2(kGrad2[gi0], x0, y0);
    }
    float t1 = 0.5f - x1 * x1 - y1 * y1;
    if (t1 > 0.0f) {
        t1 *= t1;
        n1 = t1 * t1 * Dot2(kGrad2[gi1], x1, y1);
    }
    float t2 = 0.5f - x2 * x2 - y2 * y2;
    if (t2 > 0.0f) {
        t2 *= t2;
        n2 = t2 * t2 * Dot2(kGrad2[gi2], x2, y2);
    }

    // Scale to roughly [-1, 1]. The constant is tied to the gradient set and
    // must be re-measured whenever that changes — cpp/tools/calibrate_noise.cpp.
    //
    // Calibrated to match the *standard deviation* of the previous mixed-length
    // table (0.441), not its peak. Matching the peak instead put sd 22% high,
    // which pushed ridged terrain's mean down and turned the Alpine Peaks
    // preset into spikes: peaks are rare outliers, sd is what governs how the
    // terrain actually reads, and existing seeds and presets should keep their
    // character across a bias fix.
    return 81.4f * (n0 + n1 + n2);
}

FractalNoise::FractalNoise(uint64_t seed, const FractalParams& params)
    : m_Base(seed),
      m_WarpX(seed ^ 0xA5D39EAB4C1F8E37ull),
      m_WarpY(seed ^ 0x3C6EF372FE94F82Aull),
      m_Params(params),
      m_CellSeed(seed ^ 0x94D049BB133111EBull) {}

namespace {

// Deterministic per-cell feature point for cellular noise.
inline void CellPoint(uint64_t seed, int cx, int cy, float& px, float& py) {
    uint64_t h = seed;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(cx)) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<uint64_t>(static_cast<uint32_t>(cy)) * 0xC2B2AE3D27D4EB4Full;
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 32;
    px = static_cast<float>(cx) + static_cast<float>(h & 0xFFFFu) / 65535.0f;
    py = static_cast<float>(cy) + static_cast<float>((h >> 16) & 0xFFFFu) / 65535.0f;
}

// Single-octave cellular distances F1 <= F2.
// metric: 0 = euclidean, 1 = manhattan, 2 = chebyshev.
inline void Cellular(uint64_t seed, float x, float y, int metric, float& f1, float& f2) {
    const int ix = static_cast<int>(std::floor(x));
    const int iy = static_cast<int>(std::floor(y));
    f1 = 1e9f;
    f2 = 1e9f;
    for (int j = -1; j <= 1; ++j) {
        for (int i = -1; i <= 1; ++i) {
            float px, py;
            CellPoint(seed, ix + i, iy + j, px, py);
            const float ax = std::fabs(px - x);
            const float ay = std::fabs(py - y);
            float d;
            switch (metric) {
                case 1:  d = ax + ay; break;
                case 2:  d = std::max(ax, ay); break;
                default: d = std::sqrt(ax * ax + ay * ay); break;
            }
            if (d < f1) {
                f2 = f1;
                f1 = d;
            } else if (d < f2) {
                f2 = d;
            }
        }
    }
}

} // namespace

float FractalNoise::SampleVoronoi(float x, float y, bool ridge, int metric) const {
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < m_Params.octaves; ++o) {
        float f1, f2;
        Cellular(m_CellSeed + static_cast<uint64_t>(o) * 0x632BE59BD9B4E019ull,
                 x * frequency, y * frequency, metric, f1, f2);
        // F1 in ~[0, 1.2]: cells rise toward centers. F2-F1: ridged walls.
        const float v = ridge ? std::clamp((f2 - f1), 0.0f, 1.0f)
                              : std::clamp(1.0f - f1, 0.0f, 1.0f);
        sum += v * amplitude;
        norm += amplitude;
        amplitude *= m_Params.persistence;
        frequency *= m_Params.lacunarity;
    }
    return norm > 0.0f ? std::clamp(sum / norm, 0.0f, 1.0f) : 0.0f;
}

float FractalNoise::Fbm(const SimplexNoise& noise, float x, float y, int octaves) const {
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < octaves; ++o) {
        sum += noise.Sample(x * frequency, y * frequency) * amplitude;
        norm += amplitude;
        amplitude *= m_Params.persistence;
        frequency *= m_Params.lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f; // [-1, 1]
}

float FractalNoise::SampleStandard(float x, float y) const {
    return (Fbm(m_Base, x, y, m_Params.octaves) + 1.0f) * 0.5f;
}

float FractalNoise::SampleBillow(float x, float y) const {
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;
    for (int o = 0; o < m_Params.octaves; ++o) {
        float n = std::fabs(m_Base.Sample(x * frequency, y * frequency));
        sum += n * amplitude;
        norm += amplitude;
        amplitude *= m_Params.persistence;
        frequency *= m_Params.lacunarity;
    }
    return norm > 0.0f ? sum / norm : 0.0f; // |noise| is already [0, 1]
}

float FractalNoise::SampleRidged(float x, float y) const {
    // Musgrave ridged multifractal: each octave is weighted by the previous
    // octave's signal, so ridges accumulate detail while valleys stay smooth.
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float weight = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;

    for (int o = 0; o < m_Params.octaves; ++o) {
        float n = m_Base.Sample(x * frequency, y * frequency);
        float signal = m_Params.ridgeOffset - std::fabs(n);
        signal *= signal;
        signal *= weight;

        weight = std::clamp(signal * m_Params.ridgeGain, 0.0f, 1.0f);

        sum += signal * amplitude;
        norm += amplitude;
        amplitude *= m_Params.persistence;
        frequency *= m_Params.lacunarity;
    }

    float maxSignal = m_Params.ridgeOffset * m_Params.ridgeOffset;
    if (norm > 0.0f && maxSignal > 0.0f) {
        return std::clamp(sum / (norm * maxSignal), 0.0f, 1.0f);
    }
    return 0.0f;
}

float FractalNoise::SampleHybrid(float x, float y) const {
    // Musgrave hybrid multifractal ("terrain" mode): the running product
    // weight suppresses detail in valleys while peaks accumulate roughness.
    const float offset = 0.7f;
    float amplitude = 1.0f;
    float frequency = 1.0f;
    float weight = 1.0f;
    float sum = 0.0f;
    float norm = 0.0f;

    for (int o = 0; o < m_Params.octaves; ++o) {
        weight = std::clamp(weight, 0.0f, 1.0f);
        const float signal = (m_Base.Sample(x * frequency, y * frequency) + offset) * amplitude;
        sum += weight * signal;
        norm += weight * amplitude * (1.0f + offset);
        weight *= signal;
        amplitude *= m_Params.persistence;
        frequency *= m_Params.lacunarity;
    }
    return norm > 0.0f ? std::clamp(sum / norm, 0.0f, 1.0f) : 0.0f;
}

float FractalNoise::Sample(float x, float y) const {
    if (m_Params.type == NoiseType::None) return 0.0f;

    if (m_Params.warpStrength > 0.0f) {
        float wf = m_Params.warpFrequency;
        int warpOctaves = std::min(4, m_Params.octaves);
        float wx = Fbm(m_WarpX, x * wf, y * wf, warpOctaves);
        float wy = Fbm(m_WarpY, x * wf, y * wf, warpOctaves);
        x += wx * m_Params.warpStrength;
        y += wy * m_Params.warpStrength;
    }

    switch (m_Params.type) {
        case NoiseType::Ridged:          return SampleRidged(x, y);
        case NoiseType::Billow:          return SampleBillow(x, y);
        case NoiseType::Voronoi:         return SampleVoronoi(x, y, false, 0);
        case NoiseType::VoronoiRidge:    return SampleVoronoi(x, y, true, 0);
        case NoiseType::WorleyManhattan: return SampleVoronoi(x, y, false, 1);
        case NoiseType::WorleyChebyshev: return SampleVoronoi(x, y, false, 2);
        case NoiseType::HybridMulti:     return SampleHybrid(x, y);
        case NoiseType::Standard:
        default:                         return SampleStandard(x, y);
    }
}

} // namespace Titan
