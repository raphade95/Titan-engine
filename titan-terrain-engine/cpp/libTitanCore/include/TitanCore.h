#pragma once

#include "TitanNoise.h"

#include <cstdint>
#include <vector>

namespace Titan {

struct TerrainParams {
    int size = 256;               // grid resolution (size x size)
    float cellSize = 1.0f;        // world units per cell
    float scale = 2.0f;           // noise features across the terrain extent
    float heightMultiplier = 50.0f;
    uint32_t seed = 42;
    int octaves = 6;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    float exponent = 1.0f;        // shaping curve applied to normalized height
    int noiseType = static_cast<int>(NoiseType::Standard);

    float warpStrength = 0.0f;    // domain warp, noise-space units
    float ridgeOffset = 1.0f;
    float ridgeGain = 2.0f;

    // World-space origin of this tile. Noise is sampled in global
    // coordinates so adjacent tiles line up seamlessly.
    float originX = 0.0f;
    float originY = 0.0f;
};

// How hydraulic droplets pick their spawn point.
enum class SpawnMode : int {
    Uniform = 0,       // anywhere, evenly
    Altitude = 1,      // rain concentrates on high ground
    Precipitation = 2  // user-supplied precipitation map (falls back to Uniform if unset)
};

// How a noise or stamp layer combines with the existing terrain.
enum class BlendMode : int {
    Add = 0,
    Subtract = 1,
    Multiply = 2,
    Max = 3,
    Min = 4,
    Mix = 5 // lerp toward the field by blendAlpha
};

enum class StampShape : int {
    Dome = 0,     // circular hill (cosine profile)
    Rectangle = 1,
    Ridge = 2,    // elongated ridge line
    Crater = 3    // rim + bowl
};

enum class StampOp : int {
    Raise = 0,
    Lower = 1,
    Flatten = 2,  // pull terrain toward the stamp height
    Union = 3     // terrain = max(terrain, stamp field)
};

struct NoiseLayerParams {
    int noiseType = static_cast<int>(NoiseType::Standard);
    uint32_t seedOffset = 0;      // combined with the terrain seed
    float scale = 2.0f;           // features across the terrain extent
    float amplitude = 40.0f;      // height contribution in world units
    int octaves = 6;
    float persistence = 0.5f;
    float lacunarity = 2.0f;
    float exponent = 1.0f;
    float warpStrength = 0.0f;
    float ridgeOffset = 1.0f;
    float ridgeGain = 2.0f;
    int blendMode = static_cast<int>(BlendMode::Add);
    float blendAlpha = 0.5f;      // used by Mix
};

struct StampParams {
    int shape = static_cast<int>(StampShape::Dome);
    float centerX = 0.0f;         // cell coordinates
    float centerY = 0.0f;
    float sizeX = 32.0f;          // radius / half-extent in cells
    float sizeY = 32.0f;
    float rotationDeg = 0.0f;
    float height = 20.0f;         // world units
    float falloff = 0.5f;         // 0 = hard edge, 1 = full-width falloff
    int op = static_cast<int>(StampOp::Raise);
};

struct SnowParams {
    float snowLine = 0.55f;       // fraction of heightMultiplier where snow starts
    float amount = 6.0f;          // max snow depth in world units
    float maxSlopeDeg = 42.0f;    // steeper faces shed their snow
    int settlePasses = 8;         // creep relaxation iterations
    float melt = 0.35f;           // how aggressively snow thins near the line
};

struct HydraulicParams {
    float inertia = 0.1f;
    float sedimentCapacityFactor = 4.0f;
    float minSedimentCapacity = 0.01f;
    float dissolveSpeed = 0.1f;
    float depositSpeed = 0.1f;
    float evaporateSpeed = 0.01f;
    float gravity = 4.0f;
    int maxDropletLifetime = 60;
    float erosionRadius = 3.0f;
    float bedrockErosionSpeed = 0.05f; // scaled by local strata hardness
    int spawnMode = static_cast<int>(SpawnMode::Uniform);
};

struct ThermalParams {
    float talusAngleDeg = 33.0f;  // angle of repose
    float rate = 0.5f;            // fraction of excess moved per pass
    float bedrockBreakdownRate = 0.05f;
};

struct FluvialParams {
    float strength = 1.0f;        // master multiplier on stream-power erosion
    float erodeConstant = 0.015f; // K in E = K * A^m * S^n
    float areaExponent = 0.5f;    // m
    float slopeExponent = 1.0f;   // n
    float depositRatio = 0.3f;    // fraction of eroded material re-deposited downstream
    float maxStep = 2.0f;         // absolute per-iteration erosion cap
};

// Hydraulic erosion batching contract (determinism + parallelism):
// droplets are processed in fixed batches of kDropletBatch, grouped into
// rounds of kBatchesPerRound. Batches within a round run against the same
// terrain snapshot (possibly on multiple threads) and their deltas merge in
// batch order, so results are bit-identical regardless of thread count —
// and identical whether iterations arrive in one call or chunked calls, as
// long as chunk sizes are multiples of kDropletsPerRound.
constexpr int kDropletBatch = 2048;
constexpr int kBatchesPerRound = 8;
constexpr int kDropletsPerRound = kDropletBatch * kBatchesPerRound;

class TerrainEngine {
public:
    TerrainEngine() = default;

    void Initialize(const TerrainParams& params);
    void GenerateHeightmap();

    // Resets height, sediment, flow, snow, and water to a flat empty state.
    void ClearTerrain();

    // Adds a noise field onto the existing terrain with the given blend mode.
    // Respects the active mask. Bedrock/sediment re-split 80/20 afterwards.
    void ApplyNoise(const NoiseLayerParams& p);

    // Applies a primitive shape stamp. Respects the active mask.
    void ApplyStamp(const StampParams& p);

    // Rasterizes a stamp's influence field (0..1) into the scratch buffer
    // (ScratchMask()) without touching the terrain — used for shape masks.
    void StampToScratch(const StampParams& p);

    // Snow accumulation + creep settling into the dedicated snow field.
    // Respects the active mask.
    void ApplySnow(const SnowParams& p);

    // Priority-flood water fill: water depth per cell into the water field.
    void ComputeWater();

    // Iteration counts are rounded up to whole batches (kDropletBatch).
    void ApplyHydraulicErosion(int iterations, const HydraulicParams& p = {});
    void ApplyThermalWeathering(int passes, const ThermalParams& p = {});
    void ApplyFluvialErosion(int iterations, const FluvialParams& p = {});

    // Shaping modifiers (deterministic, run any time after generation).
    void ApplyTerrace(float interval, float strength, float sharpness);
    void ApplyPlateau(float plateauHeight, float softness);

    void Carve(float x, float y, float radius, float depth);

    // Active mask: 0..1 per cell, multiplies the effect of every subsequent
    // layer operation. Copied; NULL/empty clears (mask = 1 everywhere).
    void SetMask(const float* data, int size);
    void ClearMask();

    // Optional precipitation map for SpawnMode::Precipitation. Values are
    // relative weights >= 0; the map is copied and bilinearly sampled.
    void SetPrecipitationMap(const float* data, int size);
    void ClearPrecipitationMap();

    // Builds interleaved-by-attribute mesh buffers (positions/normals/colors/
    // uvs/indices) into internal storage exposed via the accessors below.
    void BuildMesh();

    // Exporters fill the internal byte buffer (see ExportData()/ExportSize()).
    // All heightmap exporters normalize to [min,max] except R32/EXR which
    // store absolute heights.
    size_t ExportPNG16();
    size_t ExportR16();
    size_t ExportR32();
    size_t ExportEXR();
    size_t ExportOBJ();
    const uint8_t* ExportData() const { return m_ExportBuffer.data(); }
    size_t ExportSize() const { return m_ExportBuffer.size(); }

    int Size() const { return m_Params.size; }
    const TerrainParams& Params() const { return m_Params; }

    float GetHeight(int x, int y) const;
    float GetBedrock(int x, int y) const;
    float GetSediment(int x, int y) const;
    float GetFlow(int x, int y) const;
    float GetSlope(int x, int y) const;

    const std::vector<float>& BedrockMap() const { return m_Bedrock; }
    const std::vector<float>& SedimentMap() const { return m_Sediment; }
    const std::vector<float>& FlowMap() const { return m_Flow; }
    const std::vector<float>& SnowMap() const { return m_Snow; }
    const std::vector<float>& WaterMap() const { return m_Water; }
    const std::vector<float>& ScratchMask() const { return m_Scratch; }
    std::vector<float>& BedrockMap() { return m_Bedrock; }
    std::vector<float>& SedimentMap() { return m_Sediment; }
    std::vector<float>& FlowMap() { return m_Flow; }
    std::vector<float>& SnowMap() { return m_Snow; }
    std::vector<float>& WaterMap() { return m_Water; }

    // Filled by BuildMesh().
    const std::vector<float>& MeshPositions() const { return m_MeshPositions; }
    const std::vector<float>& MeshNormals() const { return m_MeshNormals; }
    const std::vector<float>& MeshColors() const { return m_MeshColors; }
    const std::vector<float>& MeshUVs() const { return m_MeshUVs; }
    const std::vector<float>& MeshSnow() const { return m_MeshSnow; }
    const std::vector<uint32_t>& MeshIndices() const { return m_MeshIndices; }

    // Layered strata hardness at a given bedrock elevation, in [0.4, 1.6].
    float HardnessAt(float height) const;

private:
    bool InBounds(int x, int y) const {
        return x >= 0 && x < m_Params.size && y >= 0 && y < m_Params.size;
    }
    int Index(int x, int y) const { return y * m_Params.size + x; }

    // Bilinear helpers used by the droplet simulation (read main maps).
    float SampleHeight(float x, float y) const;
    float SamplePrecipitation(float x, float y) const;
    void GradientAt(float x, float y, float& gx, float& gy) const;

    // Active mask access: 1.0 when no mask is set.
    float MaskAt(int i) const {
        return m_Mask.empty() ? 1.0f : m_Mask[static_cast<size_t>(i)];
    }
    float SampleMask(float x, float y) const;

    // Re-split combined height into the 80/20 bedrock/sediment model after
    // direct height edits (noise layers, stamps).
    void ResplitHeight(const std::vector<float>& newTotal);

    // One batch of droplets simulated against the current maps, writing all
    // changes into the caller's delta buffers. `firstDroplet` is the global
    // droplet index (drives per-droplet RNG — order-independent).
    void RunDropletBatch(uint64_t firstDroplet, int count, const HydraulicParams& p,
                         std::vector<float>& sedimentDelta,
                         std::vector<float>& bedrockDelta,
                         std::vector<float>& flowDelta) const;

    void CollectHeightRange(float& minH, float& maxH) const;

    TerrainParams m_Params;
    std::vector<float> m_Bedrock;
    std::vector<float> m_Sediment;
    std::vector<float> m_Flow;
    std::vector<float> m_Snow;
    std::vector<float> m_Water;
    std::vector<float> m_Mask;          // empty = no mask (1 everywhere)
    std::vector<float> m_Scratch;       // stamp/selector rasterization target
    std::vector<float> m_Precipitation; // empty when unset
    uint64_t m_DropletCursor = 0;       // global droplet index across chunked calls

    std::vector<float> m_MeshPositions;
    std::vector<float> m_MeshNormals;
    std::vector<float> m_MeshColors;
    std::vector<float> m_MeshUVs;
    std::vector<float> m_MeshSnow;      // per-vertex snow depth
    std::vector<uint32_t> m_MeshIndices;

    std::vector<uint8_t> m_ExportBuffer;
};

} // namespace Titan
