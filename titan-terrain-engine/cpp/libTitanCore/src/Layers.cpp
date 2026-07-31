// v0.4 layer operations: masks, noise stacking, shape stamps, snow, water.
//
// Everything here is deterministic and respects the active mask, which
// multiplies the effect of each operation per cell (1 = full effect).

#include "TitanCore.h"
#include "TitanNoise.h"

#include <algorithm>
#include <cmath>
#include <queue>

namespace Titan {

// ---------------------------------------------------------------------------
// Mask plumbing
// ---------------------------------------------------------------------------

void TerrainEngine::SetMask(const float* data, int size) {
    if (!data || size != m_Params.size) {
        m_Mask.clear();
        return;
    }
    m_Mask.assign(data, data + static_cast<size_t>(size) * size);
}

void TerrainEngine::ClearMask() {
    m_Mask.clear();
}

float TerrainEngine::SampleMask(float x, float y) const {
    if (m_Mask.empty()) return 1.0f;
    const int size = m_Params.size;
    const int nx = static_cast<int>(std::floor(x));
    const int ny = static_cast<int>(std::floor(y));
    const float u = x - nx;
    const float v = y - ny;
    auto at = [&](int px, int py) -> float {
        px = std::clamp(px, 0, size - 1);
        py = std::clamp(py, 0, size - 1);
        return m_Mask[static_cast<size_t>(py) * size + px];
    };
    return at(nx, ny) * (1 - u) * (1 - v) +
           at(nx + 1, ny) * u * (1 - v) +
           at(nx, ny + 1) * (1 - u) * v +
           at(nx + 1, ny + 1) * u * v;
}

// Writes a new total height back into the two-layer model.
//
// The ground model is: total = bedrock + sediment, where sediment is a
// deposit and therefore never negative, while bedrock is an elevation and may
// go below the zero datum (ocean floors, sub-sea-level basins, canyons cut
// below the reference plane).
//
// Where a cell already has material, the existing bedrock/sediment ratio is
// *preserved* rather than reset. This used to force a flat 80/20 split, which
// silently destroyed erosion stratigraphy: a Blur at strength 0.05 after a
// hydraulic pass threw away the entire sediment distribution that pass had
// just computed, changed how the next erosion layer behaved (HardnessAt reads
// bedrock elevation), and zeroed the mesh's sediment splat channel. In a
// layer stack whose passes users freely reorder, that was silent,
// order-dependent data loss.
//
// 80/20 remains the fallback for cells that had nothing to preserve.
void TerrainEngine::ResplitHeight(const std::vector<float>& newTotal) {
    const size_t count = m_Bedrock.size();
    for (size_t i = 0; i < count; ++i) {
        const float h = newTotal[i];
        const float oldTotal = m_Bedrock[i] + m_Sediment[i];

        if (h <= 0.0f) {
            // At or below the datum there is no room for a deposit.
            m_Bedrock[i] = h;
            m_Sediment[i] = 0.0f;
            continue;
        }
        if (oldTotal > 0.0f) {
            const float ratio = h / oldTotal;
            m_Bedrock[i] *= ratio;
            m_Sediment[i] *= ratio;
            // A negative bedrock scaled by a positive ratio stays negative,
            // which would put sediment above a total it cannot support.
            if (m_Sediment[i] < 0.0f) {
                m_Bedrock[i] = h;
                m_Sediment[i] = 0.0f;
            }
        } else {
            m_Bedrock[i] = h * 0.8f;
            m_Sediment[i] = h * 0.2f;
        }
    }
}

void TerrainEngine::ClearTerrain() {
    std::fill(m_Bedrock.begin(), m_Bedrock.end(), 0.0f);
    std::fill(m_Sediment.begin(), m_Sediment.end(), 0.0f);
    std::fill(m_Flow.begin(), m_Flow.end(), 0.0f);
    std::fill(m_Snow.begin(), m_Snow.end(), 0.0f);
    std::fill(m_Water.begin(), m_Water.end(), 0.0f);
    ClearLava();
    m_DropletCursor = 0;
    m_MassExported = 0.0;
    m_MassCreated = 0.0;
}

void TerrainEngine::ReadHeight(float* out, int count) const {
    if (!out) return;
    const int n = std::min<int>(count, static_cast<int>(m_Bedrock.size()));
    for (int i = 0; i < n; ++i) out[i] = m_Bedrock[i] + m_Sediment[i];
}

void TerrainEngine::SetHeight(const float* data, int count) {
    if (!data) return;
    const int n = std::min<int>(count, static_cast<int>(m_Bedrock.size()));
    ClearTerrain();
    for (int i = 0; i < n; ++i) {
        // A non-finite sample would spread through every neighbourhood
        // operation downstream, so it stops here.
        m_Bedrock[i] = std::isfinite(data[i]) ? data[i] : 0.0f;
    }
}

// ---------------------------------------------------------------------------
// Noise stacking
// ---------------------------------------------------------------------------

void TerrainEngine::ApplyNoise(const NoiseLayerParams& p) {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;

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
    const auto mode = static_cast<BlendMode>(p.blendMode);

    std::vector<float> total(count);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t i = static_cast<size_t>(Index(x, y));
            const float worldX = m_Params.originX + (static_cast<float>(x) - size * 0.5f) * m_Params.cellSize;
            const float worldY = m_Params.originY + (static_cast<float>(y) - size * 0.5f) * m_Params.cellSize;

            float n = noise.Sample(worldX * frequency, worldY * frequency); // [0,1]
            n = std::pow(std::clamp(n, 0.0f, 1.0f), p.exponent);
            const float field = n * p.amplitude;

            const float h = m_Bedrock[i] + m_Sediment[i];
            float combined;
            switch (mode) {
                case BlendMode::Subtract: combined = h - field; break;
                case BlendMode::Multiply: combined = h * n; break;
                case BlendMode::Max:      combined = std::max(h, field); break;
                case BlendMode::Min:      combined = std::min(h, field); break;
                case BlendMode::Mix:      combined = h + (field - h) * p.blendAlpha; break;
                case BlendMode::Add:
                default:                  combined = h + field; break;
            }
            total[i] = h + (combined - h) * MaskAt(static_cast<int>(i));
        }
    }
    ResplitHeight(total);
}

// ---------------------------------------------------------------------------
// Shape stamps
// ---------------------------------------------------------------------------

namespace {

// Normalized influence field for a stamp at local coordinates u, v in
// [-1, 1] (already rotated/scaled). Returns 0..1.
float StampField(StampShape shape, float u, float v, float falloff) {
    const float f = std::clamp(falloff, 0.001f, 1.0f);

    switch (shape) {
        case StampShape::Rectangle: {
            const float du = std::max(0.0f, std::fabs(u) - (1.0f - f));
            const float dv = std::max(0.0f, std::fabs(v) - (1.0f - f));
            const float t = std::max(du, dv) / f;
            if (t >= 1.0f) return 0.0f;
            const float s = 1.0f - t;
            return s * s * (3.0f - 2.0f * s);
        }
        case StampShape::Ridge: {
            // Long axis = u; profile falls off across v with a soft crest.
            if (std::fabs(u) > 1.0f || std::fabs(v) > 1.0f) return 0.0f;
            const float across = std::max(0.0f, 1.0f - std::fabs(v));
            const float along = std::min(1.0f, (1.0f - std::fabs(u)) / f);
            const float crest = across * across * (3.0f - 2.0f * across);
            const float cap = std::clamp(along, 0.0f, 1.0f);
            return crest * cap * cap * (3.0f - 2.0f * cap);
        }
        case StampShape::Crater: {
            const float r = std::sqrt(u * u + v * v);
            if (r >= 1.0f) return 0.0f;
            // Rim peaks at r=0.72, bowl dips below zero in the middle —
            // remapped to [0,1] with the bowl at 0 and the rim at 1.
            //
            // Squared explicitly rather than via pow(x, 2.0f): compilers
            // strength-reduce that call at -O2 and up but not at -O0, and
            // powf(x, 2) is not always bit-identical to x * x, which made the
            // stamp field depend on the optimization level.
            const float rimT = (r - 0.72f) / (0.18f + 0.3f * f);
            const float bowlT = r / 0.45f;
            const float rim = std::exp(-(rimT * rimT));
            const float bowl = std::exp(-(bowlT * bowlT));
            return std::clamp(rim - bowl * 0.9f + 0.15f * (1.0f - r), 0.0f, 1.0f);
        }
        case StampShape::Gradient: {
            // Linear ramp along +u; v unbounded so a rotated gradient can
            // cover the whole map. Falloff eases the ends of the ramp,
            // continuously: 0 is a straight linear ramp, 1 is a full
            // smoothstep, values between blend the two.
            //
            // This used to read `f < 0.999f ? t : smoothstep(t)`, which is
            // backwards from the documented meaning of falloff and is a binary
            // switch rather than a control — and since both UIs hard-code
            // falloff to 0.5, the parameter did nothing at all for gradients.
            const float t = std::clamp((u + 1.0f) * 0.5f, 0.0f, 1.0f);
            const float eased = t * t * (3.0f - 2.0f * t);
            return t + (eased - t) * f;
        }
        case StampShape::RadialGradient: {
            const float r = std::sqrt(u * u + v * v);
            return std::max(0.0f, 1.0f - r);
        }
        case StampShape::Dome:
        default: {
            const float r = std::sqrt(u * u + v * v);
            if (r >= 1.0f) return 0.0f;
            const float core = 1.0f - f;
            if (r <= core) return 1.0f;
            const float t = (r - core) / f;
            const float s = 1.0f - t;
            return s * s * (3.0f - 2.0f * s);
        }
    }
}

} // namespace

void TerrainEngine::StampToScratch(const StampParams& p) {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;
    m_Scratch.assign(count, 0.0f);

    const float pi = 3.14159265358979323846f;
    const float rot = -p.rotationDeg * pi / 180.0f;
    const float cosR = std::cos(rot);
    const float sinR = std::sin(rot);
    const float invSX = p.sizeX > 0.001f ? 1.0f / p.sizeX : 0.0f;
    const float invSY = p.sizeY > 0.001f ? 1.0f / p.sizeY : 0.0f;
    const auto shape = static_cast<StampShape>(p.shape);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) - p.centerX;
            const float dy = static_cast<float>(y) - p.centerY;
            const float u = (dx * cosR - dy * sinR) * invSX;
            const float v = (dx * sinR + dy * cosR) * invSY;
            m_Scratch[static_cast<size_t>(Index(x, y))] = StampField(shape, u, v, p.falloff);
        }
    }
}

void TerrainEngine::ApplyStamp(const StampParams& p) {
    StampToScratch(p);
    const size_t count = m_Bedrock.size();
    const auto op = static_cast<StampOp>(p.op);

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float s = m_Scratch[i];
        const float h = m_Bedrock[i] + m_Sediment[i];
        float combined;
        switch (op) {
            case StampOp::Lower:   combined = h - s * p.height; break;
            case StampOp::Flatten: combined = h + (p.height - h) * s; break;
            case StampOp::Union:   combined = std::max(h, s * p.height); break;
            case StampOp::Raise:
            default:               combined = h + s * p.height; break;
        }
        total[i] = h + (combined - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

// ---------------------------------------------------------------------------
// Snow
// ---------------------------------------------------------------------------

void TerrainEngine::ApplySnow(const SnowParams& p) {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;
    if (m_Snow.size() != count) m_Snow.assign(count, 0.0f);

    const float pi = 3.14159265358979323846f;
    const float maxSlope = std::tan(p.maxSlopeDeg * pi / 180.0f);
    const float heightRef = std::max(1.0f, m_Params.heightMultiplier);

    // 1. Accumulation: altitude band above the snow line, shed by slope.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int i = Index(x, y);
            const float h = m_Bedrock[i] + m_Sediment[i];
            const float alt = h / heightRef;

            float band = (alt - p.snowLine) / std::max(0.05f, 1.0f - p.snowLine);
            band = std::clamp(band, 0.0f, 1.0f);
            band = band * band * (3.0f - 2.0f * band);

            const float slope = GetSlope(x, y);
            const float shed = std::clamp(1.0f - slope / std::max(0.1f, maxSlope), 0.0f, 1.0f);

            // Assign toward the target rather than accumulate: running the
            // layer twice used to double the snowpack, which is wrong for a
            // stack that re-evaluates. The mask still gates the change, so a
            // masked-out cell keeps whatever it already had.
            //
            // Written as a lerp, not as `snow += (target - snow) * mask`.
            // Those are the same number in exact arithmetic and different in
            // float: at mask 1 the increment form evaluates S + (target - S),
            // which lands back on target for only ~77% of float pairs, so a
            // second run of the layer moved the snowpack by an ulp and the
            // idempotence test failed. It failed only on Linux and Windows,
            // because the values reaching this line come through std::tan and
            // libm is not bit-portable — macOS happened to draw from the 77%.
            // The lerp is exact at mask 0 and mask 1 by construction.
            const float target = p.amount * band * shed;
            const size_t si = static_cast<size_t>(i);
            const float mask = MaskAt(i);
            m_Snow[si] = target * mask + m_Snow[si] * (1.0f - mask);
        }
    }

    // 2. Settle: snow creeps downhill on the combined (terrain + snow)
    //    surface with a gentle angle of repose (~20 deg). Same Jacobi
    //    scheme as thermal weathering — mass-conserving.
    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    const float sqrt2 = 1.41421356f;
    const float talus = std::tan(20.0f * pi / 180.0f) * m_Params.cellSize;

    std::vector<float> delta(count);
    for (int pass = 0; pass < p.settlePasses; ++pass) {
        std::fill(delta.begin(), delta.end(), 0.0f);
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int i = Index(x, y);
                const float snowHere = m_Snow[static_cast<size_t>(i)];
                if (snowHere <= 0.0f) continue;
                const float h = m_Bedrock[i] + m_Sediment[i] + snowHere;

                float excess[8];
                int nIdx[8];
                int nCount = 0;
                float totalExcess = 0.0f;
                float maxExcess = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + dx8[k];
                    const int ny = y + dy8[k];
                    if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
                    const int ni = Index(nx, ny);
                    const float t = (k < 4) ? talus : talus * sqrt2;
                    const float nh = m_Bedrock[ni] + m_Sediment[ni] + m_Snow[static_cast<size_t>(ni)];
                    const float diff = h - nh;
                    if (diff > t) {
                        excess[nCount] = diff - t;
                        nIdx[nCount] = ni;
                        ++nCount;
                        totalExcess += diff - t;
                        maxExcess = std::max(maxExcess, diff - t);
                    }
                }
                if (nCount == 0) continue;
                const float toMove = std::min(snowHere, 0.25f * maxExcess);
                if (toMove <= 0.0f) continue;
                delta[static_cast<size_t>(i)] -= toMove;
                for (int k = 0; k < nCount; ++k) {
                    delta[static_cast<size_t>(nIdx[k])] += toMove * (excess[k] / totalExcess);
                }
            }
        }
        for (size_t i = 0; i < count; ++i) {
            m_Snow[i] = std::max(0.0f, m_Snow[i] + delta[i]);
        }
    }

    // 3. Melt: settled snow that ended up well below the snow line thins.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const int i = Index(x, y);
            const float alt = (m_Bedrock[i] + m_Sediment[i]) / heightRef;
            if (alt < p.snowLine) {
                const float below = (p.snowLine - alt) / std::max(0.05f, p.snowLine);
                const float keep = std::max(0.0f, 1.0f - below * (1.0f + 4.0f * p.melt));
                m_Snow[static_cast<size_t>(i)] *= keep;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Water (lakes) — priority-flood fill depth
// ---------------------------------------------------------------------------

void TerrainEngine::ComputeWater() {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;
    if (m_Water.size() != count) m_Water.assign(count, 0.0f);

    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};

    std::vector<float> height(count);
    for (size_t i = 0; i < count; ++i) height[i] = m_Bedrock[i] + m_Sediment[i];

    // Index tie-break keeps the pop order a total order — see the longer note
    // in Fluvial.cpp. Lakes are flat by construction so equal keys are the
    // rule here, but as there the fill value depends only on the popped
    // height, so this is hardening rather than a fix (mutation test M4).
    struct Cell {
        float h;
        int idx;
        bool operator>(const Cell& o) const {
            return h != o.h ? h > o.h : idx > o.idx;
        }
    };
    std::priority_queue<Cell, std::vector<Cell>, std::greater<Cell>> pq;
    std::vector<uint8_t> visited(count, 0);

    for (int x = 0; x < size; ++x) {
        const int top = x, bottom = (size - 1) * size + x;
        if (!visited[top]) { visited[top] = 1; pq.push({height[top], top}); }
        if (!visited[bottom]) { visited[bottom] = 1; pq.push({height[bottom], bottom}); }
    }
    for (int y = 0; y < size; ++y) {
        const int left = y * size, right = y * size + size - 1;
        if (!visited[left]) { visited[left] = 1; pq.push({height[left], left}); }
        if (!visited[right]) { visited[right] = 1; pq.push({height[right], right}); }
    }

    std::vector<float> filled = height;
    while (!pq.empty()) {
        const Cell c = pq.top();
        pq.pop();
        const int cx = c.idx % size;
        const int cy = c.idx / size;
        for (int k = 0; k < 8; ++k) {
            const int nx = cx + dx8[k];
            const int ny = cy + dy8[k];
            if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
            const int ni = ny * size + nx;
            if (visited[ni]) continue;
            visited[ni] = 1;
            filled[ni] = std::max(height[ni], c.h);
            pq.push({filled[ni], ni});
        }
    }

    for (size_t i = 0; i < count; ++i) {
        m_Water[i] = std::max(0.0f, filled[i] - height[i]);
    }
}

} // namespace Titan
