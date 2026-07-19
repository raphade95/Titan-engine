// libTitanCore test harness.
//
// These are the guarantees the product depends on:
//   1. Determinism — same seed, same params => bit-identical terrain,
//      including after every erosion pass.
//   2. Seed sensitivity — different seeds => genuinely different terrain.
//   3. Mass conservation — thermal weathering neither creates nor destroys
//      material.
//   4. Numerical health — no NaN/Inf anywhere after heavy simulation.
//   5. Chunk seamlessness — two tiles sharing an edge sample identical noise.

#include "TitanCore.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

namespace {

int g_Failures = 0;

void Check(bool condition, const char* label) {
    if (condition) {
        std::printf("  PASS  %s\n", label);
    } else {
        std::printf("  FAIL  %s\n", label);
        ++g_Failures;
    }
}

Titan::TerrainParams MakeParams(uint32_t seed, int noiseType, int size = 128) {
    Titan::TerrainParams p;
    p.size = size;
    p.scale = 2.0f;
    p.heightMultiplier = 40.0f;
    p.seed = seed;
    p.octaves = 6;
    p.persistence = 0.5f;
    p.lacunarity = 2.0f;
    p.exponent = 1.2f;
    p.noiseType = noiseType;
    p.warpStrength = 0.6f;
    return p;
}

double TotalMass(const Titan::TerrainEngine& e) {
    // Accumulate in double so the check isn't at the mercy of float order.
    double sum = 0.0;
    for (float v : e.BedrockMap()) sum += v;
    for (float v : e.SedimentMap()) sum += v;
    return sum;
}

bool AllFinite(const std::vector<float>& v) {
    for (float x : v) {
        if (!std::isfinite(x)) return false;
    }
    return true;
}

bool MapsIdentical(const Titan::TerrainEngine& a, const Titan::TerrainEngine& b) {
    return a.BedrockMap() == b.BedrockMap() && a.SedimentMap() == b.SedimentMap();
}

double MeanAbsDifference(const Titan::TerrainEngine& a, const Titan::TerrainEngine& b) {
    // Compare total surface height (bedrock + sediment) — erosion largely
    // works by redistributing the sediment layer.
    double sum = 0.0;
    const auto& ba = a.BedrockMap();
    const auto& bb = b.BedrockMap();
    const auto& sa = a.SedimentMap();
    const auto& sb = b.SedimentMap();
    for (size_t i = 0; i < ba.size(); ++i) {
        sum += std::fabs((ba[i] + sa[i]) - (bb[i] + sb[i]));
    }
    return sum / static_cast<double>(ba.size());
}

void RunFullPipeline(Titan::TerrainEngine& e, uint32_t seed) {
    e.Initialize(MakeParams(seed, 1));
    e.GenerateHeightmap();
    e.ApplyHydraulicErosion(20000);
    e.ApplyThermalWeathering(10);
    e.ApplyFluvialErosion(2, {});
}

void TestDeterminism() {
    std::printf("Determinism:\n");
    Titan::TerrainEngine a, b;
    RunFullPipeline(a, 1234);
    RunFullPipeline(b, 1234);
    Check(MapsIdentical(a, b), "same seed reproduces identical terrain after full pipeline");
}

void TestSeedSensitivity() {
    std::printf("Seed sensitivity:\n");
    Titan::TerrainEngine a, b;
    a.Initialize(MakeParams(1, 1));
    a.GenerateHeightmap();
    b.Initialize(MakeParams(2, 1));
    b.GenerateHeightmap();
    const double diff = MeanAbsDifference(a, b);
    std::printf("        mean |dh| between seeds 1 and 2: %.4f\n", diff);
    Check(diff > 0.5, "different seeds produce substantially different terrain");

    // Ridged and billow must also react to the seed.
    Titan::TerrainEngine r1, r2;
    r1.Initialize(MakeParams(1, 2));
    r1.GenerateHeightmap();
    r2.Initialize(MakeParams(2, 2));
    r2.GenerateHeightmap();
    Check(MeanAbsDifference(r1, r2) > 0.5, "ridged noise responds to seed");
}

void TestFlatStart() {
    std::printf("Flat start:\n");
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(42, 0)); // noiseType none
    e.GenerateHeightmap();
    double mass = TotalMass(e);
    Check(mass == 0.0, "noiseType=none yields a perfectly flat, empty heightfield");
}

void TestThermalMassConservation() {
    std::printf("Thermal weathering mass conservation:\n");
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(77, 2)); // ridged: plenty of steep slopes
    e.GenerateHeightmap();

    const double before = TotalMass(e);
    e.ApplyThermalWeathering(25);
    const double after = TotalMass(e);

    const double relError = std::fabs(after - before) / std::max(1.0, std::fabs(before));
    std::printf("        mass before %.6f, after %.6f, rel error %.3e\n", before, after, relError);
    Check(relError < 1e-5, "thermal weathering conserves mass (rel error < 1e-5)");

    // And it must actually do something.
    Titan::TerrainEngine ref;
    ref.Initialize(MakeParams(77, 2));
    ref.GenerateHeightmap();
    Check(MeanAbsDifference(e, ref) > 1e-4, "thermal weathering changes the terrain");
}

void TestNumericalHealth() {
    std::printf("Numerical health after heavy simulation:\n");
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(99, 2, 128));
    e.GenerateHeightmap();
    e.ApplyHydraulicErosion(60000);
    e.ApplyThermalWeathering(20);
    e.ApplyFluvialErosion(3, {});

    Check(AllFinite(e.BedrockMap()), "bedrock has no NaN/Inf");
    Check(AllFinite(e.SedimentMap()), "sediment has no NaN/Inf");
    Check(AllFinite(e.FlowMap()), "flow map has no NaN/Inf");

    bool nonNegative = true;
    for (float s : e.SedimentMap()) {
        if (s < 0.0f) { nonNegative = false; break; }
    }
    Check(nonNegative, "sediment never goes negative");

    e.BuildMesh();
    Check(AllFinite(e.MeshPositions()), "mesh positions finite");
    Check(AllFinite(e.MeshNormals()), "mesh normals finite");
    Check(e.MeshIndices().size() == static_cast<size_t>(127 * 127 * 6),
          "index count matches grid size");
}

void TestErosionEffectiveness() {
    std::printf("Erosion effectiveness:\n");
    Titan::TerrainEngine e, ref;
    e.Initialize(MakeParams(2024, 1));
    e.GenerateHeightmap();
    ref.Initialize(MakeParams(2024, 1));
    ref.GenerateHeightmap();

    e.ApplyHydraulicErosion(50000);
    Check(MeanAbsDifference(e, ref) > 0.01, "hydraulic erosion visibly modifies terrain");

    float maxFlow = 0.0f;
    e.ApplyFluvialErosion(2, {});
    for (float f : e.FlowMap()) maxFlow = std::max(maxFlow, f);
    Check(maxFlow > 0.5f, "fluvial pass produces strong flow channels");
}

void TestChunkSeams() {
    std::printf("Chunk seamlessness (world-space noise):\n");
    // Tile A centered at origin; tile B shifted exactly one tile east.
    // Column overlap: A's x = size-1 world coord equals B's x = -1... instead
    // easier: generate two overlapping tiles and compare the shared column.
    const int size = 64;

    Titan::TerrainParams pa = MakeParams(555, 1, size);
    pa.warpStrength = 0.0f;
    Titan::TerrainParams pb = pa;
    // Shift tile B so its column 0 lands exactly on tile A's column 32.
    pb.originX = 32.0f * pa.cellSize;

    Titan::TerrainEngine a, b;
    a.Initialize(pa);
    a.GenerateHeightmap();
    b.Initialize(pb);
    b.GenerateHeightmap();

    double maxDiff = 0.0;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size - 32; ++x) {
            maxDiff = std::max(maxDiff, static_cast<double>(std::fabs(
                a.GetHeight(x + 32, y) - b.GetHeight(x, y))));
        }
    }
    std::printf("        max overlap difference: %.6e\n", maxDiff);
    Check(maxDiff == 0.0, "overlapping tiles sample identical world-space noise");
}

} // namespace

int main() {
    std::printf("=== libTitanCore test harness ===\n\n");

    TestDeterminism();
    TestSeedSensitivity();
    TestFlatStart();
    TestThermalMassConservation();
    TestNumericalHealth();
    TestErosionEffectiveness();
    TestChunkSeams();

    std::printf("\n%s (%d failure%s)\n",
                g_Failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_Failures, g_Failures == 1 ? "" : "s");
    return g_Failures == 0 ? 0 : 1;
}
