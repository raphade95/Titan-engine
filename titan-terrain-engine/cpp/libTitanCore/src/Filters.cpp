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

// Filter radii are world distances converted to cells.
//
// They were plain cell counts, so "blur radius 4" smoothed four samples' worth
// of terrain — a different physical distance at every resolution, and a
// different amount of the landscape. cellSize 1.0 is exactly identity.
void TerrainEngine::ApplyBlur(float radius, float strength) {
    const float cs = m_Params.cellSize > 0.0f ? m_Params.cellSize : 1.0f;
    const int r = static_cast<int>(std::lround(radius / cs));
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
    const float cs = m_Params.cellSize > 0.0f ? m_Params.cellSize : 1.0f;
    const int r = std::max(1, static_cast<int>(std::lround(radius / cs)));
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

namespace {

// Monotone piecewise-cubic Hermite (Fritsch-Carlson slopes), the one
// definition of Titan's height-remap curve.
//
// Extracted so the curve editors in both apps can *draw* exactly what the
// engine will *apply*, via titan_sample_curve, rather than each
// reimplementing the spline. This codebase has already paid for duplicating a
// formula three ways once — the mask band curve lived in C++, TypeScript and
// Swift simultaneously and had to agree forever.
//
// Control points must be sorted by x; inputs outside [x0, xn] clamp.
class MonotoneCurve {
public:
    MonotoneCurve(const float* xs, const float* ys, int count)
        : m_Xs(xs), m_Ys(ys), m_N(count), m_Slope(count, 0.0f) {
        std::vector<float> secant(count - 1, 0.0f);
        for (int i = 0; i < count - 1; ++i) {
            const float dx = std::max(1e-6f, xs[i + 1] - xs[i]);
            secant[i] = (ys[i + 1] - ys[i]) / dx;
        }
        m_Slope[0] = secant[0];
        m_Slope[count - 1] = secant[count - 2];
        for (int i = 1; i < count - 1; ++i) {
            m_Slope[i] = (secant[i - 1] * secant[i] <= 0.0f)
                ? 0.0f : 0.5f * (secant[i - 1] + secant[i]);
        }
        for (int i = 0; i < count - 1; ++i) {
            if (secant[i] == 0.0f) {
                m_Slope[i] = m_Slope[i + 1] = 0.0f;
            } else {
                const float a = m_Slope[i] / secant[i];
                const float b = m_Slope[i + 1] / secant[i];
                const float sum = a * a + b * b;
                if (sum > 9.0f) {
                    const float tau = 3.0f / std::sqrt(sum);
                    m_Slope[i] = tau * a * secant[i];
                    m_Slope[i + 1] = tau * b * secant[i];
                }
            }
        }
    }

    float Evaluate(float t) const {
        if (t <= m_Xs[0]) return m_Ys[0];
        if (t >= m_Xs[m_N - 1]) return m_Ys[m_N - 1];
        int seg = 0;
        while (seg < m_N - 2 && t > m_Xs[seg + 1]) ++seg;
        const float dx = std::max(1e-6f, m_Xs[seg + 1] - m_Xs[seg]);
        const float u = (t - m_Xs[seg]) / dx;
        const float u2 = u * u;
        const float u3 = u2 * u;
        return m_Ys[seg] * (2 * u3 - 3 * u2 + 1)
             + m_Slope[seg] * dx * (u3 - 2 * u2 + u)
             + m_Ys[seg + 1] * (-2 * u3 + 3 * u2)
             + m_Slope[seg + 1] * dx * (u3 - u2);
    }

private:
    const float* m_Xs;
    const float* m_Ys;
    int m_N;
    std::vector<float> m_Slope;
};

} // namespace

void TerrainEngine::ApplyCurve(const float* xs, const float* ys, int count) {
    if (!xs || !ys || count < 2) return;
    const size_t cells = m_Bedrock.size();
    float minH, maxH;
    CollectHeightRange(minH, maxH);
    const float range = maxH - minH;
    if (range <= 0.0f) return;

    const MonotoneCurve curve(xs, ys, count);

    std::vector<float> total(cells);
    for (size_t i = 0; i < cells; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        const float t = (h - minH) / range;
        const float remapped = minH + std::clamp(curve.Evaluate(t), 0.0f, 1.0f) * range;
        total[i] = h + (remapped - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

// Samples the same curve the remap applies, evenly across [0,1]. This is what
// the editors draw, so a preview can never disagree with the result.
void TerrainEngine::SampleCurve(const float* xs, const float* ys, int count,
                                float* out, int samples) {
    if (!xs || !ys || !out || count < 2 || samples < 2) return;
    const MonotoneCurve curve(xs, ys, count);
    for (int i = 0; i < samples; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(samples - 1);
        out[i] = std::clamp(curve.Evaluate(t), 0.0f, 1.0f);
    }
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
