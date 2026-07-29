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
    Dome = 0,           // circular hill (cosine profile)
    Rectangle = 1,
    Ridge = 2,          // elongated ridge line
    Crater = 3,         // rim + bowl
    Gradient = 4,       // linear ramp along the (rotated) u axis
    RadialGradient = 5  // linear cone: 1 at center, 0 at radius
};

// Feature source for procedural mask generation (MaskByFeature).
enum class MaskFeature : int {
    Height = 0,    // normalized by heightMultiplier
    Slope = 1,     // slope angle / 90 deg
    Curvature = 2  // tanh-squashed Laplacian: <0.5 convex, >0.5 concave
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

// A volcanic edifice stamped onto the terrain. Unlike a Dome stamp this is a
// real stratovolcano profile: concave-up flanks (steep at the summit, flaring
// at the base), a summit crater with a jagged rim, radial barranca gullies,
// and a spillway notch cut through the rim where lava will breach.
struct VolcanoParams {
    float centerX = 0.0f;         // cell coordinates
    float centerY = 0.0f;
    float radius = 48.0f;         // base radius in cells
    float height = 60.0f;         // summit above the local ground, world units
    float coneExponent = 1.75f;   // >1 = concave-up flanks (stratovolcano)
    float craterRadius = 0.16f;   // fraction of radius
    float craterDepth = 0.16f;    // fraction of height
    float rimJaggedness = 0.6f;   // 0..1 angular break-up of the crater rim
    float roughness = 0.5f;       // 0..1 surface detail + barranca gullies
    float breachAngleDeg = -1.0f; // rim spillway bearing; < 0 = derived from seed
    float breachWidthDeg = 46.0f; // angular width of the spillway
    uint32_t seedOffset = 0;      // combined with the terrain seed
};

// Cellular lava flow. Molten lava is a fluid layer with a *yield strength*:
// it only moves where its thickness exceeds a critical value that grows as it
// cools. That single rule is what separates lava from water — it forms thick
// lobes, builds levees at its chilled margins, and self-channelizes into
// streams that outlive the eruption, which is exactly how real flows behave.
// Defaults are calibrated against how far a flow front actually advances: a
// front moves roughly one cell per step down a flank, so a lava that cools in
// tens of steps never leaves its own crater however much of it erupts.
struct LavaParams {
    float eruptionRate = 1.5f;    // world-height units added per vent per step
    int steps = 600;              // relaxation steps (flow length)
    float viscosity = 0.35f;      // 0..1 -> yield slope; higher = stubbier lobes
    float solidifyRate = 0.02f;   // fraction of chilled lava turning to rock/step
    float coolRate = 0.0015f;     // heat lost per step
    float ventRadius = 2.5f;      // cells over which lava is injected
    int sustain = 1;              // 1 = erupt every step, 0 = one initial pulse
};

// A registered eruption source, created by ApplyVolcano and consumed by
// SimulateLava. Keeping vents in the engine is what makes multiple volcanoes
// work: every volcano layer in the stack adds one, and a single lava pass
// erupts all of them into the same flow field, so their streams interact.
struct Vent {
    float x = 0.0f;
    float y = 0.0f;
    float craterRadius = 8.0f;    // cells
    uint32_t seed = 0;
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
// batch order, so results are bit-identical regardless of thread count.
// ApplyHydraulicErosion rounds every request up to whole rounds, so results
// are also identical however the caller chunks the work.
constexpr int kDropletBatch = 2048;
constexpr int kBatchesPerRound = 8;
constexpr int kDropletsPerRound = kDropletBatch * kBatchesPerRound;

class TerrainEngine {
public:
    TerrainEngine() = default;

    // Clamps every field into a safe, usable range. Initialize applies this,
    // so no caller can reach an allocation or loop bound it did not intend.
    static TerrainParams Sanitize(const TerrainParams& params);

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

    // --- Volcanism ----------------------------------------------------------

    // Stamps a volcanic edifice onto the terrain and registers its vent.
    // The cone rises out of whatever is already there (union on the flanks)
    // while the crater cuts down through it, so it reads as part of the
    // landscape rather than a dome pasted on top. Respects the active mask.
    void ApplyVolcano(const VolcanoParams& p);

    // Erupts every registered vent and relaxes the flow for p.steps.
    // Molten depth lands in the lava field, chilled lava accumulates in the
    // lava-rock field (which is terrain: it diverts later flows), and the
    // heat field drives the viewport's glow. No-op with no vents.
    void SimulateLava(const LavaParams& p);

    // Drops molten lava, chilled lava rock, heat, and every registered vent.
    void ClearLava();

    int VentCount() const { return static_cast<int>(m_Vents.size()); }

    // Iteration counts are rounded up to whole rounds (kDropletsPerRound), so
    // any chunking composes — see the batching contract above.
    void ApplyHydraulicErosion(int iterations, const HydraulicParams& p = {});
    void ApplyThermalWeathering(int passes, const ThermalParams& p = {});
    void ApplyFluvialErosion(int iterations, const FluvialParams& p = {});

    // Shaping modifiers (deterministic, run any time after generation).
    void ApplyTerrace(float interval, float strength, float sharpness);
    void ApplyPlateau(float plateauHeight, float softness);

    // --- v0.5 filters. All respect the active mask and re-split 80/20. ----

    // Clamp total height into [minH, maxH].
    void ApplyClamp(float minH, float maxH);

    // Vertical scale + offset; invert flips the terrain within its own
    // current height range before scaling.
    void ApplyTransform(float scaleV, float offset, bool invert);

    // Separable box blur (two passes ~ gaussian), lerped in by strength.
    void ApplyBlur(float radius, float strength);

    // Unsharp mask: h + (h - blur(h)) * strength.
    void ApplySharpen(float radius, float strength);

    // Custom transfer curve: monotone piecewise-cubic through (xs, ys)
    // control points in [0,1], applied over the terrain's own height range.
    void ApplyCurve(const float* xs, const float* ys, int count);

    // General combiner / heightfield import: bilinearly resamples a
    // srcSize x srcSize field to the terrain grid, scales samples by
    // heightScale, and blends with the given BlendMode (alpha for Mix).
    void ApplyHeightfield(const float* data, int srcSize, float heightScale,
                          int blendMode, float alpha);

    // Derived maps rasterized into the scratch buffer (ScratchMask()).
    void ComputeSlopeMap();     // slope as rise/run
    void ComputeCurvatureMap(); // Laplacian: >0 concave (valley), <0 convex

    // Horizon-based ambient occlusion into a persistent field, 0..1 (1 = open
    // sky). Shared by the AO exporter and the mesh's aux attribute, so what a
    // viewport shades with and what a user exports are the same numbers.
    //
    // Deliberately explicit rather than folded into BuildMesh: this is the most
    // expensive derived map in the engine, and BuildMesh runs once per pipeline
    // chunk. Hosts call it once, after the stack settles.
    void ComputeAOField();
    const std::vector<float>& AOField() const { return m_AO; }

    // Mask generators — write a 0..1 field into scratch. Promote to the
    // active mask with SetMaskFromScratch().
    void MaskByFeature(int feature, float rangeLo, float rangeHi,
                       float softness, bool invert);
    void NoiseToScratch(const NoiseLayerParams& p);
    // Applies the same soft band MaskByFeature uses to whatever is already in
    // scratch (a noise field, a derived map). Exists so hosts never have to
    // reimplement the curve: it previously lived in C++, TypeScript and Swift
    // at once, three copies of one formula that had to agree forever.
    void BandScratch(float lo, float hi, float softness, bool invert);

    void SetMaskFromScratch();

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
    //
    // `stride` decimates the preview: every Nth cell per axis. This decouples
    // simulation resolution from preview cost, which is what allows grids
    // beyond 512 — a full-resolution mesh of a 2048 terrain is 4.2M vertices,
    // and copying that out of a 32-bit WASM heap is not viable. Exports are
    // unaffected; they always read the full-resolution field.
    void BuildMesh(int stride = 1);

    // Stride the current mesh was built with (1 = full resolution).
    int MeshStride() const { return m_MeshStride; }

    // Vertices per edge of the current mesh.
    int MeshEdgeVertices() const {
        return m_MeshStride > 0 ? (m_Params.size - 1) / m_MeshStride + 1 : 0;
    }

    // Exporters fill the internal byte buffer (see ExportData()/ExportSize()).
    // All heightmap exporters normalize to [min,max] except R32/EXR which
    // store absolute heights.
    size_t ExportPNG16();
    size_t ExportR16();
    size_t ExportR32();
    size_t ExportEXR();
    size_t ExportOBJ();
    size_t ExportNormalPNG(); // 8-bit RGB world-space normals (+X east, +Y south, +Z up)
    size_t ExportAOPNG();     // 8-bit grayscale horizon-based ambient occlusion

    // 8-bit RGBA splatmap. Channels are computed by the *same* code path that
    // fills the mesh's vertex colours, so the exported masks match what the
    // viewport shades. R rock (slope), G normalized height, B flow/wetness,
    // A sediment depth.
    size_t ExportSplatPNG();
    const uint8_t* ExportData() const { return m_ExportBuffer.data(); }
    size_t ExportSize() const { return m_ExportBuffer.size(); }

    int Size() const { return m_Params.size; }
    const TerrainParams& Params() const { return m_Params; }

    // Actual min/max of the current surface. This is NOT [0, heightMultiplier]
    // once a stack has run: erosion lowers peaks, fluvial carries material off
    // the map, and clamp/plateau/transform move both ends. Exporters that
    // normalize (PNG16, R16) stretch to exactly this range, so importing tools
    // need it to reconstruct real-world elevations.
    void HeightRange(float& outMin, float& outMax) const {
        CollectHeightRange(outMin, outMax);
    }

    // Mass accounting for hydraulic erosion, in world-height units summed over
    // cells. `MassExported` is material droplets legitimately carried off the
    // map; `MassCreated` is the (small) amount conjured by the sediment floor
    // when two batches in a round over-draw the same cell. Together they let a
    // test state conservation exactly rather than assert it loosely. Reset by
    // Initialize/GenerateHeightmap/ClearTerrain.
    double MassExported() const { return m_MassExported; }
    double MassCreated() const { return m_MassCreated; }

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

    // Lava fields. Empty until a volcano or lava layer runs — a terrain with
    // no volcanism pays nothing for them, which matters at large grid sizes
    // where each full-size float map is hundreds of megabytes.
    std::vector<float>& LavaMap() { return m_Lava; }         // molten depth
    std::vector<float>& LavaRockMap() { return m_LavaRock; } // chilled, is terrain
    std::vector<float>& LavaHeatMap() { return m_LavaHeat; } // 0..1, drives glow
    std::vector<float>& LavaGlowMap() { return m_LavaGlow; } // blurred emission
    const std::vector<float>& LavaMap() const { return m_Lava; }
    const std::vector<float>& LavaRockMap() const { return m_LavaRock; }
    const std::vector<float>& LavaHeatMap() const { return m_LavaHeat; }
    const std::vector<float>& LavaGlowMap() const { return m_LavaGlow; }

    // Filled by BuildMesh().
    const std::vector<float>& MeshPositions() const { return m_MeshPositions; }
    const std::vector<float>& MeshNormals() const { return m_MeshNormals; }
    const std::vector<float>& MeshColors() const { return m_MeshColors; }
    const std::vector<float>& MeshUVs() const { return m_MeshUVs; }
    const std::vector<float>& MeshSnow() const { return m_MeshSnow; }
    // Per-vertex surface attribute, 4 floats: ambient occlusion, curvature
    // (0.5 = flat, >0.5 concave), snow depth, water depth.
    //
    // These exist because the engine was already computing all four and the
    // viewports were using none of them — snow was faked in the shader from a
    // height threshold that had nothing to do with the simulated snowpack, and
    // lakes were invisible in 3D entirely.
    const std::vector<float>& MeshSurface() const { return m_MeshSurface; }
    // Per-vertex lava attribute, 4 floats: molten depth, heat 0..1, chilled
    // rock depth, and a blurred glow term the shaders use to let flows light
    // the rock around them. All zero when there is no volcanism.
    const std::vector<float>& MeshLava() const { return m_MeshLava; }
    const std::vector<uint32_t>& MeshIndices() const { return m_MeshIndices; }

    // Per-cell splat channels — the single definition shared by BuildMesh's
    // vertex colours and the splatmap exporter. The web lab used to compute
    // its own version in JavaScript with a different rock formula
    // (slope * 2.5 against the mesh's (slope - 0.36) / 0.48), so the exported
    // splatmap did not match the terrain the user was looking at.
    void SplatAt(int x, int y, float& rock, float& height, float& flow, float& sediment) const;

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
                         std::vector<float>& flowDelta,
                         double& exported) const;

    void CollectHeightRange(float& minH, float& maxH) const;

    // Allocates the lava fields on first volcanic use (they stay empty for
    // terrain that never erupts). Idempotent.
    void EnsureLavaFields();

    // Blurs molten heat into m_LavaGlow so nearby rock can be lit by it.
    void UpdateLavaGlow();

    // Caches the surface's real height range for SplatAt's height channel.
    // Called by BuildMesh and ExportSplatPNG — the two entry points that walk
    // every cell anyway — so the mesh and the exported splatmap always measure
    // against the same span.
    void RefreshSplatRange();

    TerrainParams m_Params;
    std::vector<float> m_Bedrock;
    std::vector<float> m_Sediment;
    std::vector<float> m_Flow;
    std::vector<float> m_Snow;
    std::vector<float> m_Water;
    std::vector<float> m_Lava;          // molten depth, empty when unused
    std::vector<float> m_LavaRock;      // chilled lava — counts as terrain
    std::vector<float> m_LavaHeat;      // 0..1 temperature of the molten layer
    std::vector<float> m_LavaGlow;      // blurred emission for the viewport
    std::vector<Vent> m_Vents;          // eruption sources, one per volcano
    std::vector<float> m_AO;            // horizon AO, empty until computed

    // Height range the splat's normalized-height channel is measured against,
    // refreshed by BuildMesh and the splatmap exporter. See RefreshSplatRange.
    float m_SplatLo = 0.0f;
    float m_SplatHi = 0.0f;
    bool m_SplatRangeValid = false;
    std::vector<float> m_Mask;          // empty = no mask (1 everywhere)
    std::vector<float> m_Scratch;       // stamp/selector rasterization target
    std::vector<float> m_Precipitation; // empty when unset
    uint64_t m_DropletCursor = 0;       // global droplet index across chunked calls
    double m_MassExported = 0.0;        // sediment droplets carried off the map
    double m_MassCreated = 0.0;         // material conjured by the sediment floor

    std::vector<float> m_MeshPositions;
    std::vector<float> m_MeshNormals;
    std::vector<float> m_MeshColors;
    std::vector<float> m_MeshUVs;
    std::vector<float> m_MeshSnow;      // per-vertex snow depth
    std::vector<float> m_MeshLava;      // per-vertex vec4: molten, heat, rock, glow
    std::vector<float> m_MeshSurface;   // per-vertex vec4: ao, curvature, snow, water
    int m_MeshStride = 1;
    std::vector<uint32_t> m_MeshIndices;

    std::vector<uint8_t> m_ExportBuffer;
};

} // namespace Titan
