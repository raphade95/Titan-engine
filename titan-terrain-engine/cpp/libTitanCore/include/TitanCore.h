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
    float depositRatio = 0.25f;   // fraction of eroded material re-deposited downstream
    float maxStep = 2.0f;         // absolute per-iteration erosion cap
};

class TerrainEngine {
public:
    TerrainEngine() = default;

    void Initialize(const TerrainParams& params);
    void GenerateHeightmap();

    void ApplyHydraulicErosion(int iterations, const HydraulicParams& p = {});
    void ApplyThermalWeathering(int passes, const ThermalParams& p = {});
    void ApplyFluvialErosion(int iterations, const FluvialParams& p = {});

    void Carve(float x, float y, float radius, float depth);

    // Builds interleaved-by-attribute mesh buffers (positions/normals/colors/
    // uvs/indices) into internal storage exposed via the accessors below.
    void BuildMesh();

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
    std::vector<float>& BedrockMap() { return m_Bedrock; }
    std::vector<float>& SedimentMap() { return m_Sediment; }
    std::vector<float>& FlowMap() { return m_Flow; }

    // Filled by BuildMesh().
    const std::vector<float>& MeshPositions() const { return m_MeshPositions; }
    const std::vector<float>& MeshNormals() const { return m_MeshNormals; }
    const std::vector<float>& MeshColors() const { return m_MeshColors; }
    const std::vector<float>& MeshUVs() const { return m_MeshUVs; }
    const std::vector<uint32_t>& MeshIndices() const { return m_MeshIndices; }

    // Layered strata hardness at a given bedrock elevation, in [0.4, 1.6].
    float HardnessAt(float height) const;

private:
    friend class HydraulicSimulator;

    bool InBounds(int x, int y) const {
        return x >= 0 && x < m_Params.size && y >= 0 && y < m_Params.size;
    }
    int Index(int x, int y) const { return y * m_Params.size + x; }

    // Bilinear helpers used by the droplet simulation.
    float SampleHeight(float x, float y) const;
    float SampleSediment(float x, float y) const;
    void GradientAt(float x, float y, float& gx, float& gy) const;

    void DepositSediment(float x, float y, float amount);
    // Return the amount actually removed (respects clamping at zero), so the
    // droplet's sediment load never claims material that didn't exist.
    float ErodeSedimentBrush(float x, float y, float amount, float radius);
    float ErodeBedrockBrush(float x, float y, float amount, float radius);

    TerrainParams m_Params;
    std::vector<float> m_Bedrock;
    std::vector<float> m_Sediment;
    std::vector<float> m_Flow;

    std::vector<float> m_MeshPositions;
    std::vector<float> m_MeshNormals;
    std::vector<float> m_MeshColors;
    std::vector<float> m_MeshUVs;
    std::vector<uint32_t> m_MeshIndices;
};

} // namespace Titan
