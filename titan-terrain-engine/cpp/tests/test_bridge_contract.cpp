// The contract the Unreal plugin depends on.
//
// TitanBridge is the one host CI cannot build: it needs Unreal, which is not
// on a runner. The header-sync and library-symbol jobs prove the plugin is
// compiled against current declarations and that every function it calls
// still exists — but a signature can hold while the meaning underneath it
// changes, and that is the failure this file exists to catch.
//
// So this replays the plugin's generation path exactly as
// TitanTerrainActor.cpp performs it, in the same order with the same
// arguments, and asserts everything the plugin then assumes about what it
// gets back. If the engine ever changes the shape of that data, this fails on
// Linux and Windows and macOS at once, rather than in a customer's editor.
//
// Keep it in step with ATitanTerrainActor::GenerateInternal. It is deliberately
// a transcription, not an abstraction.

#include "TitanCAPI.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>

namespace {

int g_Failures = 0;

void Check(bool condition, const char* label) {
    std::printf("  %s  %s\n", condition ? "PASS" : "FAIL", label);
    if (!condition) ++g_Failures;
}

// The plugin's own defaults, from TitanTerrainActor.h.
struct PluginDefaults {
    int   resolution   = 256;
    int   worldSize    = 256;
    float scale        = 2.5f;
    float height       = 70.0f;
    int   octaves      = 8;
    float exponent     = 1.1f;
    float warp         = 0.6f;
    float unitsPerWorldUnit = 100.0f;

    float cellSize() const {
        return static_cast<float>(worldSize) / static_cast<float>(resolution);
    }
    int   noiseType    = 2;      // ETitanNoiseType::Ridged
    int   riverPasses  = 3;
    float riverStrength = 1.2f;
    int   dropletRounds = 4;
    int   thermalPasses = 12;
    float talus         = 35.0f;
};

} // namespace

int main() {
    std::printf("=== TitanBridge engine contract ===\n\n");
    const PluginDefaults P;

    TitanHandle* engine = titan_create();
    Check(engine != nullptr, "titan_create returns a handle");
    if (!engine) return 1;

    // --- the plugin's exact call sequence ---------------------------------
    titan_configure(engine, P.resolution, P.cellSize(), P.scale, P.height,
                    titan_hash_seed("titan"), P.octaves, 0.5f, 2.0f,
                    P.exponent, P.noiseType, P.warp, 1.0f, 2.0f, 0.0f, 0.0f);
    titan_generate(engine);
    titan_erode_fluvial(engine, P.riverPasses, P.riverStrength);
    titan_erode_hydraulic(engine, P.dropletRounds * 16384, 0);
    titan_erode_thermal(engine, P.thermalPasses, P.talus, 0.5f);
    titan_build_mesh(engine);

    // The plugin never checks this. If any call above had failed it would
    // have built a mesh out of whatever was left behind, so the contract
    // includes "none of that sequence errors on default settings".
    const char* err = titan_last_error();
    Check(err == nullptr || err[0] == '\0',
          "the whole default pipeline runs without setting an error");

    // --- what the plugin reads back ---------------------------------------
    const int vertexCount = titan_mesh_vertex_count(engine);
    const int indexCount  = titan_mesh_index_count(engine);
    const float* positions = titan_mesh_positions_ptr(engine);
    const float* normals   = titan_mesh_normals_ptr(engine);
    const float* colors    = titan_mesh_colors_ptr(engine);
    const float* uvs       = titan_mesh_uvs_ptr(engine);
    const uint32_t* indices = titan_mesh_indices_ptr(engine);

    Check(vertexCount > 0, "mesh has vertices");
    Check(indexCount > 0 && indexCount % 3 == 0, "index count is a whole number of triangles");
    Check(positions && normals && colors && uvs && indices,
          "every mesh accessor returns a buffer");
    if (!positions || !normals || !colors || !uvs || !indices || vertexCount <= 0) {
        titan_destroy(engine);
        return 1;
    }

    // The plugin indexes positions[v*3], normals[v*3], uvs[v*2], colors[v*4].
    // Those strides are the contract: the interleaved 18-float vertex the Metal
    // path uses must never become what these accessors return.
    bool posFinite = true, normUnit = true, colorInRange = true, uvInRange = true;
    float worstNormal = 0.0f;
    for (int v = 0; v < vertexCount; ++v) {
        for (int k = 0; k < 3; ++k) {
            if (!std::isfinite(positions[v * 3 + k])) posFinite = false;
        }
        const float nx = normals[v * 3 + 0];
        const float ny = normals[v * 3 + 1];
        const float nz = normals[v * 3 + 2];
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        worstNormal = std::max(worstNormal, std::fabs(len - 1.0f));
        if (std::fabs(len - 1.0f) > 1e-3f) normUnit = false;
        for (int k = 0; k < 4; ++k) {
            const float c = colors[v * 4 + k];
            if (!(c >= 0.0f && c <= 1.0f)) colorInRange = false;
        }
        for (int k = 0; k < 2; ++k) {
            const float t = uvs[v * 2 + k];
            if (!(t >= 0.0f && t <= 1.0f)) uvInRange = false;
        }
    }
    Check(posFinite, "positions are all finite");
    Check(normUnit, "normals are unit length");
    std::printf("        worst normal deviation %.2e\n", worstNormal);
    Check(colorInRange, "vertex colours are 0..1, so the uint8 cast cannot wrap");
    Check(uvInRange, "UVs are 0..1");

    // Out-of-range indices are the one defect here that crashes the editor
    // rather than looking wrong, because the plugin hands them straight to
    // CreateMeshSection.
    bool indicesInRange = true;
    for (int i = 0; i < indexCount; ++i) {
        if (indices[i] >= static_cast<uint32_t>(vertexCount)) indicesInRange = false;
    }
    Check(indicesInRange, "every index is within the vertex buffer");

    // --- the plugin's axis swap -------------------------------------------
    //
    // Core is Y-up; Unreal is Z-up. The plugin swaps Y and Z and reverses each
    // triangle's winding to match the handedness change. If either half of
    // that is ever done without the other the terrain renders inside out, so
    // check them together: after the swap, a face's geometric normal must
    // still agree with the supplied vertex normal.
    int agree = 0, tested = 0;
    for (int t = 0; t + 2 < indexCount && tested < 2000; t += 3) {
        const uint32_t i0 = indices[t + 0];
        const uint32_t i2 = indices[t + 1];   // the plugin's reversal:
        const uint32_t i1 = indices[t + 2];   // (0, 2, 1)

        auto swapped = [&](uint32_t i, float* out) {
            out[0] = positions[i * 3 + 0];
            out[1] = positions[i * 3 + 2];    // Y <- Z
            out[2] = positions[i * 3 + 1];    // Z <- Y
        };
        float a[3], b[3], c[3];
        swapped(i0, a); swapped(i1, b); swapped(i2, c);

        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
        const float e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float fx = e1[1] * e2[2] - e1[2] * e2[1];
        const float fy = e1[2] * e2[0] - e1[0] * e2[2];
        const float fz = e1[0] * e2[1] - e1[1] * e2[0];
        const float flen = std::sqrt(fx * fx + fy * fy + fz * fz);
        if (flen < 1e-9f) continue;           // degenerate, skip

        // The same swap applied to the supplied normal of the first corner.
        const float vn[3] = {normals[i0 * 3 + 0],
                             normals[i0 * 3 + 2],
                             normals[i0 * 3 + 1]};
        const float dot = (fx * vn[0] + fy * vn[1] + fz * vn[2]) / flen;
        ++tested;
        if (dot > 0.0f) ++agree;
    }
    const double agreement = tested > 0 ? static_cast<double>(agree) / tested : 0.0;
    std::printf("        face/vertex normal agreement %.1f%% over %d triangles\n",
                agreement * 100.0, tested);
    Check(tested > 0, "there were non-degenerate triangles to check");
    Check(agreement > 0.95,
          "after the Y/Z swap and winding reversal, faces still point outward");

    // --- world scale -------------------------------------------------------
    //
    // The engine's extent is size * cellSize, which is World Size by
    // construction, and the plugin multiplies by UnitsPerWorldUnit to get
    // centimetres. So the terrain must span WorldSize * UnitsPerWorldUnit
    // regardless of resolution — that independence is the whole point of the
    // cell-size fix, and asserting the span here is what stops it regressing.
    float minX = positions[0], maxX = positions[0];
    for (int v = 0; v < vertexCount; ++v) {
        minX = std::min(minX, positions[v * 3 + 0]);
        maxX = std::max(maxX, positions[v * 3 + 0]);
    }
    const float span = (maxX - minX) * P.unitsPerWorldUnit;
    const float expected = static_cast<float>(P.worldSize) * P.unitsPerWorldUnit;
    std::printf("        terrain spans %.0f cm, expected about %.0f cm\n", span, expected);
    Check(std::fabs(span - expected) < expected * 0.05f,
          "the mesh spans WorldSize * UnitsPerWorldUnit centimetres");

    // And the independence itself: the same world size at half the sample
    // density must cover the same ground. Under the old hardcoded cell size
    // this failed by a factor of two, silently, which is what made Resolution
    // a world-size control.
    TitanHandle* coarse = titan_create();
    PluginDefaults C;
    C.resolution = P.resolution / 2;
    titan_configure(coarse, C.resolution, C.cellSize(), C.scale, C.height,
                    titan_hash_seed("titan"), C.octaves, 0.5f, 2.0f,
                    C.exponent, C.noiseType, C.warp, 1.0f, 2.0f, 0.0f, 0.0f);
    titan_generate(coarse);
    titan_build_mesh(coarse);
    const int coarseCount = titan_mesh_vertex_count(coarse);
    const float* coarsePos = titan_mesh_positions_ptr(coarse);
    float cMin = coarsePos[0], cMax = coarsePos[0];
    for (int v = 0; v < coarseCount; ++v) {
        cMin = std::min(cMin, coarsePos[v * 3 + 0]);
        cMax = std::max(cMax, coarsePos[v * 3 + 0]);
    }
    const float coarseSpan = (cMax - cMin) * C.unitsPerWorldUnit;
    std::printf("        half resolution spans %.0f cm\n", coarseSpan);
    Check(std::fabs(coarseSpan - span) < span * 0.05f,
          "halving Resolution changes detail, not how much world is covered");
    titan_destroy(coarse);

    titan_destroy(engine);

    std::printf("\n");
    if (g_Failures == 0) {
        std::printf("BRIDGE CONTRACT HOLDS\n");
        return 0;
    }
    std::printf("BRIDGE CONTRACT BROKEN (%d failure%s)\n",
                g_Failures, g_Failures == 1 ? "" : "s");
    return 1;
}
