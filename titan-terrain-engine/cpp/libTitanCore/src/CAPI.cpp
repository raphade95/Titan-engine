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
    return "libTitanCore 0.3.0";
}

TITAN_API int titan_api_version(void) {
    return TITAN_C_API_VERSION;
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

TITAN_API void titan_erode_hydraulic(TitanHandle* handle, int iterations, int spawnMode) {
    Titan::HydraulicParams p;
    p.spawnMode = spawnMode;
    Engine(handle)->ApplyHydraulicErosion(iterations, p);
}

TITAN_API void titan_apply_terrace(TitanHandle* handle, float interval,
                                   float strength, float sharpness) {
    Engine(handle)->ApplyTerrace(interval, strength, sharpness);
}

TITAN_API void titan_apply_plateau(TitanHandle* handle, float plateauHeight,
                                   float softness) {
    Engine(handle)->ApplyPlateau(plateauHeight, softness);
}

TITAN_API void titan_set_precipitation(TitanHandle* handle, const float* data, int size) {
    if (data) {
        Engine(handle)->SetPrecipitationMap(data, size);
    } else {
        Engine(handle)->ClearPrecipitationMap();
    }
}

TITAN_API int titan_export_png16(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->ExportPNG16());
}

TITAN_API int titan_export_r16(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->ExportR16());
}

TITAN_API int titan_export_r32(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->ExportR32());
}

TITAN_API int titan_export_exr(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->ExportEXR());
}

TITAN_API int titan_export_obj(TitanHandle* handle) {
    return static_cast<int>(Engine(handle)->ExportOBJ());
}

TITAN_API const uint8_t* titan_export_data_ptr(TitanHandle* handle) {
    return Engine(handle)->ExportData();
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

TITAN_API void titan_set_mask(TitanHandle* handle, const float* data, int size) {
    if (data) {
        Engine(handle)->SetMask(data, size);
    } else {
        Engine(handle)->ClearMask();
    }
}

TITAN_API void titan_clear_terrain(TitanHandle* handle) {
    Engine(handle)->ClearTerrain();
}

TITAN_API void titan_apply_noise(TitanHandle* handle,
                                 unsigned int seedOffset,
                                 int noiseType,
                                 float scale,
                                 float amplitude,
                                 int octaves,
                                 float persistence,
                                 float lacunarity,
                                 float exponent,
                                 float warpStrength,
                                 int blendMode,
                                 float blendAlpha) {
    Titan::NoiseLayerParams p;
    p.seedOffset = seedOffset;
    p.noiseType = noiseType;
    p.scale = scale;
    p.amplitude = amplitude;
    p.octaves = octaves;
    p.persistence = persistence;
    p.lacunarity = lacunarity;
    p.exponent = exponent;
    p.warpStrength = warpStrength;
    p.blendMode = blendMode;
    p.blendAlpha = blendAlpha;
    Engine(handle)->ApplyNoise(p);
}

TITAN_API void titan_apply_stamp(TitanHandle* handle, int shape,
                                 float centerX, float centerY,
                                 float sizeX, float sizeY,
                                 float rotationDeg, float height,
                                 float falloff, int op) {
    Titan::StampParams p;
    p.shape = shape;
    p.centerX = centerX;
    p.centerY = centerY;
    p.sizeX = sizeX;
    p.sizeY = sizeY;
    p.rotationDeg = rotationDeg;
    p.height = height;
    p.falloff = falloff;
    p.op = op;
    Engine(handle)->ApplyStamp(p);
}

TITAN_API void titan_stamp_to_mask(TitanHandle* handle, int shape,
                                   float centerX, float centerY,
                                   float sizeX, float sizeY,
                                   float rotationDeg, float falloff) {
    Titan::StampParams p;
    p.shape = shape;
    p.centerX = centerX;
    p.centerY = centerY;
    p.sizeX = sizeX;
    p.sizeY = sizeY;
    p.rotationDeg = rotationDeg;
    p.falloff = falloff;
    Engine(handle)->StampToScratch(p);
}

TITAN_API float* titan_scratch_ptr(TitanHandle* handle) {
    return const_cast<float*>(Engine(handle)->ScratchMask().data());
}

TITAN_API void titan_apply_snow(TitanHandle* handle, float snowLine,
                                float amount, float maxSlopeDeg,
                                int settlePasses, float melt) {
    Titan::SnowParams p;
    p.snowLine = snowLine;
    p.amount = amount;
    p.maxSlopeDeg = maxSlopeDeg;
    p.settlePasses = settlePasses;
    p.melt = melt;
    Engine(handle)->ApplySnow(p);
}

TITAN_API void titan_compute_water(TitanHandle* handle) {
    Engine(handle)->ComputeWater();
}

TITAN_API float* titan_snow_ptr(TitanHandle* handle) {
    return Engine(handle)->SnowMap().data();
}

TITAN_API float* titan_water_ptr(TitanHandle* handle) {
    return Engine(handle)->WaterMap().data();
}

TITAN_API void titan_erode_hydraulic_ex(TitanHandle* handle, int iterations,
                                        int spawnMode, float inertia,
                                        float capacity, float minCapacity,
                                        float dissolve, float deposit,
                                        float evaporate, float gravity,
                                        int lifetime, float radius,
                                        float bedrockSpeed) {
    Titan::HydraulicParams p;
    p.spawnMode = spawnMode;
    p.inertia = inertia;
    p.sedimentCapacityFactor = capacity;
    p.minSedimentCapacity = minCapacity;
    p.dissolveSpeed = dissolve;
    p.depositSpeed = deposit;
    p.evaporateSpeed = evaporate;
    p.gravity = gravity;
    p.maxDropletLifetime = lifetime;
    p.erosionRadius = radius;
    p.bedrockErosionSpeed = bedrockSpeed;
    Engine(handle)->ApplyHydraulicErosion(iterations, p);
}

TITAN_API void titan_erode_thermal_ex(TitanHandle* handle, int passes,
                                      float talusAngleDeg, float rate,
                                      float bedrockBreakdownRate) {
    Titan::ThermalParams p;
    p.talusAngleDeg = talusAngleDeg;
    p.rate = rate;
    p.bedrockBreakdownRate = bedrockBreakdownRate;
    Engine(handle)->ApplyThermalWeathering(passes, p);
}

TITAN_API void titan_erode_fluvial_ex(TitanHandle* handle, int iterations,
                                      float strength, float erodeConstant,
                                      float areaExponent, float slopeExponent,
                                      float depositRatio, float maxStep) {
    Titan::FluvialParams p;
    p.strength = strength;
    p.erodeConstant = erodeConstant;
    p.areaExponent = areaExponent;
    p.slopeExponent = slopeExponent;
    p.depositRatio = depositRatio;
    p.maxStep = maxStep;
    Engine(handle)->ApplyFluvialErosion(iterations, p);
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

TITAN_API float* titan_mesh_snow_ptr(TitanHandle* handle) {
    return const_cast<float*>(Engine(handle)->MeshSnow().data());
}

TITAN_API uint32_t* titan_mesh_indices_ptr(TitanHandle* handle) {
    return const_cast<uint32_t*>(Engine(handle)->MeshIndices().data());
}
