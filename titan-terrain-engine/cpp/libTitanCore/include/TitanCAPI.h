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

// Last error on this thread, or NULL if none. Every entry point catches C++
// exceptions rather than letting them cross the extern "C" boundary (which is
// undefined behaviour), records the message here, and returns a benign value.
// Out-of-memory on a large grid is the common real case.
TITAN_API const char* titan_last_error(void);
TITAN_API void titan_clear_error(void);

// Canonical seed-string hash: FNV-1a over UTF-8 bytes.
//
// Every host must route user-typed seeds through this rather than
// reimplementing the hash, so that one seed string means one terrain in the
// web lab, TitanLab, and the Unreal plugin alike. Pass UTF-8; a NULL pointer
// returns the FNV offset basis.
TITAN_API uint32_t titan_hash_seed(const char* utf8);

// Reallocates the terrain and stores generation parameters.
// noiseType: 0 = none/flat, 1 = standard fBm, 2 = ridged multifractal, 3 = billow.
//
// Every parameter is clamped into a safe range before anything is allocated:
// size to [2, 8192], octaves to [1, 16], non-finite floats to their defaults,
// and so on. Read them back with titan_size() to see what was actually used.
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
//
// Iteration counts round up to a whole number of 16384-droplet rounds, so a
// request for 20000 droplets simulates 32768. A round is the unit that
// composes: any sequence of calls is now bit-identical to one large call for
// the same total, whatever the chunking. (This previously rounded to
// 2048-droplet batches, which made the result depend on how a host sliced the
// work — the web lab and the desktop apps disagreed for non-multiples.)
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

// --- v0.11: whole-field read/write, for graph evaluation -------------------
//
// A node graph forks: two branches run from the same upstream state and are
// combined further down. That needs somewhere to park a branch's result, and
// a way to rewind the engine to an earlier surface — otherwise a host has to
// reimplement the operations to evaluate anything but a straight line.
//
// `count` is size*size; a short buffer is filled or read as far as it goes
// rather than overrunning. titan_set_height resets sediment, flow, snow,
// water and lava: they describe a history the incoming field does not have.
TITAN_API void titan_read_height(TitanHandle* handle, float* out, int count);
TITAN_API void titan_set_height(TitanHandle* handle, const float* data, int count);

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

// --- v0.7: volcanism ------------------------------------------------------

// Stamps a volcanic edifice and registers its eruption vent. Center and radius
// are in cells; height is the summit above the local ground in world units.
//
// This is not a Dome stamp with different numbers. The profile is concave-up
// (steepest at the summit, flaring at the base), it carries a jagged summit
// crater, radial barranca gullies, and a spillway notch cut through the rim —
// and the flanks union with the terrain while the crater cuts into it, so a
// volcano dropped on a mountainside merges with it instead of shaving it flat.
//
// coneExponent: >1 concave-up (1.75 is a stratovolcano, 1.1 a shield).
// craterRadius/craterDepth: fractions of radius/height.
// rimJaggedness, roughness: 0..1.
// breachAngleDeg: rim spillway bearing; pass < 0 to derive one from the seed.
//
// Call it once per volcano. Every vent registered erupts in the next
// titan_simulate_lava call, so their flows meet and divert each other.
TITAN_API void titan_apply_volcano(TitanHandle* handle,
                                   float centerX, float centerY,
                                   float radius, float height,
                                   float coneExponent,
                                   float craterRadius, float craterDepth,
                                   float rimJaggedness, float roughness,
                                   float breachAngleDeg, float breachWidthDeg,
                                   unsigned int seedOffset);

// Runs the cellular lava flow from every registered vent.
//
// Lava is modelled as a fluid with a yield strength that rises as it cools:
// it only moves where its thickness beats a critical value. That is what makes
// it lava and not water — it piles into lobes, freezes levees along its
// chilled margins, and self-channelizes into streams that run to the map edge.
// Chilled lava is folded into the bedrock, so it is real terrain that diverts
// later flows and appears in every export.
//
// viscosity 0..1 (higher = stubbier, thicker flows), sustain non-zero keeps the
// vent erupting for the whole run rather than releasing one pulse.
// Iteration is bounded to the region lava has actually reached, so a short flow
// on a large grid costs what the flow covers, not what the grid holds.
TITAN_API void titan_simulate_lava(TitanHandle* handle, int steps,
                                   float eruptionRate, float viscosity,
                                   float solidifyRate, float coolRate,
                                   float ventRadius, int sustain);

// Drops molten lava, the chilled-lava record, and every registered vent.
TITAN_API void titan_clear_lava(TitanHandle* handle);

// Number of vents registered by titan_apply_volcano since the last configure/
// generate/clear. titan_simulate_lava is a no-op at zero.
TITAN_API int titan_vent_count(TitanHandle* handle);

// Lava layer buffers (size * size floats). NULL until something erupts —
// terrain with no volcanism never allocates them.
TITAN_API float* titan_lava_ptr(TitanHandle* handle);       // molten depth
TITAN_API float* titan_lava_rock_ptr(TitanHandle* handle);  // chilled record
TITAN_API float* titan_lava_heat_ptr(TitanHandle* handle);  // 0..1 temperature
TITAN_API float* titan_lava_glow_ptr(TitanHandle* handle);  // blurred emission

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

// --- v0.5: filters, combiner/import, feature masks, derived maps ----------
// All filters respect the active mask.

// Clamp total height into [minH, maxH].
TITAN_API void titan_apply_clamp(TitanHandle* handle, float minH, float maxH);

// Vertical scale + offset; invert (non-zero) flips the terrain within its
// current height range first.
TITAN_API void titan_apply_transform(TitanHandle* handle, float scaleV,
                                     float offset, int invert);

// Separable box blur (~gaussian), lerped in by strength 0..1.
TITAN_API void titan_apply_blur(TitanHandle* handle, float radius, float strength);

// Unsharp mask: h + (h - blur(h)) * strength.
TITAN_API void titan_apply_sharpen(TitanHandle* handle, float radius, float strength);

// Custom transfer curve over the terrain's own height range. xs/ys are
// `count` control points in [0,1], sorted by x (monotone cubic).
TITAN_API void titan_apply_curve(TitanHandle* handle, const float* xs,
                                 const float* ys, int count);

// Samples that same curve evenly across [0,1] into `out` (`samples` floats).
// No handle: it is pure math on the caller's control points. The curve editors
// draw with this rather than reimplementing the spline, so a preview cannot
// disagree with what the remap will do.
TITAN_API void titan_sample_curve(const float* xs, const float* ys, int count,
                                  float* out, int samples);

// General combiner / heightfield import: resamples a srcSize x srcSize
// float field to the terrain grid, scales samples by heightScale, and
// blends (blendMode as titan_apply_noise; alpha for Mix). Import = clear
// terrain, then apply with blendMode 0 (add).
TITAN_API void titan_apply_heightfield(TitanHandle* handle, const float* data,
                                       int srcSize, float heightScale,
                                       int blendMode, float alpha);

// --- v0.10: real-world elevation import -----------------------------------
//
// Decodes a GeoTIFF/TIFF or an SRTM .hgt into a square, 0..1 normalized field
// ready for titan_apply_heightfield. Returns the field's edge length, or 0 on
// failure (see titan_last_error) — malformed third-party files are expected,
// so every read in the decoder is bounds-checked and reported rather than
// trusted.
//
// Supports classic TIFF, both byte orders, strips and tiles, 8/16/32-bit
// unsigned, signed and float samples, horizontal differencing, and none/LZW/
// Deflate/PackBits compression. BigTIFF is not supported.
// `bytes` is an int, not an int64: the engine caps DEM dimensions at 32768 a
// side, so no readable file approaches 2 GB, and an int64 parameter marshals as
// a BigInt under the WASM build — a trap every JS caller would have to know
// about. Oversized or negative lengths are rejected.
TITAN_API int titan_decode_dem(TitanHandle* handle, const uint8_t* data, int bytes);

// The decoded field: edgeSize*edgeSize floats normalized to 0..1.
TITAN_API float* titan_dem_ptr(TitanHandle* handle);

// The source's real elevation range in its own units (metres, for most DEMs),
// before normalization — what a user needs to set a true vertical scale.
// Either pointer may be NULL.
TITAN_API void titan_dem_elevation_range(TitanHandle* handle, float* outMin, float* outMax);

// Dimensions before the square resample, so a host can say when a rectangular
// DEM was stretched to fit Titan's square terrain.
TITAN_API int titan_dem_source_width(TitanHandle* handle);
TITAN_API int titan_dem_source_height(TitanHandle* handle);

// Derived maps rasterized into scratch (read via titan_scratch_ptr).
TITAN_API void titan_compute_slope_map(TitanHandle* handle);     // rise/run
TITAN_API void titan_compute_curvature_map(TitanHandle* handle); // Laplacian

// Horizon-based ambient occlusion into a persistent field (0 = fully occluded,
// 1 = open sky), read via titan_ao_ptr and carried into the mesh's surface
// attribute. The AO exporter uses the same field, so a viewport and an export
// agree.
//
// This is the engine's most expensive derived map, so it is explicit rather
// than automatic: call it once after the layer stack settles, then rebuild the
// mesh. Without it the mesh's AO channel is 1.0 everywhere and the terrain
// shades as though it sat on an open plain.
TITAN_API void titan_compute_ao(TitanHandle* handle);
TITAN_API float* titan_ao_ptr(TitanHandle* handle); // NULL until computed

// Feature mask into scratch. feature: 0 height (normalized), 1 slope
// (angle/90), 2 curvature (0.5 = flat, >0.5 concave). Soft band
// [rangeLo, rangeHi] with `softness` edge width; invert flips.
TITAN_API void titan_mask_by_feature(TitanHandle* handle, int feature,
                                     float rangeLo, float rangeHi,
                                     float softness, int invert);

// Fractal noise field (0..1) into scratch — for noise-driven masks.
TITAN_API void titan_noise_to_mask(TitanHandle* handle, unsigned int seedOffset,
                                   int noiseType, float scale, int octaves,
                                   float persistence, float lacunarity,
                                   float warpStrength);

// Promote the scratch buffer (clamped 0..1) to the active mask.
// Applies MaskByFeature's soft band to whatever is in scratch. Hosts use this
// after titan_noise_to_mask instead of reimplementing the curve — it used to
// exist in C++, TypeScript and Swift simultaneously.
TITAN_API void titan_band_scratch(TitanHandle* handle, float lo, float hi,
                                  float softness, int invert);

TITAN_API void titan_set_mask_from_scratch(TitanHandle* handle);

// Exporters: each returns the byte size and fills an internal buffer read
// via titan_export_data_ptr. Formats: png16 (16-bit grayscale PNG), r16
// (RAW uint16 LE, normalized), r32 (RAW float32 LE, absolute), exr
// (uncompressed float RGB), obj (Wavefront mesh).
// Exports return the byte length of the buffer at titan_export_data_ptr, or 0
// on failure (check titan_last_error). int64 because a full-resolution OBJ of
// a large grid exceeds 2 GB.
TITAN_API int64_t titan_export_png16(TitanHandle* handle);
TITAN_API int64_t titan_export_r16(TitanHandle* handle);
TITAN_API int64_t titan_export_r32(TitanHandle* handle);
TITAN_API int64_t titan_export_exr(TitanHandle* handle);
TITAN_API int64_t titan_export_obj(TitanHandle* handle);
// normal_png: 8-bit RGB world-space normals (R=+X east, G=+Y south, B=+Z up).
// ao_png: 8-bit grayscale horizon-based ambient occlusion.
TITAN_API int64_t titan_export_normal_png(TitanHandle* handle);
TITAN_API int64_t titan_export_ao_png(TitanHandle* handle);
TITAN_API const uint8_t* titan_export_data_ptr(TitanHandle* handle);

// Raw layer buffers (size * size floats, row-major).
TITAN_API int titan_size(TitanHandle* handle);

// Actual min/max of the current surface, in world height units.
//
// This is NOT [0, heightMultiplier] once a layer stack has run — erosion
// lowers peaks, fluvial carries material off the map, and clamp/plateau/
// transform move both ends. The normalizing exporters (PNG16, R16) stretch to
// exactly this range, so a host must report it for users to set a correct Z
// scale on import. Either pointer may be NULL.
TITAN_API void titan_height_range(TitanHandle* handle, float* outMin, float* outMax);

// The range the normalizing exporters (.r16, .png16) actually stretch to.
//
// Usually identical to titan_height_range. It differs when droplet erosion has
// left a few single-cell sediment towers: normalizing to the true maximum then
// spends most of the 16-bit depth on the gap between the terrain and a handful
// of pixels. Use *this* range to set an importing tool's Z scale — it is what
// the file encodes. Either pointer may be NULL.
TITAN_API void titan_export_height_range(TitanHandle* handle, float* outMin, float* outMax);

// --- Tiled export ---------------------------------------------------------
//
// Slices the already-simulated terrain into tilesPerSide^2 tiles, one call per
// tile, into the usual export buffer (read via titan_export_data_ptr).
//
// Tiles are sliced, not regenerated at their own world origins. World-space
// noise sampling does make independently generated tiles line up exactly on raw
// terrain, but erosion is not local: droplets do not cross a tile boundary,
// drainage terminates at it, and talus has nothing to slump onto beyond it, so
// separately eroded tiles disagree along their shared edge by several percent
// of the relief. Every tile also normalizes against the whole terrain's export
// range, so the set assembles without steps between tiles.
//
// tilesPerSide must divide the grid. `overlap` adds that many samples to each
// tile's far edge, so 1 makes neighbours share a row of vertices (what
// landscape importers usually expect) and the last tile in each axis clamps at
// the grid edge; 0 is an exact partition with nothing duplicated.
// format: 0 = .r16, 1 = 16-bit PNG, 2 = .r32 (absolute float).
//
// Returns bytes written, or 0 on invalid arguments (see titan_last_error).
TITAN_API int64_t titan_export_tile(TitanHandle* handle, int tileX, int tileY,
                                    int tilesPerSide, int overlap, int format);

// Samples per edge a tile would have, or 0 if the split is invalid — use it to
// validate a tile count before offering it.
TITAN_API int titan_tile_resolution(TitanHandle* handle, int tilesPerSide,
                                    int overlap);

// Mass accounting for hydraulic erosion, summed over cells in world height
// units. `exported` is material droplets carried off the map (a real export to
// the sea, not a leak); `created` is the small amount conjured when two
// droplet batches in a round over-draw the same cell against the shared
// snapshot and the sediment floor clips the result. Reset by configure/
// generate/clear. Either pointer may be NULL.
TITAN_API void titan_mass_balance(TitanHandle* handle, double* exported, double* created);
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

// Builds a decimated preview mesh with at most `maxEdgeVertices` vertices per
// edge, so simulation resolution and preview cost are independent. Pass 0 for
// full resolution. Exports always use the full-resolution field regardless.
TITAN_API void titan_build_mesh_lod(TitanHandle* handle, int maxEdgeVertices);
TITAN_API int titan_mesh_stride(TitanHandle* handle);
TITAN_API int titan_mesh_edge_vertices(TitanHandle* handle);

// 8-bit RGBA splatmap PNG: R rock, G height, B flow, A sediment. Uses the same
// channel computation as the mesh vertex colours, so it matches the viewport.
TITAN_API int64_t titan_export_splat_png(TitanHandle* handle);
TITAN_API int titan_mesh_vertex_count(TitanHandle* handle);
TITAN_API int titan_mesh_index_count(TitanHandle* handle);
TITAN_API float* titan_mesh_positions_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_normals_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_colors_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_uvs_ptr(TitanHandle* handle);
TITAN_API float* titan_mesh_snow_ptr(TitanHandle* handle); // vertexCount floats
// vertexCount * 4 floats: molten depth, heat 0..1, chilled-rock depth, glow.
// All zero when the terrain has no volcanism, so hosts can bind it
// unconditionally.
TITAN_API float* titan_mesh_lava_ptr(TitanHandle* handle);
// vertexCount * 4 floats: ambient occlusion, curvature (0.5 = flat, >0.5
// concave), snow depth, water depth. The engine simulates all four; before
// this existed the viewports faked snow from a height threshold and could not
// show lakes at all.
TITAN_API float* titan_mesh_surface_ptr(TitanHandle* handle);
TITAN_API uint32_t* titan_mesh_indices_ptr(TitanHandle* handle);
