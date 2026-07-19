#include "TitanCore.h"
#include "TitanNoise.h"

#include <algorithm>
#include <cmath>

#if !defined(__EMSCRIPTEN__)
#include <future>
#include <thread>
#endif

namespace Titan {

void TerrainEngine::Initialize(const TerrainParams& params) {
    m_Params = params;
    const size_t count = static_cast<size_t>(params.size) * params.size;
    m_Bedrock.assign(count, 0.0f);
    m_Sediment.assign(count, 0.0f);
    m_Flow.assign(count, 0.0f);
    m_DropletCursor = 0;
    if (m_Precipitation.size() != count) m_Precipitation.clear();
}

void TerrainEngine::GenerateHeightmap() {
    const int size = m_Params.size;
    m_DropletCursor = 0;

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

float TerrainEngine::HardnessAt(float height) const {
    // Layered geological strata: overlapping sine bands give irregular
    // hard/soft rock layers by altitude. Range [0.4, 1.6].
    const float layerScale = 0.5f;
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
// Shaping modifiers. Both scale bedrock and sediment proportionally so the
// two-layer ratio is preserved.
// ---------------------------------------------------------------------------

void TerrainEngine::ApplyTerrace(float interval, float strength, float sharpness) {
    if (interval <= 0.0f || strength <= 0.0f) return;
    strength = std::clamp(strength, 0.0f, 1.0f);
    sharpness = std::max(1.0f, sharpness);
    const size_t count = m_Bedrock.size();

    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        if (h <= 0.0f) continue;

        const float t = h / interval;
        const float level = std::floor(t);
        float frac = t - level;
        // Sharpened smoothstep: pushes the transition toward the step edge.
        frac = std::pow(frac, sharpness);
        frac = frac * frac * (3.0f - 2.0f * frac);
        const float terraced = (level + frac) * interval;

        const float target = h + (terraced - h) * strength;
        const float ratio = target / h;
        m_Bedrock[i] *= ratio;
        m_Sediment[i] *= ratio;
    }
}

void TerrainEngine::ApplyPlateau(float plateauHeight, float softness) {
    if (plateauHeight <= 0.0f) return;
    softness = std::max(0.01f, softness);
    const size_t count = m_Bedrock.size();
    const float shoulder = plateauHeight - softness;

    for (size_t i = 0; i < count; ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        if (h <= shoulder || h <= 0.0f) continue;

        // Rounded shoulder: heights above (plateau - softness) compress
        // asymptotically toward the plateau height.
        const float over = h - shoulder;
        const float target = shoulder + softness * std::tanh(over / softness);
        const float ratio = target / h;
        m_Bedrock[i] *= ratio;
        m_Sediment[i] *= ratio;
    }
}

void TerrainEngine::Carve(float x, float y, float radius, float depth) {
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
            m_Bedrock[idx] = std::max(0.0f, m_Bedrock[idx] - amount);
            m_Sediment[idx] = std::max(0.0f, m_Sediment[idx] - amount * 0.5f);
            m_Flow[idx] += weight * 0.2f;
        }
    }
}

void TerrainEngine::BuildMesh() {
    const int size = m_Params.size;
    const size_t vertexCount = static_cast<size_t>(size) * size;
    const size_t indexCount = static_cast<size_t>(size - 1) * (size - 1) * 6;

    m_MeshPositions.resize(vertexCount * 3);
    m_MeshNormals.resize(vertexCount * 3);
    m_MeshColors.resize(vertexCount * 4);
    m_MeshUVs.resize(vertexCount * 2);
    m_MeshIndices.resize(indexCount);

    const float c2 = 2.0f * m_Params.cellSize;

    // Rock mask by slope *angle*: bare rock above ~40 deg, none below ~20 deg.
    const float rockLo = 0.36f; // tan(20 deg)
    const float rockHi = 0.84f; // tan(40 deg)

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const size_t i = static_cast<size_t>(Index(x, y));
            const float b = m_Bedrock[i];
            const float s = m_Sediment[i];
            const float h = b + s;

            m_MeshPositions[i * 3 + 0] = (static_cast<float>(x) - size * 0.5f) * m_Params.cellSize;
            m_MeshPositions[i * 3 + 1] = h;
            m_MeshPositions[i * 3 + 2] = (static_cast<float>(y) - size * 0.5f) * m_Params.cellSize;

            const float dhdx = (GetHeight(x + 1, y) - GetHeight(x - 1, y)) / c2;
            const float dhdy = (GetHeight(x, y + 1) - GetHeight(x, y - 1)) / c2;
            float nx = -dhdx, ny = 1.0f, nz = -dhdy;
            const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            m_MeshNormals[i * 3 + 0] = nx * invLen;
            m_MeshNormals[i * 3 + 1] = ny * invLen;
            m_MeshNormals[i * 3 + 2] = nz * invLen;

            m_MeshUVs[i * 2 + 0] = static_cast<float>(x) / (size - 1);
            m_MeshUVs[i * 2 + 1] = static_cast<float>(y) / (size - 1);

            // Splat data — R: rock (slope angle), G: normalized height,
            // B: flow/wetness, A: sediment depth.
            const float slope = std::sqrt(dhdx * dhdx + dhdy * dhdy);
            m_MeshColors[i * 4 + 0] = std::clamp((slope - rockLo) / (rockHi - rockLo), 0.0f, 1.0f);
            m_MeshColors[i * 4 + 1] = m_Params.heightMultiplier > 0.0f
                ? std::clamp(h / m_Params.heightMultiplier, 0.0f, 1.0f) : 0.0f;
            m_MeshColors[i * 4 + 2] = std::clamp(m_Flow[i] * 0.5f, 0.0f, 1.0f);
            m_MeshColors[i * 4 + 3] = std::clamp(s / 3.0f, 0.0f, 1.0f);
        }
    }

    size_t idx = 0;
    for (int y = 0; y < size - 1; ++y) {
        for (int x = 0; x < size - 1; ++x) {
            const uint32_t i0 = static_cast<uint32_t>(y * size + x);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(size);
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
