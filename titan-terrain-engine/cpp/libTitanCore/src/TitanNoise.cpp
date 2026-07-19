#include "TitanNoise.h"
#include "TitanRandom.h"

#include <algorithm>
#include <cmath>

namespace Titan {

namespace {

const float kGrad2[12][2] = {
    {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
    {1, 0}, {-1, 0}, {1, 0},  {-1, 0},
    {0, 1}, {0, -1}, {0, 1},  {0, -1}
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
    // permutation tables the old prototype produced.
    uint64_t state = seed;
    Pcg32 rng(SplitMix64(state), SplitMix64(state));
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
    int gi0 = m_Perm[ii + m_Perm[jj]] % 12;
    int gi1 = m_Perm[ii + i1 + m_Perm[jj + j1]] % 12;
    int gi2 = m_Perm[ii + 1 + m_Perm[jj + 1]] % 12;

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

    // Scale to roughly [-1, 1].
    return 70.0f * (n0 + n1 + n2);
}

FractalNoise::FractalNoise(uint64_t seed, const FractalParams& params)
    : m_Base(seed),
      m_WarpX(seed ^ 0xA5D39EAB4C1F8E37ull),
      m_WarpY(seed ^ 0x3C6EF372FE94F82Aull),
      m_Params(params) {}

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
        case NoiseType::Ridged:   return SampleRidged(x, y);
        case NoiseType::Billow:   return SampleBillow(x, y);
        case NoiseType::Standard:
        default:                  return SampleStandard(x, y);
    }
}

} // namespace Titan
