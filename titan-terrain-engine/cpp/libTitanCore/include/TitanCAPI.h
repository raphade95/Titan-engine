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
TITAN_API uint32_t* titan_mesh_indices_ptr(TitanHandle* handle);
