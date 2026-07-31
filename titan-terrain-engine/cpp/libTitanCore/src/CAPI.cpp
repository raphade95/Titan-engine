#include "TitanCAPI.h"
#include "TitanCore.h"
#include "TitanRandom.h"

#include <cstring>
#include <exception>
#include <new>
#include <string>

// Every entry point below is wrapped in TITAN_GUARD / TITAN_GUARD_RET.
//
// These functions are `extern "C"`, and letting a C++ exception propagate
// across that boundary is undefined behaviour — in a DLL loaded by the Unreal
// editor it is a hard crash with no diagnostic. std::bad_alloc from a large
// terrain, or std::length_error from a bad size, are both reachable from
// ordinary callers. The guards convert them into a readable message that a
// host can fetch with titan_last_error(), and a benign return value.
namespace {

thread_local std::string g_LastError;

Titan::TerrainEngine* Engine(TitanHandle* handle) {
    return reinterpret_cast<Titan::TerrainEngine*>(handle);
}

void SetError(const char* what) {
    g_LastError = what ? what : "unknown error";
}

} // namespace

// Variadic: the wrapped bodies contain commas (declarations, call arguments),
// which a fixed-arity function-like macro would split on.
#define TITAN_GUARD(...)                                         \
    try { __VA_ARGS__ }                                          \
    catch (const std::exception& e) { SetError(e.what()); }      \
    catch (...) { SetError("unknown error"); }

#define TITAN_GUARD_RET(fallback, ...)                           \
    try { __VA_ARGS__ }                                          \
    catch (const std::exception& e) { SetError(e.what()); }      \
    catch (...) { SetError("unknown error"); }                   \
    return fallback;

TITAN_API const char* titan_last_error(void) {
    return g_LastError.empty() ? nullptr : g_LastError.c_str();
}

TITAN_API void titan_clear_error(void) {
    g_LastError.clear();
}

TITAN_API TitanHandle* titan_create(void) {
    TITAN_GUARD_RET(nullptr, {
        return reinterpret_cast<TitanHandle*>(new Titan::TerrainEngine());
    })
}

TITAN_API void titan_destroy(TitanHandle* handle) {
    TITAN_GUARD({ delete Engine(handle); })
}

TITAN_API const char* titan_version(void) {
    return "libTitanCore 0.5.0";
}

TITAN_API int titan_api_version(void) {
    return TITAN_C_API_VERSION;
}

TITAN_API uint32_t titan_hash_seed(const char* utf8) {
    return Titan::HashSeedUtf8(utf8);
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
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
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
    })
}

TITAN_API void titan_generate(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->GenerateHeightmap();
    })
}

TITAN_API void titan_erode_hydraulic(TitanHandle* handle, int iterations, int spawnMode) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::HydraulicParams p;
        p.spawnMode = spawnMode;
        Engine(handle)->ApplyHydraulicErosion(iterations, p);
    })
}

TITAN_API void titan_apply_terrace(TitanHandle* handle, float interval,
                                   float strength, float sharpness) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyTerrace(interval, strength, sharpness);
    })
}

TITAN_API void titan_apply_plateau(TitanHandle* handle, float plateauHeight,
                                   float softness) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyPlateau(plateauHeight, softness);
    })
}

TITAN_API void titan_set_precipitation(TitanHandle* handle, const float* data, int size) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        if (data) {
            Engine(handle)->SetPrecipitationMap(data, size);
        } else {
            Engine(handle)->ClearPrecipitationMap();
        }
    })
}

TITAN_API int64_t titan_export_png16(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportPNG16());
    })
}

TITAN_API int64_t titan_export_r16(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportR16());
    })
}

TITAN_API int64_t titan_export_r32(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportR32());
    })
}

TITAN_API int64_t titan_export_exr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportEXR());
    })
}

TITAN_API int64_t titan_export_obj(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportOBJ());
    })
}

TITAN_API const uint8_t* titan_export_data_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return Engine(handle)->ExportData();
    })
}

TITAN_API void titan_erode_thermal(TitanHandle* handle, int passes,
                                   float talusAngleDeg, float rate) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::ThermalParams p;
        p.talusAngleDeg = talusAngleDeg;
        p.rate = rate;
        Engine(handle)->ApplyThermalWeathering(passes, p);
    })
}

TITAN_API void titan_erode_fluvial(TitanHandle* handle, int iterations, float strength) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::FluvialParams p;
        p.strength = strength;
        Engine(handle)->ApplyFluvialErosion(iterations, p);
    })
}

TITAN_API void titan_carve(TitanHandle* handle, float x, float y,
                           float radius, float depth) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->Carve(x, y, radius, depth);
    })
}

TITAN_API void titan_apply_clamp(TitanHandle* handle, float minH, float maxH) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyClamp(minH, maxH);
    })
}

TITAN_API void titan_apply_transform(TitanHandle* handle, float scaleV,
                                     float offset, int invert) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyTransform(scaleV, offset, invert != 0);
    })
}

TITAN_API void titan_apply_blur(TitanHandle* handle, float radius, float strength) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyBlur(radius, strength);
    })
}

TITAN_API void titan_apply_sharpen(TitanHandle* handle, float radius, float strength) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplySharpen(radius, strength);
    })
}

TITAN_API void titan_apply_curve(TitanHandle* handle, const float* xs,
                                 const float* ys, int count) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyCurve(xs, ys, count);
    })
}

TITAN_API void titan_sample_curve(const float* xs, const float* ys, int count,
                                  float* out, int samples) {
    TITAN_GUARD({
        Titan::TerrainEngine::SampleCurve(xs, ys, count, out, samples);
    })
}

TITAN_API void titan_apply_heightfield(TitanHandle* handle, const float* data,
                                       int srcSize, float heightScale,
                                       int blendMode, float alpha) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ApplyHeightfield(data, srcSize, heightScale, blendMode, alpha);
    })
}

TITAN_API void titan_compute_slope_map(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ComputeSlopeMap();
    })
}

TITAN_API void titan_compute_curvature_map(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ComputeCurvatureMap();
    })
}

TITAN_API void titan_compute_ao(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({ Engine(handle)->ComputeAOField(); })
}

TITAN_API float* titan_ao_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        const auto& v = Engine(handle)->AOField();
        return v.empty() ? nullptr : const_cast<float*>(v.data());
    })
}

TITAN_API void titan_mask_by_feature(TitanHandle* handle, int feature,
                                     float rangeLo, float rangeHi,
                                     float softness, int invert) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->MaskByFeature(feature, rangeLo, rangeHi, softness, invert != 0);
    })
}

TITAN_API void titan_noise_to_mask(TitanHandle* handle, unsigned int seedOffset,
                                   int noiseType, float scale, int octaves,
                                   float persistence, float lacunarity,
                                   float warpStrength) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::NoiseLayerParams p;
        p.seedOffset = seedOffset;
        p.noiseType = noiseType;
        p.scale = scale;
        p.octaves = octaves;
        p.persistence = persistence;
        p.lacunarity = lacunarity;
        p.warpStrength = warpStrength;
        Engine(handle)->NoiseToScratch(p);
    })
}

TITAN_API void titan_set_mask_from_scratch(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->SetMaskFromScratch();
    })
}

TITAN_API int64_t titan_export_normal_png(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportNormalPNG());
    })
}

TITAN_API int64_t titan_export_ao_png(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportAOPNG());
    })
}

TITAN_API void titan_set_mask(TitanHandle* handle, const float* data, int size) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        if (data) {
            Engine(handle)->SetMask(data, size);
        } else {
            Engine(handle)->ClearMask();
        }
    })
}

TITAN_API void titan_clear_terrain(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ClearTerrain();
    })
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
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
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
    })
}

TITAN_API void titan_apply_stamp(TitanHandle* handle, int shape,
                                 float centerX, float centerY,
                                 float sizeX, float sizeY,
                                 float rotationDeg, float height,
                                 float falloff, int op) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
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
    })
}

TITAN_API void titan_stamp_to_mask(TitanHandle* handle, int shape,
                                   float centerX, float centerY,
                                   float sizeX, float sizeY,
                                   float rotationDeg, float falloff) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::StampParams p;
        p.shape = shape;
        p.centerX = centerX;
        p.centerY = centerY;
        p.sizeX = sizeX;
        p.sizeY = sizeY;
        p.rotationDeg = rotationDeg;
        p.falloff = falloff;
        Engine(handle)->StampToScratch(p);
    })
}

TITAN_API float* titan_scratch_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->ScratchMask().data());
    })
}

TITAN_API void titan_apply_snow(TitanHandle* handle, float snowLine,
                                float amount, float maxSlopeDeg,
                                int settlePasses, float melt) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::SnowParams p;
        p.snowLine = snowLine;
        p.amount = amount;
        p.maxSlopeDeg = maxSlopeDeg;
        p.settlePasses = settlePasses;
        p.melt = melt;
        Engine(handle)->ApplySnow(p);
    })
}

TITAN_API void titan_compute_water(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->ComputeWater();
    })
}

TITAN_API float* titan_snow_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return Engine(handle)->SnowMap().data();
    })
}

TITAN_API float* titan_water_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return Engine(handle)->WaterMap().data();
    })
}

TITAN_API void titan_apply_volcano(TitanHandle* handle,
                                   float centerX, float centerY,
                                   float radius, float height,
                                   float coneExponent,
                                   float craterRadius, float craterDepth,
                                   float rimJaggedness, float roughness,
                                   float breachAngleDeg, float breachWidthDeg,
                                   unsigned int seedOffset) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::VolcanoParams p;
        p.centerX = centerX;
        p.centerY = centerY;
        p.radius = radius;
        p.height = height;
        p.coneExponent = coneExponent;
        p.craterRadius = craterRadius;
        p.craterDepth = craterDepth;
        p.rimJaggedness = rimJaggedness;
        p.roughness = roughness;
        p.breachAngleDeg = breachAngleDeg;
        p.breachWidthDeg = breachWidthDeg;
        p.seedOffset = seedOffset;
        Engine(handle)->ApplyVolcano(p);
    })
}

TITAN_API void titan_simulate_lava(TitanHandle* handle, int steps,
                                   float eruptionRate, float viscosity,
                                   float solidifyRate, float coolRate,
                                   float ventRadius, int sustain) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::LavaParams p;
        p.steps = steps;
        p.eruptionRate = eruptionRate;
        p.viscosity = viscosity;
        p.solidifyRate = solidifyRate;
        p.coolRate = coolRate;
        p.ventRadius = ventRadius;
        p.sustain = sustain;
        Engine(handle)->SimulateLava(p);
    })
}

TITAN_API void titan_clear_lava(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({ Engine(handle)->ClearLava(); })
}

TITAN_API int titan_vent_count(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, { return Engine(handle)->VentCount(); })
}

// The lava buffers are empty until something erupts, and `.data()` on an empty
// vector may be null — which is the contract these advertise, so hosts must
// null-check rather than assume a live grid the way they can for bedrock.
TITAN_API float* titan_lava_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        auto& v = Engine(handle)->LavaMap();
        return v.empty() ? nullptr : v.data();
    })
}

TITAN_API float* titan_lava_rock_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        auto& v = Engine(handle)->LavaRockMap();
        return v.empty() ? nullptr : v.data();
    })
}

TITAN_API float* titan_lava_heat_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        auto& v = Engine(handle)->LavaHeatMap();
        return v.empty() ? nullptr : v.data();
    })
}

TITAN_API float* titan_lava_glow_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        auto& v = Engine(handle)->LavaGlowMap();
        return v.empty() ? nullptr : v.data();
    })
}

TITAN_API void titan_erode_hydraulic_ex(TitanHandle* handle, int iterations,
                                        int spawnMode, float inertia,
                                        float capacity, float minCapacity,
                                        float dissolve, float deposit,
                                        float evaporate, float gravity,
                                        int lifetime, float radius,
                                        float bedrockSpeed) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
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
    })
}

TITAN_API void titan_erode_thermal_ex(TitanHandle* handle, int passes,
                                      float talusAngleDeg, float rate,
                                      float bedrockBreakdownRate) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::ThermalParams p;
        p.talusAngleDeg = talusAngleDeg;
        p.rate = rate;
        p.bedrockBreakdownRate = bedrockBreakdownRate;
        Engine(handle)->ApplyThermalWeathering(passes, p);
    })
}

TITAN_API void titan_erode_fluvial_ex(TitanHandle* handle, int iterations,
                                      float strength, float erodeConstant,
                                      float areaExponent, float slopeExponent,
                                      float depositRatio, float maxStep) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Titan::FluvialParams p;
        p.strength = strength;
        p.erodeConstant = erodeConstant;
        p.areaExponent = areaExponent;
        p.slopeExponent = slopeExponent;
        p.depositRatio = depositRatio;
        p.maxStep = maxStep;
        Engine(handle)->ApplyFluvialErosion(iterations, p);
    })
}

TITAN_API int titan_size(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return Engine(handle)->Size();
    })
}

TITAN_API void titan_height_range(TitanHandle* handle, float* outMin, float* outMax) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        float lo = 0.0f, hi = 0.0f;
        Engine(handle)->HeightRange(lo, hi);
        if (outMin) *outMin = lo;
        if (outMax) *outMax = hi;
    })
}

TITAN_API void titan_export_height_range(TitanHandle* handle, float* outMin, float* outMax) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        float lo = 0.0f, hi = 0.0f;
        Engine(handle)->ExportHeightRange(lo, hi);
        if (outMin) *outMin = lo;
        if (outMax) *outMax = hi;
    })
}

TITAN_API int64_t titan_export_tile(TitanHandle* handle, int tileX, int tileY,
                                    int tilesPerSide, int overlap, int format) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(
            Engine(handle)->ExportTile(tileX, tileY, tilesPerSide, overlap, format));
    })
}

TITAN_API int titan_tile_resolution(TitanHandle* handle, int tilesPerSide,
                                    int overlap) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return Engine(handle)->TileResolution(tilesPerSide, overlap);
    })
}

TITAN_API void titan_mass_balance(TitanHandle* handle, double* exported, double* created) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        if (exported) *exported = Engine(handle)->MassExported();
        if (created) *created = Engine(handle)->MassCreated();
    })
}

TITAN_API float* titan_bedrock_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return Engine(handle)->BedrockMap().data();
    })
}

TITAN_API float* titan_sediment_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return Engine(handle)->SedimentMap().data();
    })
}

TITAN_API float* titan_flow_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return Engine(handle)->FlowMap().data();
    })
}

TITAN_API float titan_height_at(TitanHandle* handle, int x, int y) {
    if (!handle) { SetError("null handle"); return 0.0f; }
    TITAN_GUARD_RET(0.0f, {
        return Engine(handle)->GetHeight(x, y);
    })
}

TITAN_API float titan_sediment_at(TitanHandle* handle, int x, int y) {
    if (!handle) { SetError("null handle"); return 0.0f; }
    TITAN_GUARD_RET(0.0f, {
        return Engine(handle)->GetSediment(x, y);
    })
}

TITAN_API float titan_flow_at(TitanHandle* handle, int x, int y) {
    if (!handle) { SetError("null handle"); return 0.0f; }
    TITAN_GUARD_RET(0.0f, {
        return Engine(handle)->GetFlow(x, y);
    })
}

TITAN_API float titan_slope_at(TitanHandle* handle, int x, int y) {
    if (!handle) { SetError("null handle"); return 0.0f; }
    TITAN_GUARD_RET(0.0f, {
        return Engine(handle)->GetSlope(x, y);
    })
}

TITAN_API int64_t titan_export_splat_png(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int64_t>(Engine(handle)->ExportSplatPNG());
    })
}

TITAN_API void titan_band_scratch(TitanHandle* handle, float lo, float hi,
                                  float softness, int invert) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({ Engine(handle)->BandScratch(lo, hi, softness, invert != 0); })
}

TITAN_API void titan_build_mesh_lod(TitanHandle* handle, int maxEdgeVertices) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        const int size = Engine(handle)->Size();
        int stride = 1;
        if (maxEdgeVertices > 1 && size > maxEdgeVertices) {
            stride = (size + maxEdgeVertices - 1) / maxEdgeVertices;
        }
        Engine(handle)->BuildMesh(stride);
    })
}

TITAN_API int titan_mesh_stride(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, { return Engine(handle)->MeshStride(); })
}

TITAN_API int titan_mesh_edge_vertices(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, { return Engine(handle)->MeshEdgeVertices(); })
}

TITAN_API void titan_build_mesh(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return; }
    TITAN_GUARD({
        Engine(handle)->BuildMesh();
    })
}

TITAN_API int titan_mesh_vertex_count(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int>(Engine(handle)->MeshPositions().size() / 3);
    })
}

TITAN_API int titan_mesh_index_count(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return 0; }
    TITAN_GUARD_RET(0, {
        return static_cast<int>(Engine(handle)->MeshIndices().size());
    })
}

TITAN_API float* titan_mesh_positions_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshPositions().data());
    })
}

TITAN_API float* titan_mesh_normals_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshNormals().data());
    })
}

TITAN_API float* titan_mesh_colors_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshColors().data());
    })
}

TITAN_API float* titan_mesh_uvs_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshUVs().data());
    })
}

TITAN_API float* titan_mesh_snow_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshSnow().data());
    })
}

TITAN_API float* titan_mesh_lava_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshLava().data());
    })
}

TITAN_API float* titan_mesh_surface_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<float*>(Engine(handle)->MeshSurface().data());
    })
}

TITAN_API uint32_t* titan_mesh_indices_ptr(TitanHandle* handle) {
    if (!handle) { SetError("null handle"); return nullptr; }
    TITAN_GUARD_RET(nullptr, {
        return const_cast<uint32_t*>(Engine(handle)->MeshIndices().data());
    })
}
