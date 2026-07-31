#include "TitanCore.h"
#include "TitanNoise.h"

#include <algorithm>
#include <cmath>

#if !defined(__EMSCRIPTEN__)
#include <future>
#include <thread>
#endif

namespace Titan {

// Bounds every generation parameter before anything is allocated or looped on.
//
// This is the outermost trust boundary in the library: `size` drives a
// size*size allocation and `octaves` an inner loop per cell, and both arrive
// straight from a C caller — a WASM host reading a shared .titan file, a
// Blueprint property, a scripted pipeline. Unvalidated, size = 100000 asked
// for ~40 GB and size = -1 turned into a huge size_t that threw
// std::length_error *through* an extern "C" boundary, which is undefined
// behaviour and, inside the Unreal editor, a hard crash.
//
// Clamping rather than rejecting keeps the C API total: every call produces a
// usable engine, and hosts that want to surface "your value was adjusted" can
// compare Params() afterwards.
TerrainParams TerrainEngine::Sanitize(const TerrainParams& in) {
    auto finite = [](float v, float fallback) {
        return std::isfinite(v) ? v : fallback;
    };

    TerrainParams p = in;
    // 8192 is the practical ceiling: 8192^2 float maps are ~268 MB each and
    // the engine holds seven of them.
    p.size = std::clamp(in.size, 2, 8192);
    p.cellSize = std::clamp(finite(in.cellSize, 1.0f), 1e-4f, 1e6f);
    p.scale = std::clamp(finite(in.scale, 2.0f), 1e-3f, 1e4f);
    p.heightMultiplier = std::clamp(finite(in.heightMultiplier, 40.0f), 0.0f, 1e6f);
    p.octaves = std::clamp(in.octaves, 1, 16);
    p.persistence = std::clamp(finite(in.persistence, 0.5f), 0.0f, 1.0f);
    p.lacunarity = std::clamp(finite(in.lacunarity, 2.0f), 1.0f, 8.0f);
    p.exponent = std::clamp(finite(in.exponent, 1.0f), 0.01f, 16.0f);
    p.noiseType = std::clamp(in.noiseType, 0, static_cast<int>(NoiseType::HybridMulti));
    p.warpStrength = std::clamp(finite(in.warpStrength, 0.0f), 0.0f, 100.0f);
    p.ridgeOffset = std::clamp(finite(in.ridgeOffset, 1.0f), 0.01f, 10.0f);
    p.ridgeGain = std::clamp(finite(in.ridgeGain, 2.0f), 0.0f, 10.0f);
    p.originX = finite(in.originX, 0.0f);
    p.originY = finite(in.originY, 0.0f);
    return p;
}

void TerrainEngine::Initialize(const TerrainParams& params) {
    m_Params = Sanitize(params);
    const size_t count = static_cast<size_t>(m_Params.size) * m_Params.size;
    m_Bedrock.assign(count, 0.0f);
    m_Sediment.assign(count, 0.0f);
    m_Flow.assign(count, 0.0f);
    m_Snow.assign(count, 0.0f);
    m_Water.assign(count, 0.0f);
    m_Scratch.assign(count, 0.0f);
    m_DropletCursor = 0;
    m_MassExported = 0.0;
    m_MassCreated = 0.0;
    m_Mask.clear();
    // Lava fields stay unallocated until something erupts — see EnsureLavaFields.
    ClearLava();
    if (m_Precipitation.size() != count) m_Precipitation.clear();
}

void TerrainEngine::GenerateHeightmap() {
    const int size = m_Params.size;
    m_DropletCursor = 0;
    m_MassExported = 0.0;
    m_MassCreated = 0.0;

    std::fill(m_Snow.begin(), m_Snow.end(), 0.0f);
    std::fill(m_Water.begin(), m_Water.end(), 0.0f);
    ClearLava();

    if (m_Params.noiseType == static_cast<int>(NoiseType::None)) {
        std::fill(m_Bedrock.begin(), m_Bedrock.end(), 0.0f);
        std::fill(m_Sediment.begin(), m_Sediment.end(), 0.0f);
        std::fill(m_Flow.begin(), m_Flow.end(), 0.0f);
        return;
    }

    FractalParams fp;
    fp.type = static_cast<NoiseType>(m_Params.noiseType);
    fp.octaves = m_Params.octaves;
    fp.persistence = m_Params.persistence;
    fp.lacunarity = m_Params.lacunarity;
    fp.ridgeOffset = m_Params.ridgeOffset;
    fp.ridgeGain = m_Params.ridgeGain;
    fp.warpStrength = m_Params.warpStrength;

    const FractalNoise noise(m_Params.seed, fp);

    // Noise is sampled in world space, then scaled so `scale` noise features
    // span the tile regardless of resolution. Tiles that share an origin grid
    // line up seamlessly because neighbours sample the same world coordinates.
    const float extent = static_cast<float>(size) * m_Params.cellSize;
    const float frequency = extent > 0.0f ? m_Params.scale / extent : 0.0f;

    auto generateRows = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; ++y) {
            for (int x = 0; x < size; ++x) {
                const float worldX = m_Params.originX + (static_cast<float>(x) - size * 0.5f) * m_Params.cellSize;
                const float worldY = m_Params.originY + (static_cast<float>(y) - size * 0.5f) * m_Params.cellSize;

                float h = noise.Sample(worldX * frequency, worldY * frequency); // [0, 1]
                h = std::pow(std::clamp(h, 0.0f, 1.0f), m_Params.exponent);

                const float total = h * m_Params.heightMultiplier;
                m_Bedrock[Index(x, y)] = total * 0.8f;
                m_Sediment[Index(x, y)] = total * 0.2f;
            }
        }
    };

#if defined(__EMSCRIPTEN__)
    generateRows(0, size);
#else
    // Rows are independent — deterministic regardless of the split.
    const int bands = std::min(8, size);
    std::vector<std::future<void>> jobs;
    jobs.reserve(bands);
    for (int b = 0; b < bands; ++b) {
        const int yBegin = size * b / bands;
        const int yEnd = size * (b + 1) / bands;
        jobs.push_back(std::async(std::launch::async, generateRows, yBegin, yEnd));
    }
    for (auto& j : jobs) j.get();
#endif

    std::fill(m_Flow.begin(), m_Flow.end(), 0.0f);
}

float TerrainEngine::GetHeight(int x, int y) const {
    // Clamp instead of returning 0 so edges don't read as cliffs.
    x = std::clamp(x, 0, m_Params.size - 1);
    y = std::clamp(y, 0, m_Params.size - 1);
    return m_Bedrock[Index(x, y)] + m_Sediment[Index(x, y)];
}

float TerrainEngine::GetBedrock(int x, int y) const {
    if (!InBounds(x, y)) return 0.0f;
    return m_Bedrock[Index(x, y)];
}

float TerrainEngine::GetSediment(int x, int y) const {
    if (!InBounds(x, y)) return 0.0f;
    return m_Sediment[Index(x, y)];
}

float TerrainEngine::GetFlow(int x, int y) const {
    if (!InBounds(x, y)) return 0.0f;
    return m_Flow[Index(x, y)];
}

float TerrainEngine::GetSlope(int x, int y) const {
    const float c2 = 2.0f * m_Params.cellSize;
    const float dx = (GetHeight(x + 1, y) - GetHeight(x - 1, y)) / c2;
    const float dy = (GetHeight(x, y + 1) - GetHeight(x, y - 1)) / c2;
    return std::sqrt(dx * dx + dy * dy);
}

void TerrainEngine::SplatAt(int x, int y, float& rock, float& height,
                            float& flow, float& sediment) const {
    // Rock mask by slope *angle*: bare rock above ~40 deg, none below ~20 deg.
    const float rockLo = 0.36f; // tan(20 deg)
    const float rockHi = 0.84f; // tan(40 deg)

    const int cx = std::clamp(x, 0, m_Params.size - 1);
    const int cy = std::clamp(y, 0, m_Params.size - 1);
    const size_t i = static_cast<size_t>(cy) * m_Params.size + cx;

    const float snow = m_Snow.size() == m_Bedrock.size() ? m_Snow[i] : 0.0f;
    const float h = m_Bedrock[i] + m_Sediment[i] + snow;

    // Slope measured on the same surface the mesh is built from (ground plus
    // snowpack), via the shared height accessor.
    const float c2 = 2.0f * m_Params.cellSize;
    auto total = [&](int px, int py) {
        const int qx = std::clamp(px, 0, m_Params.size - 1);
        const int qy = std::clamp(py, 0, m_Params.size - 1);
        const size_t j = static_cast<size_t>(qy) * m_Params.size + qx;
        const float sn = m_Snow.size() == m_Bedrock.size() ? m_Snow[j] : 0.0f;
        return m_Bedrock[j] + m_Sediment[j] + sn;
    };
    const float dhdx = (total(cx + 1, cy) - total(cx - 1, cy)) / c2;
    const float dhdy = (total(cx, cy + 1) - total(cx, cy - 1)) / c2;
    const float slope = std::sqrt(dhdx * dhdx + dhdy * dhdy);

    rock = std::clamp((slope - rockLo) / (rockHi - rockLo), 0.0f, 1.0f);

    // Normalized height, measured against the surface's *actual* span rather
    // than the Height slider.
    //
    // The slider only bounds the base noise. Anything that adds relief on top
    // of it — a gradient, a stamp, a volcano, fluvial deposition — pushes the
    // terrain past it, and dividing by it then clamps every cell above the
    // slider to exactly 1.0. A volcano rendered as one flat saturated wash
    // with no relief anywhere on its upper cone, and the exported splatmap's
    // green channel was equally useless. RefreshSplatRange keeps this in step
    // with the same span the normalizing exporters use.
    const float span = m_SplatHi - m_SplatLo;
    height = (m_SplatRangeValid && span > 1e-6f)
        ? std::clamp((h - m_SplatLo) / span, 0.0f, 1.0f)
        : (m_Params.heightMultiplier > 0.0f
              ? std::clamp(h / m_Params.heightMultiplier, 0.0f, 1.0f) : 0.0f);

    flow = std::clamp(m_Flow[i] * 0.5f, 0.0f, 1.0f);
    sediment = std::clamp(m_Sediment[i] / 3.0f, 0.0f, 1.0f);
}

void TerrainEngine::RefreshSplatRange() {
    // Measured on the shaded surface (ground plus snowpack), which is what
    // SplatAt reads, not the bare ground CollectHeightRange reports.
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;
    const bool hasSnow = m_Snow.size() == count;

    m_SplatLo = 1e30f;
    m_SplatHi = -1e30f;
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i] + (hasSnow ? m_Snow[i] : 0.0f);
        m_SplatLo = std::min(m_SplatLo, h);
        m_SplatHi = std::max(m_SplatHi, h);
    }
    if (m_SplatLo > m_SplatHi) { m_SplatLo = 0.0f; m_SplatHi = 0.0f; }
    m_SplatRangeValid = true;
}

float TerrainEngine::HardnessAt(float height) const {
    // Layered geological strata: overlapping sine bands give irregular
    // hard/soft rock layers by altitude. Range [0.4, 1.6].
    //
    // The band spacing is relative to the terrain's own height scale, so the
    // *number* of strata across the relief is constant. A fixed absolute
    // spacing put a band every ~12 world units, which is geology at
    // heightMultiplier 40 and visual noise at a real-world 5000 m range.
    // 20/heightRef reproduces the original 0.5 at the default of 40.
    const float heightRef = std::max(1.0f, m_Params.heightMultiplier);
    const float layerScale = 20.0f / heightRef;
    const float strata = (std::sin(height * layerScale) + std::sin(height * layerScale * 2.1f) * 0.5f) / 1.5f;
    return 1.0f + strata * 0.6f;
}

float TerrainEngine::SampleHeight(float x, float y) const {
    const int nx = static_cast<int>(std::floor(x));
    const int ny = static_cast<int>(std::floor(y));
    const float u = x - nx;
    const float v = y - ny;
    return GetHeight(nx, ny) * (1 - u) * (1 - v) +
           GetHeight(nx + 1, ny) * u * (1 - v) +
           GetHeight(nx, ny + 1) * (1 - u) * v +
           GetHeight(nx + 1, ny + 1) * u * v;
}

void TerrainEngine::SetPrecipitationMap(const float* data, int size) {
    if (!data || size != m_Params.size) {
        m_Precipitation.clear();
        return;
    }
    m_Precipitation.assign(data, data + static_cast<size_t>(size) * size);
}

void TerrainEngine::ClearPrecipitationMap() {
    m_Precipitation.clear();
}

float TerrainEngine::SamplePrecipitation(float x, float y) const {
    if (m_Precipitation.empty()) return 1.0f;
    const int size = m_Params.size;
    const int nx = static_cast<int>(std::floor(x));
    const int ny = static_cast<int>(std::floor(y));
    const float u = x - nx;
    const float v = y - ny;

    auto at = [&](int px, int py) -> float {
        px = std::clamp(px, 0, size - 1);
        py = std::clamp(py, 0, size - 1);
        return m_Precipitation[py * size + px];
    };
    return at(nx, ny) * (1 - u) * (1 - v) +
           at(nx + 1, ny) * u * (1 - v) +
           at(nx, ny + 1) * (1 - u) * v +
           at(nx + 1, ny + 1) * u * v;
}

void TerrainEngine::GradientAt(float x, float y, float& gx, float& gy) const {
    const int nx = static_cast<int>(std::floor(x));
    const int ny = static_cast<int>(std::floor(y));
    const float u = x - nx;
    const float v = y - ny;

    const float h00 = GetHeight(nx, ny);
    const float h10 = GetHeight(nx + 1, ny);
    const float h01 = GetHeight(nx, ny + 1);
    const float h11 = GetHeight(nx + 1, ny + 1);

    gx = (h10 - h00) * (1 - v) + (h11 - h01) * v;
    gy = (h01 - h00) * (1 - u) + (h11 - h10) * u;
}

// ---------------------------------------------------------------------------
// Shaping modifiers. Both write through ResplitHeight, which preserves the
// bedrock/sediment ratio and handles below-datum cells.
// ---------------------------------------------------------------------------

void TerrainEngine::ApplyTerrace(float interval, float strength, float sharpness) {
    if (interval <= 0.0f || strength <= 0.0f) return;
    strength = std::clamp(strength, 0.0f, 1.0f);
    sharpness = std::max(1.0f, sharpness);
    const size_t count = m_Bedrock.size();

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];

        // std::floor rather than truncation, so terracing behaves the same
        // above and below the datum.
        const float t = h / interval;
        const float level = std::floor(t);
        float frac = t - level;
        // Sharpened smoothstep: pushes the transition toward the step edge.
        frac = std::pow(frac, sharpness);
        frac = frac * frac * (3.0f - 2.0f * frac);
        const float terraced = (level + frac) * interval;

        total[i] = h + (terraced - h) * strength * MaskAt(static_cast<int>(i));
    }
    // Routing through ResplitHeight keeps every height edit on one path, so
    // the bedrock/sediment ratio is preserved consistently and below-datum
    // cells are handled in one place rather than each op inventing its own
    // rule. (This used to scale the two layers by target/h directly, which
    // inverts the terrain for negative h.)
    ResplitHeight(total);
}

void TerrainEngine::ApplyPlateau(float plateauHeight, float softness) {
    if (plateauHeight <= 0.0f) return;
    softness = std::max(0.01f, softness);
    const size_t count = m_Bedrock.size();
    const float shoulder = plateauHeight - softness;

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        if (h <= shoulder) {
            total[i] = h;
            continue;
        }
        // Rounded shoulder: heights above (plateau - softness) compress
        // asymptotically toward the plateau height.
        const float over = h - shoulder;
        const float capped = shoulder + softness * std::tanh(over / softness);
        total[i] = h + (capped - h) * MaskAt(static_cast<int>(i));
    }
    ResplitHeight(total);
}

void TerrainEngine::Carve(float x, float y, float radius, float depth) {
    // Brush radius is a world distance, converted to cells. cellSize 1.0 is
    // exactly identity.
    radius /= (m_Params.cellSize > 0.0f ? m_Params.cellSize : 1.0f);
    const int centerX = static_cast<int>(std::floor(x));
    const int centerY = static_cast<int>(std::floor(y));
    const int r = static_cast<int>(std::ceil(radius));

    for (int j = -r; j <= r; ++j) {
        for (int i = -r; i <= r; ++i) {
            const int px = centerX + i;
            const int py = centerY + j;
            if (!InBounds(px, py)) continue;
            const float dx = static_cast<float>(px) - x;
            const float dy = static_cast<float>(py) - y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= radius) continue;

            const float weight = 1.0f - dist / radius;
            const float amount = depth * weight;
            const int idx = Index(px, py);
            // Bedrock may be carved below the datum; sediment is a deposit
            // and stops at zero.
            m_Bedrock[idx] -= amount;
            m_Sediment[idx] = std::max(0.0f, m_Sediment[idx] - amount * 0.5f);
            m_Flow[idx] += weight * 0.2f;
        }
    }
}

// Builds the preview mesh, optionally decimated.
//
// `stride` samples every Nth cell in each axis, so simulation resolution and
// preview resolution are independent. That separation is what lets the grid go
// past 512: a 2048 terrain is a perfectly reasonable simulation but a
// full-resolution mesh of it is 4.2M vertices — roughly 300 MB of typed arrays
// once a JS host copies positions, normals, colours, UVs and indices out of
// the WASM heap, and 4096 would exceed what a 32-bit heap can hold at all.
// Exports still run at full resolution; only what you orbit is decimated.
void TerrainEngine::BuildMesh(int stride) {
    const int size = m_Params.size;
    stride = std::max(1, stride);
    // Vertices along one edge after decimation. Always includes the last row
    // and column so the mesh spans the full terrain extent.
    const int outSize = (size - 1) / stride + 1;
    if (outSize < 2) {
        m_MeshPositions.clear();
        m_MeshNormals.clear();
        m_MeshColors.clear();
        m_MeshUVs.clear();
        m_MeshSnow.clear();
        m_MeshLava.clear();
        m_MeshSurface.clear();
        m_MeshIndices.clear();
        m_MeshStride = stride;
        return;
    }

    const size_t vertexCount = static_cast<size_t>(outSize) * outSize;
    const size_t indexCount = static_cast<size_t>(outSize - 1) * (outSize - 1) * 6;

    m_MeshPositions.resize(vertexCount * 3);
    m_MeshNormals.resize(vertexCount * 3);
    m_MeshColors.resize(vertexCount * 4);
    m_MeshUVs.resize(vertexCount * 2);
    m_MeshSnow.resize(vertexCount);
    m_MeshLava.resize(vertexCount * 4);
    m_MeshSurface.resize(vertexCount * 4);
    m_MeshIndices.resize(indexCount);
    m_MeshStride = stride;

    const size_t cellCount = static_cast<size_t>(size) * size;
    const bool hasSnow = m_Snow.size() == cellCount;
    const bool hasWater = m_Water.size() == cellCount;
    // AO is only present if a host asked for it — it is the one derived map
    // too expensive to recompute per chunk. Absent, the surface shades as
    // fully open, which is exactly the old behaviour.
    const bool hasAO = m_AO.size() == cellCount;

    // Height range for the splat channel, measured once per build.
    RefreshSplatRange();
    // Molten lava sits on the surface like the snowpack: it is displaced into
    // the mesh so a flow reads as a raised, lobed ribbon rather than a decal,
    // but it is not terrain and never reaches the exporters. The rock it
    // leaves behind went into the bedrock the moment it chilled.
    const bool hasLava = m_Lava.size() == cellCount;

    // Total rendered surface: ground, snowpack, molten lava, and lake water.
    //
    // Water is part of the surface rather than a shader tint because the
    // priority-flood fill produces a *level* pond — displacing to it is what
    // makes a lake read as a flat sheet sitting in a basin instead of a blue
    // stain painted down the contours of the lakebed.
    auto totalAt = [&](int x, int y) -> float {
        const float ground = GetHeight(x, y);
        const int cx = std::clamp(x, 0, size - 1);
        const int cy = std::clamp(y, 0, size - 1);
        const size_t j = static_cast<size_t>(cy) * size + cx;
        float h = ground;
        if (hasSnow) h += m_Snow[j];
        if (hasLava) h += m_Lava[j];
        if (hasWater) h += m_Water[j];
        return h;
    };

    // Normals are differenced across the *stride*, so a decimated mesh is
    // shaded from the surface it actually renders rather than from
    // high-frequency detail it does not have.
    const float c2 = 2.0f * m_Params.cellSize * static_cast<float>(stride);

    for (int oy = 0; oy < outSize; ++oy) {
        for (int ox = 0; ox < outSize; ++ox) {
            const int x = std::min(ox * stride, size - 1);
            const int y = std::min(oy * stride, size - 1);
            const size_t i = static_cast<size_t>(oy) * outSize + ox;
            const size_t src = static_cast<size_t>(y) * size + x;

            const float snow = hasSnow ? m_Snow[src] : 0.0f;
            const float molten = hasLava ? m_Lava[src] : 0.0f;
            const float pond = hasWater ? m_Water[src] : 0.0f;
            const float h = m_Bedrock[src] + m_Sediment[src] + snow + molten + pond;

            m_MeshPositions[i * 3 + 0] = (static_cast<float>(x) - size * 0.5f) * m_Params.cellSize;
            m_MeshPositions[i * 3 + 1] = h;
            m_MeshPositions[i * 3 + 2] = (static_cast<float>(y) - size * 0.5f) * m_Params.cellSize;

            m_MeshSnow[i] = snow;

            // vec4(molten depth, heat, chilled-rock depth, glow) — everything a
            // shader needs to render a flow without reading back a texture.
            m_MeshLava[i * 4 + 0] = molten;
            m_MeshLava[i * 4 + 1] = hasLava ? m_LavaHeat[src] : 0.0f;
            m_MeshLava[i * 4 + 2] = hasLava ? m_LavaRock[src] : 0.0f;
            m_MeshLava[i * 4 + 3] = hasLava ? m_LavaGlow[src] : 0.0f;

            // Surface attribute: ao, curvature, snow depth, water depth.
            //
            // Curvature is the Laplacian of the rendered surface, differenced
            // across the stride like the normal so a decimated preview reads
            // the shape it actually has. Squashed to 0..1 with 0.5 = flat,
            // which is the same convention MaskByFeature uses.
            const float lap = totalAt(x + stride, y) + totalAt(x - stride, y)
                            + totalAt(x, y + stride) + totalAt(x, y - stride)
                            - 4.0f * totalAt(x, y);
            const float curv = 0.5f + 0.5f * std::tanh(lap / (c2 * 0.5f + 1e-6f));

            m_MeshSurface[i * 4 + 0] = hasAO ? m_AO[src] : 1.0f;
            m_MeshSurface[i * 4 + 1] = std::clamp(curv, 0.0f, 1.0f);
            m_MeshSurface[i * 4 + 2] = snow;
            m_MeshSurface[i * 4 + 3] = hasWater ? m_Water[src] : 0.0f;

            const float dhdx = (totalAt(x + stride, y) - totalAt(x - stride, y)) / c2;
            const float dhdy = (totalAt(x, y + stride) - totalAt(x, y - stride)) / c2;
            float nx = -dhdx, ny = 1.0f, nz = -dhdy;
            const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            m_MeshNormals[i * 3 + 0] = nx * invLen;
            m_MeshNormals[i * 3 + 1] = ny * invLen;
            m_MeshNormals[i * 3 + 2] = nz * invLen;

            m_MeshUVs[i * 2 + 0] = static_cast<float>(x) / static_cast<float>(size - 1);
            m_MeshUVs[i * 2 + 1] = static_cast<float>(y) / static_cast<float>(size - 1);

            // Splat data — shared with the splatmap exporter.
            SplatAt(x, y, m_MeshColors[i * 4 + 0], m_MeshColors[i * 4 + 1],
                    m_MeshColors[i * 4 + 2], m_MeshColors[i * 4 + 3]);
        }
    }

    size_t idx = 0;
    for (int y = 0; y < outSize - 1; ++y) {
        for (int x = 0; x < outSize - 1; ++x) {
            const uint32_t i0 = static_cast<uint32_t>(y * outSize + x);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(outSize);
            const uint32_t i3 = i2 + 1;
            m_MeshIndices[idx++] = i0;
            m_MeshIndices[idx++] = i2;
            m_MeshIndices[idx++] = i1;
            m_MeshIndices[idx++] = i1;
            m_MeshIndices[idx++] = i2;
            m_MeshIndices[idx++] = i3;
        }
    }
}

void TerrainEngine::CollectHeightRange(float& minH, float& maxH) const {
    minH = 1e30f;
    maxH = -1e30f;
    const size_t count = m_Bedrock.size();
    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        minH = std::min(minH, h);
        maxH = std::max(maxH, h);
    }
    if (minH > maxH) { minH = 0.0f; maxH = 0.0f; }
}

} // namespace Titan
