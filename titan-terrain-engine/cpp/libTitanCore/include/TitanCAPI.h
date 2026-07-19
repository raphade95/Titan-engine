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

typedef struct TitanHandle TitanHandle;

TITAN_API TitanHandle* titan_create(void);
TITAN_API void titan_destroy(TitanHandle* handle);

TITAN_API const char* titan_version(void);

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

TITAN_API void titan_erode_hydraulic(TitanHandle* handle, int iterations);
TITAN_API void titan_erode_thermal(TitanHandle* handle, int passes,
                                   float talusAngleDeg, float rate);
TITAN_API void titan_erode_fluvial(TitanHandle* handle, int iterations, float strength);

TITAN_API void titan_carve(TitanHandle* handle, float x, float y,
                           float radius, float depth);

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
