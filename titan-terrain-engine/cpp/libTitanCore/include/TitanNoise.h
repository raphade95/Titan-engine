#pragma once

#include <cstdint>

namespace Titan {

enum class NoiseType : int {
    None = 0,
    Standard = 1,     // fBm simplex
    Ridged = 2,       // Musgrave ridged multifractal
    Billow = 3,
    Voronoi = 4,      // fractal cellular F1 (plateau cells)
    VoronoiRidge = 5  // fractal cellular F2-F1 (ridged cell walls)
};

// 2D simplex noise (Gustavson variant) with a permutation table shuffled by a
// properly seeded PRNG, so every seed yields a well-mixed, distinct field.
class SimplexNoise {
public:
    explicit SimplexNoise(uint64_t seed);

    // Returns approximately [-1, 1].
    float Sample(float x, float y) const;

private:
    uint8_t m_Perm[512];
};

struct FractalParams {
    NoiseType type = NoiseType::Standard;
    int octaves = 6;
    float persistence = 0.5f;
    float lacunarity = 2.0f;

    // Ridged multifractal shape controls.
    float ridgeOffset = 1.0f;
    float ridgeGain = 2.0f;

    // Domain warp: displaces sample coordinates by a secondary fBm field.
    // Strength is in noise-space units (0 disables), frequency is relative
    // to the base sample frequency.
    float warpStrength = 0.0f;
    float warpFrequency = 0.5f;
};

// Combines a base noise with two warp channels into a single [0, 1] field.
class FractalNoise {
public:
    FractalNoise(uint64_t seed, const FractalParams& params);

    // Returns [0, 1].
    float Sample(float x, float y) const;

private:
    float Fbm(const SimplexNoise& noise, float x, float y, int octaves) const;
    float SampleStandard(float x, float y) const;
    float SampleBillow(float x, float y) const;
    float SampleRidged(float x, float y) const;
    float SampleVoronoi(float x, float y, bool ridge) const;

    SimplexNoise m_Base;
    SimplexNoise m_WarpX;
    SimplexNoise m_WarpY;
    FractalParams m_Params;
    uint64_t m_CellSeed;
};

} // namespace Titan
