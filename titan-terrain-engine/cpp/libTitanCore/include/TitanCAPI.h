#pragma once

// Flat C API over libTitanCore.
//
// This is the only boundary the outside world touches: raw pointers and
// primitive types, no STL, no C++ ABI. The same surface is consumed by
//   - the WASM build (TitanLab web viewer),
//   - the Swift/Metal macOS app (via a bridging header),
//   - the Unreal plugin (TitanBridge).

#include <stdint.h>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#define TITAN_API extern "C" EMSCRIPTEN_KEEPALIVE
#elif defined(__cplusplus)
#define TITAN_API extern "C"
#else
#define TITAN_API
#endif

// API stability contract: functions below are frozen as of version 1.
// Additions are allowed; signature changes require a major bump.
#define TITAN_C_API_VERSION 1

typedef struct TitanHandle TitanHandle;

TITAN_API TitanHandle* titan_create(void);
TITAN_API void titan_destroy(TitanHandle* handle);

TITAN_API const char* titan_version(void);
TITAN_API int titan_api_version(void);

// Reallocates the terrain and stores generation parameters.
// noiseType: 0 = none/flat, 1 = standard fBm, 2 = ridged multifractal, 3 = billow.
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
                               float originY);

TITAN_API void titan_generate(TitanHandle* handle);

// spawnMode: 0 = uniform, 1 = altitude-weighted, 2 = precipitation map.
// Iteration counts round up to whole batches of 2048 droplets; chunked calls
// with multiples of 16384 are bit-identical to one large call.
TITAN_API void titan_erode_hydraulic(TitanHandle* handle, int iterations, int spawnMode);
TITAN_API void titan_erode_thermal(TitanHandle* handle, int passes,
                                   float talusAngleDeg, float rate);
TITAN_API void titan_erode_fluvial(TitanHandle* handle, int iterations, float strength);

// Shaping modifiers.
TITAN_API void titan_apply_terrace(TitanHandle* handle, float interval,
                                   float strength, float sharpness);
TITAN_API void titan_apply_plateau(TitanHandle* handle, float plateauHeight,
                                   float softness);

// Precipitation map for spawnMode 2 (size*size floats, copied; NULL clears).
TITAN_API void titan_set_precipitation(TitanHandle* handle, const float* data, int size);

// --- v0.4: masks, noise stacking, stamps, snow, water --------------------

// Active mask (0..1 per cell, copied; NULL clears). Multiplies the effect
// of every subsequent layer operation until changed.
TITAN_API void titan_set_mask(TitanHandle* handle, const float* data, int size);

// Reset height/flow/snow/water to a flat empty state (stack rebuild start).
TITAN_API void titan_clear_terrain(TitanHandle* handle);

// Adds a noise field with a blend mode. noiseType: 0 none, 1 simplex fBm,
// 2 ridged, 3 billow, 4 voronoi cells, 5 voronoi ridges. blendMode: 0 add,
// 1 subtract, 2 multiply, 3 max, 4 min, 5 mix (by blendAlpha).
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
                                 float blendAlpha);

// Primitive stamp. shape: 0 dome, 1 rectangle, 2 ridge, 3 crater.
// op: 0 raise, 1 lower, 2 flatten, 3 union. Center/size in cells.
TITAN_API void titan_apply_stamp(TitanHandle* handle, int shape,
                                 float centerX, float centerY,
                                 float sizeX, float sizeY,
                                 float rotationDeg, float height,
                                 float falloff, int op);

// Rasterize a stamp's 0..1 field into the scratch buffer (no terrain edit);
// read it via titan_scratch_ptr. Used to build shape masks.
TITAN_API void titan_stamp_to_mask(TitanHandle* handle, int shape,
                                   float centerX, float centerY,
                                   float sizeX, float sizeY,
                                   float rotationDeg, float falloff);
TITAN_API float* titan_scratch_ptr(TitanHandle* handle);

// Snow accumulation + settle + melt into the snow field.
TITAN_API void titan_apply_snow(TitanHandle* handle, float snowLine,
                                float amount, float maxSlopeDeg,
                                int settlePasses, float melt);

// Priority-flood lake fill; depths into the water field.
TITAN_API void titan_compute_water(TitanHandle* handle);

TITAN_API float* titan_snow_ptr(TitanHandle* handle);
TITAN_API float* titan_water_ptr(TitanHandle* handle);

// Full-parameter erosion variants (the short forms use engine defaults).
TITAN_API void titan_erode_hydraulic_ex(TitanHandle* handle, int iterations,
                                        int spawnMode, float inertia,
                                        float capacity, float minCapacity,
                                        float dissolve, float deposit,
                                        float evaporate, float gravity,
                                        int lifetime, float radius,
                                        float bedrockSpeed);
TITAN_API void titan_erode_thermal_ex(TitanHandle* handle, int passes,
                                      float talusAngleDeg, float rate,
                                      float bedrockBreakdownRate);
TITAN_API void titan_erode_fluvial_ex(TitanHandle* handle, int iterations,
                                      float strength, float erodeConstant,
                                      float areaExponent, float slopeExponent,
                                      float depositRatio, float maxStep);

TITAN_API void titan_carve(TitanHandle* handle, float x, float y,
                           float radius, float depth);

// Exporters: each returns the byte size and fills an internal buffer read
// via titan_export_data_ptr. Formats: png16 (16-bit grayscale PNG), r16
// (RAW uint16 LE, normalized), r32 (RAW float32 LE, absolute), exr
// (uncompressed float RGB), obj (Wavefront mesh).
TITAN_API int titan_export_png16(TitanHandle* handle);
TITAN_API int titan_export_r16(TitanHandle* handle);
TITAN_API int titan_export_r32(TitanHandle* handle);
TITAN_API int titan_export_exr(TitanHandle* handle);
TITAN_API int titan_export_obj(TitanHandle* handle);
TITAN_API const uint8_t* titan_export_data_ptr(TitanHandle* handle);

// Raw layer buffers (size * size floats, row-major).
TITAN_API int titan_size(TitanHandle* handle);
TITAN_API float* titan_bedrock_ptr(TitanHandle* handle);
TITAN_API float* titan_sediment_ptr(TitanHandle* handle);
TITAN_API float* titan_flow_ptr(TitanHandle* handle);

// Point probes (used by the inspector UI).
TITAN_API float titan_height_at(TitanHandle* handle, int x, int y);
TITAN_API float titan_sediment_at(TitanHandle* handle, int x, int y);
TITAN_API float titan_flow_at(TitanHandle* handle, int x, int y);
TITAN_API float titan_slope_at(TitanHandle* handle, int x, int y);

// Mesh: call titan_build_mesh, then read the buffers.
// positions/normals: vertexCount * 3 floats. colors: vertexCount * 4.
// uvs: vertexCount * 2. indices: indexCount uint32.
TITAN_API void titan_build_mesh(TitanHandle* handle);
TITAN_API int titan_mesh_vertex_count(TitanHandle* handle);
TITAN_API int titan_mesh_index_count(TitanHandle* handle);
TITAN_API float* titan_mesh_positions_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_normals_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_colors_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_uvs_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_snow_ptr(TitanHandle* handle); // vertexCount floats
TITAN_API uint32_t* titan_mesh_indices_ptr(TitanHandle* handle);
