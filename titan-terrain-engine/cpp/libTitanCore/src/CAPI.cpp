#include "TitanCAPI.h"
#include "TitanCore.h"

namespace {

Titan::TerrainEngine* Engine(TitanHandle* handle) {
    return reinterpret_cast<Titan::TerrainEngine*>(handle);
}

} // namespace

TITAN_API TitanHandle* titan_create(void) {
    return reinterpret_cast<TitanHandle*>(new Titan::TerrainEngine());
}

TITAN_API void titan_destroy(TitanHandle* handle) {
    delete Engine(handle);
}

TITAN_API const char* titan_version(void) {
    return "libTitanCore 0.2.0";
}

TITAN_API void titan_configure(TitanHandle* handle,
                               int size,
                               float cellSize,
                               float scale,
                               float heightMultiplier,
                               uint32_t seed,
                               int octaves,
                               float persistence,
                               float lacunarity,
                               float exponent,
                               int noiseType,
                               float warpStrength,
                               float ridgeOffset,
                               float ridgeGain,
                               float originX,
                               float originY) {
    Titan::TerrainParams p;
    p.size = size;
    p.cellSize = cellSize;
    p.scale = scale;
    p.heightMultiplier = heightMultiplier;
    p.seed = seed;
    p.octaves = octaves;
    p.persistence = persistence;
    p.lacunarity = lacunarity;
    p.exponent = exponent;
    p.noiseType = noiseType;
    p.warpStrength = warpStrength;
    p.ridgeOffset = ridgeOffset;
    p.ridgeGain = ridgeGain;
    p.originX = originX;
    p.originY = originY;
    Engine(handle)->Initialize(p);
}

TITAN_API void titan_generate(TitanHandle* handle) {
    Engine(handle)->GenerateHeightmap();
}

TITAN_API void titan_erode_hydraulic(TitanHandle* handle, int iterations) {
    Engine(handle)->ApplyHydraulicErosion(iterations);
}

TITAN_API void titan_erode_thermal(TitanHandle* handle, int passes,
                                   float talusAngleDeg, float rate) {
    Titan::ThermalParams p;
    p.talusAngleDeg = talusAngleDeg;
    p.rate = rate;
    Engine(handle)->ApplyThermalWeathering(passes, p);
}

TITAN_API void titan_erode_fluvial(TitanHandle* handle, int iterations, float strength) {
    Titan::FluvialParams p;
    p.strength = strength;
    Engine(handle)->ApplyFluvialErosion(iterations, p);
}

TITAN_API void titan_carve(TitanHandle* handle, float x, float y,
                           float radius, float depth) {
    Engine(handle)->Carve(x, y, radius, depth);
}

TITAN_API int titan_size(TitanHandle* handle) {
    return Engine(handle)->Size();
}

TITAN_API float* titan_bedrock_ptr(TitanHandle* handle) {
    return Engine(handle)->BedrockMap().data();
}

TITAN_API float* titan_sediment_ptr(TitanHandle* handle) {
    return Engine(handle)->SedimentMap().data();
}

TITAN_API float* titan_flow_ptr(TitanHandle* handle) {
    return Engine(handle)->FlowMap().data();
}

TITAN_API float titan_height_at(TitanHandle* handle, int x, int y) {
    return Engine(handle)->GetHeight(x, y);
}

TITAN_API float titan_sediment_at(TitanHandle* handle, int x, int y) {
    return Engine(handle)->GetSediment(x, y);
}

TITAN_API float titan_flow_at(TitanHandle* handle, int x, int y) {
    return Engine(handle)->GetFlow(x, y);
}

TITAN_API float titan_slope_at(TitanHandle* handle, int x, int y) {
    return Engine(handle)->GetSlope(x, y);
}

TITAN_API void titan_build_mesh(TitanHandle* handle) {
    Engine(handle)->BuildMesh();
}

TITAN_API int titan_mesh_vertex_count(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->MeshPositions().size() / 3);
}

TITAN_API int titan_mesh_index_count(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->MeshIndices().size());
}

TITAN_API float* titan_mesh_positions_ptr(TitanHandle* handle) {
    return const_cast<float*>(Engine(handle)->MeshPositions().data());
}

TITAN_API float* titan_mesh_normals_ptr(TitanHandle* handle) {
    return const_cast<float*>(Engine(handle)->MeshNormals().data());
}

TITAN_API float* titan_mesh_colors_ptr(TitanHandle* handle) {
    return const_cast<float*>(Engine(handle)->MeshColors().data());
}

TITAN_API float* titan_mesh_uvs_ptr(TitanHandle* handle) {
    return const_cast<float*>(Engine(handle)->MeshUVs().data());
}

TITAN_API uint32_t* titan_mesh_indices_ptr(TitanHandle* handle) {
    return const_cast<uint32_t*>(Engine(handle)->MeshIndices().data());
}
