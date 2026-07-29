// v0.5 filters, combiners, and mask generators.
//
// Every operation here is deterministic, respects the active mask (which
// scales the per-cell effect), and re-splits the edited height 80/20 into
// bedrock/sediment like the other direct height edits.

#include "TitanCore.h"
#include "TitanNoise.h"

#include <algorithm>
#include <cmath>

namespace Titan {

namespace {

// Separable box blur over a square grid, run twice for a smoother
// (triangular) kernel profile.
void BoxBlur(std::vector<float>& field, int size, int radius) {
    if (radius < 1) return;
    std::vector<float> tmp(field.size());

    for (int pass = 0; pass < 2; ++pass) {
        // Horizontal.
        for (int y = 0; y < size; ++y) {
            const float* row = field.data() + static_cast<size_t>(y) * size;
            float* out = tmp.data() + static_cast<size_t>(y) * size;
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                int n = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int px = x + k;
                    if (px < 0 || px >= size) continue;
                    sum += row[px];
                    ++n;
                }
                out[x] = sum / static_cast<float>(n);
            }
        }
        // Vertical.
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                int n = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int py = y + k;
                    if (py < 0 || py >= size) continue;
                    sum += tmp[static_cast<size_t>(py) * size + x];
                    ++n;
                }
                field[static_cast<size_t>(y) * size + x] = sum / static_cast<float>(n);
            }
        }
    }
}

float Smoothstep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace

// ---------------------------------------------------------------------------
// Height filters
// ---------------------------------------------------------------------------

void TerrainEngine::ApplyClamp(float minH, float maxH) {
    if (maxH < minH) std::swap(minH, maxH);
    const size_t count = m_Bedrock.size();
    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        const float clamped = std::clamp(h, minH, maxH);
        total[i] = h + (clamped - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

void TerrainEngine::ApplyTransform(float scaleV, float offset, bool invert) {
    const size_t count = m_Bedrock.size();
    float minH, maxH;
    CollectHeightRange(minH, maxH);

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        float v = invert ? (maxH - h + minH) : h;
        v = v * scaleV + offset;
        // No zero clamp: terrain is allowed below the datum (see ResplitHeight).
        total[i] = h + (v - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

void TerrainEngine::ApplyBlur(float radius, float strength) {
    const int r = static_cast<int>(std::lround(radius));
    if (r < 1 || strength <= 0.0f) return;
    strength = std::clamp(strength, 0.0f, 1.0f);
    const size_t count = m_Bedrock.size();

    std::vector<float> blurred(count);
    for (size_t i = 0; i < count; ++i) blurred[i] = m_Bedrock[i] + m_Sediment[i];
    BoxBlur(blurred, m_Params.size, r);

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        total[i] = h + (blurred[i] - h) * strength * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

void TerrainEngine::ApplySharpen(float radius, float strength) {
    const int r = std::max(1, static_cast<int>(std::lround(radius)));
    if (strength <= 0.0f) return;
    const size_t count = m_Bedrock.size();

    std::vector<float> blurred(count);
    for (size_t i = 0; i < count; ++i) blurred[i] = m_Bedrock[i] + m_Sediment[i];
    BoxBlur(blurred, m_Params.size, r);

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        const float sharpened = h + (h - blurred[i]) * strength;
        total[i] = h + (sharpened - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

void TerrainEngine::ApplyCurve(const float* xs, const float* ys, int count) {
    if (!xs || !ys || count < 2) return;
    const size_t cells = m_Bedrock.size();
    float minH, maxH;
    CollectHeightRange(minH, maxH);
    const float range = maxH - minH;
    if (range <= 0.0f) return;

    // Monotone piecewise-cubic Hermite (Fritsch–Carlson slopes). Control
    // points are assumed sorted by x; inputs outside [x0, xn] clamp.
    const int n = count;
    std::vector<float> slope(n, 0.0f);
    std::vector<float> secant(n - 1, 0.0f);
    for (int i = 0; i < n - 1; ++i) {
        const float dx = std::max(1e-6f, xs[i + 1] - xs[i]);
        secant[i] = (ys[i + 1] - ys[i]) / dx;
    }
    slope[0] = secant[0];
    slope[n - 1] = secant[n - 2];
    for (int i = 1; i < n - 1; ++i) {
        slope[i] = (secant[i - 1] * secant[i] <= 0.0f)
            ? 0.0f : 0.5f * (secant[i - 1] + secant[i]);
    }
    for (int i = 0; i < n - 1; ++i) {
        if (secant[i] == 0.0f) {
            slope[i] = slope[i + 1] = 0.0f;
        } else {
            const float a = slope[i] / secant[i];
            const float b = slope[i + 1] / secant[i];
            const float s = a * a + b * b;
            if (s > 9.0f) {
                const float tau = 3.0f / std::sqrt(s);
                slope[i] = tau * a * secant[i];
                slope[i + 1] = tau * b * secant[i];
            }
        }
    }

    auto evaluate = [&](float t) -> float {
        if (t <= xs[0]) return ys[0];
        if (t >= xs[n - 1]) return ys[n - 1];
        int seg = 0;
        while (seg < n - 2 && t > xs[seg + 1]) ++seg;
        const float dx = std::max(1e-6f, xs[seg + 1] - xs[seg]);
        const float u = (t - xs[seg]) / dx;
        const float u2 = u * u;
        const float u3 = u2 * u;
        return ys[seg] * (2 * u3 - 3 * u2 + 1)
             + slope[seg] * dx * (u3 - 2 * u2 + u)
             + ys[seg + 1] * (-2 * u3 + 3 * u2)
             + slope[seg + 1] * dx * (u3 - u2);
    };

    std::vector<float> total(cells);
    for (size_t i = 0; i < cells; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        const float t = (h - minH) / range;
        const float remapped = minH + std::clamp(evaluate(t), 0.0f, 1.0f) * range;
        total[i] = h + (remapped - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

// ---------------------------------------------------------------------------
// General combiner / heightfield import
// ---------------------------------------------------------------------------

void TerrainEngine::ApplyHeightfield(const float* data, int srcSize,
                                     float heightScale, int blendMode,
                                     float alpha) {
    if (!data || srcSize < 2) return;
    const int size = m_Params.size;
    const size_t count = m_Bedrock.size();
    const auto mode = static_cast<BlendMode>(blendMode);

    // Multiply blends by the field's own normalized value so absolute and
    // 0..1-normalized sources behave identically.
    float maxField = 0.0f;
    for (int i = 0; i < srcSize * srcSize; ++i) {
        maxField = std::max(maxField, data[i] * heightScale);
    }

    auto sampleSrc = [&](float fx, float fy) -> float {
        const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, srcSize - 1);
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, srcSize - 1);
        const int x1 = std::min(x0 + 1, srcSize - 1);
        const int y1 = std::min(y0 + 1, srcSize - 1);
        const float u = std::clamp(fx - static_cast<float>(x0), 0.0f, 1.0f);
        const float v = std::clamp(fy - static_cast<float>(y0), 0.0f, 1.0f);
        return data[y0 * srcSize + x0] * (1 - u) * (1 - v)
             + data[y0 * srcSize + x1] * u * (1 - v)
             + data[y1 * srcSize + x0] * (1 - u) * v
             + data[y1 * srcSize + x1] * u * v;
    };

    const float step = static_cast<float>(srcSize - 1) / static_cast<float>(size - 1);

    std::vector<float> total(count);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t i = static_cast<size_t>(Index(x, y));
            const float field = sampleSrc(x * step, y * step) * heightScale;
            const float n = maxField > 0.0f ? std::clamp(field / maxField, 0.0f, 1.0f) : 0.0f;

            const float h = m_Bedrock[i] + m_Sediment[i];
            float combined;
            switch (mode) {
                case BlendMode::Subtract: combined = h - field; break;
                case BlendMode::Multiply: combined = h * n; break;
                case BlendMode::Max:      combined = std::max(h, field); break;
                case BlendMode::Min:      combined = std::min(h, field); break;
                case BlendMode::Mix:      combined = h + (field - h) * alpha; break;
                case BlendMode::Add:
                default:                  combined = h + field; break;
            }
            total[i] = h + (combined - h) * MaskAt(static_cast<int>(i));
        }
    }
    ResplitHeight(total);
}

// ---------------------------------------------------------------------------
// Derived maps (into scratch)
// ---------------------------------------------------------------------------

void TerrainEngine::ComputeSlopeMap() {
    const int size = m_Params.size;
    m_Scratch.resize(static_cast<size_t>(size) * size);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            m_Scratch[static_cast<size_t>(Index(x, y))] = GetSlope(x, y);
        }
    }
}

void TerrainEngine::ComputeCurvatureMap() {
    const int size = m_Params.size;
    m_Scratch.resize(static_cast<size_t>(size) * size);
    const float c2 = m_Params.cellSize * m_Params.cellSize;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float lap = (GetHeight(x - 1, y) + GetHeight(x + 1, y)
                             + GetHeight(x, y - 1) + GetHeight(x, y + 1)
                             - 4.0f * GetHeight(x, y)) / c2;
            m_Scratch[static_cast<size_t>(Index(x, y))] = lap;
        }
    }
}

// ---------------------------------------------------------------------------
// Mask generators
// ---------------------------------------------------------------------------

void TerrainEngine::MaskByFeature(int feature, float rangeLo, float rangeHi,
                                  float softness, bool invert) {
    const int size = m_Params.size;
    m_Scratch.resize(static_cast<size_t>(size) * size);
    const float s = std::max(1e-4f, softness);
    const float heightRef = std::max(1.0f, m_Params.heightMultiplier);
    const float pi = 3.14159265358979323846f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float v;
            switch (static_cast<MaskFeature>(feature)) {
                case MaskFeature::Slope:
                    v = std::atan(GetSlope(x, y)) / (pi * 0.5f);
                    break;
                case MaskFeature::Curvature: {
                    // The raw Laplacian scales with the terrain's relief, so a
                    // fixed tanh gain saturated into a binary mask at large
                    // height scales. Normalising by heightRef keeps the mask's
                    // response the same at any scale; 30/heightRef reproduces
                    // the original 0.75 gain at the default of 40.
                    const float raw = GetHeight(x - 1, y) + GetHeight(x + 1, y)
                                    + GetHeight(x, y - 1) + GetHeight(x, y + 1)
                                    - 4.0f * GetHeight(x, y);
                    v = 0.5f + 0.5f * std::tanh(raw * 30.0f / heightRef);
                    break;
                }
                case MaskFeature::Height:
                default:
                    v = std::clamp(GetHeight(x, y) / heightRef, 0.0f, 1.0f);
                    break;
            }
            // Band with soft edges: rises over [lo-s, lo], falls over [hi, hi+s].
            float m = Smoothstep01((v - (rangeLo - s)) / s)
                    * (1.0f - Smoothstep01((v - rangeHi) / s));
            if (invert) m = 1.0f - m;
            m_Scratch[static_cast<size_t>(Index(x, y))] = m;
        }
    }
}

void TerrainEngine::NoiseToScratch(const NoiseLayerParams& p) {
    const int size = m_Params.size;
    m_Scratch.resize(static_cast<size_t>(size) * size);

    FractalParams fp;
    fp.type = static_cast<NoiseType>(p.noiseType);
    fp.octaves = p.octaves;
    fp.persistence = p.persistence;
    fp.lacunarity = p.lacunarity;
    fp.ridgeOffset = p.ridgeOffset;
    fp.ridgeGain = p.ridgeGain;
    fp.warpStrength = p.warpStrength;

    const uint64_t seed = static_cast<uint64_t>(m_Params.seed)
        + (static_cast<uint64_t>(p.seedOffset) << 32 | p.seedOffset);
    const FractalNoise noise(seed, fp);

    const float extent = static_cast<float>(size) * m_Params.cellSize;
    const float frequency = extent > 0.0f ? p.scale / extent : 0.0f;

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float worldX = m_Params.originX + (static_cast<float>(x) - size * 0.5f) * m_Params.cellSize;
            const float worldY = m_Params.originY + (static_cast<float>(y) - size * 0.5f) * m_Params.cellSize;
            const float n = noise.Sample(worldX * frequency, worldY * frequency);
            m_Scratch[static_cast<size_t>(Index(x, y))] =
                std::pow(std::clamp(n, 0.0f, 1.0f), p.exponent);
        }
    }
}

void TerrainEngine::BandScratch(float lo, float hi, float softness, bool invert) {
    const float s = std::max(1e-4f, softness);
    for (float& v : m_Scratch) {
        float m = Smoothstep01((v - (lo - s)) / s) * (1.0f - Smoothstep01((v - hi) / s));
        if (invert) m = 1.0f - m;
        v = std::clamp(m, 0.0f, 1.0f);
    }
}

void TerrainEngine::SetMaskFromScratch() {
    m_Mask = m_Scratch;
    for (float& v : m_Mask) v = std::clamp(v, 0.0f, 1.0f);
}

} // namespace Titan
