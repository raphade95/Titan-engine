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

void TestChunkedErosionEquivalence() {
    std::printf("Chunked erosion equivalence:\n");
    // One 49152-droplet call vs three 16384-droplet calls (whole rounds)
    // must be bit-identical — this is what lets the UI stream progress.
    Titan::TerrainEngine a, b;
    a.Initialize(MakeParams(31337, 1));
    a.GenerateHeightmap();
    b.Initialize(MakeParams(31337, 1));
    b.GenerateHeightmap();

    a.ApplyHydraulicErosion(Titan::kDropletsPerRound * 3);
    for (int i = 0; i < 3; ++i) b.ApplyHydraulicErosion(Titan::kDropletsPerRound);

    Check(MapsIdentical(a, b), "3 chunked calls == 1 large call (bit-identical)");
}

void TestSpawnModes() {
    std::printf("Spawn modes:\n");
    Titan::TerrainEngine u, alt;
    u.Initialize(MakeParams(808, 1));
    u.GenerateHeightmap();
    alt.Initialize(MakeParams(808, 1));
    alt.GenerateHeightmap();

    Titan::HydraulicParams pu;
    pu.spawnMode = 0;
    Titan::HydraulicParams pa;
    pa.spawnMode = 1;
    u.ApplyHydraulicErosion(20000, pu);
    alt.ApplyHydraulicErosion(20000, pa);

    Check(MeanAbsDifference(u, alt) > 1e-4, "altitude-weighted spawning changes the outcome");
    Check(AllFinite(alt.BedrockMap()) && AllFinite(alt.SedimentMap()),
          "altitude-weighted erosion stays finite");
}

void TestModifiers() {
    std::printf("Shaping modifiers:\n");
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(99, 1));
    e.GenerateHeightmap();

    float maxBefore = 0.0f;
    for (size_t i = 0; i < e.BedrockMap().size(); ++i) {
        maxBefore = std::max(maxBefore, e.BedrockMap()[i] + e.SedimentMap()[i]);
    }

    e.ApplyPlateau(maxBefore * 0.6f, 2.0f);
    float maxAfter = 0.0f;
    for (size_t i = 0; i < e.BedrockMap().size(); ++i) {
        maxAfter = std::max(maxAfter, e.BedrockMap()[i] + e.SedimentMap()[i]);
    }
    Check(maxAfter <= maxBefore * 0.6f + 1e-3f, "plateau caps peaks at the target height");

    Titan::TerrainEngine t, ref;
    t.Initialize(MakeParams(99, 1));
    t.GenerateHeightmap();
    ref.Initialize(MakeParams(99, 1));
    ref.GenerateHeightmap();
    t.ApplyTerrace(8.0f, 1.0f, 2.0f);
    Check(MeanAbsDifference(t, ref) > 1e-3, "terrace visibly reshapes the terrain");
    Check(AllFinite(t.BedrockMap()) && AllFinite(t.SedimentMap()), "terrace stays finite");
}

void TestExporters() {
    std::printf("Exporters:\n");
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(7, 1, 64));
    e.GenerateHeightmap();

    const size_t pngSize = e.ExportPNG16();
    const uint8_t* d = e.ExportData();
    Check(pngSize > 8 && d[0] == 137 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G',
          "PNG16 has a valid signature");

    const size_t r16 = e.ExportR16();
    Check(r16 == 64u * 64u * 2u, "R16 is exactly 2 bytes per cell");

    const size_t r32 = e.ExportR32();
    Check(r32 == 64u * 64u * 4u, "R32 is exactly 4 bytes per cell");

    const size_t exr = e.ExportEXR();
    const uint8_t* x = e.ExportData();
    Check(exr > 8 && x[0] == 0x76 && x[1] == 0x2f && x[2] == 0x31 && x[3] == 0x01,
          "EXR has a valid magic number");

    const size_t obj = e.ExportOBJ();
    const char* o = reinterpret_cast<const char*>(e.ExportData());
    Check(obj > 2 && o[0] == '#', "OBJ starts with header comment");
}

void TestMasks() {
    std::printf("Mask system:\n");
    const int size = 128;
    Titan::TerrainEngine a, b;
    a.Initialize(MakeParams(555, 2, size));
    a.GenerateHeightmap();
    b.Initialize(MakeParams(555, 2, size));
    b.GenerateHeightmap();

    // Mask out the left half on b; erode both identically.
    std::vector<float> mask(static_cast<size_t>(size) * size, 0.0f);
    for (int y = 0; y < size; ++y) {
        for (int x = size / 2; x < size; ++x) mask[static_cast<size_t>(y) * size + x] = 1.0f;
    }
    b.SetMask(mask.data(), size);
    a.ApplyThermalWeathering(10);
    b.ApplyThermalWeathering(10);

    // Left half of b should be untouched; right half should have changed.
    double leftDiff = 0.0, rightDiff = 0.0;
    Titan::TerrainEngine ref;
    ref.Initialize(MakeParams(555, 2, size));
    ref.GenerateHeightmap();
    for (int y = 2; y < size - 2; ++y) {
        for (int x = 2; x < size - 2; ++x) {
            const double d = std::fabs(
                (b.GetBedrock(x, y) + b.GetSediment(x, y)) -
                (ref.GetBedrock(x, y) + ref.GetSediment(x, y)));
            if (x < size / 2 - 2) leftDiff += d;
            if (x >= size / 2 + 2) rightDiff += d;
        }
    }
    Check(leftDiff < 1e-4, "masked-out region is untouched by erosion");
    Check(rightDiff > 0.01, "unmasked region erodes normally");
    b.ClearMask();
}

void TestNoiseStacking() {
    std::printf("Noise stacking:\n");
    const int size = 96;
    Titan::TerrainEngine e;
    Titan::TerrainParams tp = MakeParams(777, 0, size);
    e.Initialize(tp);
    e.ClearTerrain();

    Titan::NoiseLayerParams n1;
    n1.noiseType = 1;
    n1.amplitude = 30.0f;
    n1.scale = 2.0f;
    e.ApplyNoise(n1);

    double mass1 = TotalMass(e);
    Check(mass1 > 0.0, "first noise layer adds material");

    Titan::NoiseLayerParams n2;
    n2.noiseType = 4; // voronoi cells
    n2.seedOffset = 7;
    n2.amplitude = 20.0f;
    n2.blendMode = 3; // max
    e.ApplyNoise(n2);
    Check(TotalMass(e) >= mass1 - 1e-3, "max-blend never lowers terrain");
    Check(AllFinite(e.BedrockMap()), "stacked noise stays finite");

    Titan::TerrainEngine v;
    v.Initialize(tp);
    v.ClearTerrain();
    Titan::NoiseLayerParams nv;
    nv.noiseType = 5; // voronoi ridge
    nv.amplitude = 25.0f;
    v.ApplyNoise(nv);
    Check(TotalMass(v) > 0.0, "voronoi-ridge noise generates terrain");
}

void TestStamps() {
    std::printf("Shape stamps:\n");
    const int size = 128;
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(1, 0, size));
    e.ClearTerrain();

    Titan::StampParams dome;
    dome.shape = 0;
    dome.centerX = 64.0f;
    dome.centerY = 64.0f;
    dome.sizeX = 30.0f;
    dome.sizeY = 30.0f;
    dome.height = 25.0f;
    dome.falloff = 0.5f;
    dome.op = 0;
    e.ApplyStamp(dome);

    Check(std::fabs(e.GetHeight(64, 64) - 25.0f) < 0.5f, "dome peak reaches stamp height");
    Check(e.GetHeight(5, 5) == 0.0f, "terrain outside the stamp is untouched");

    Titan::StampParams rect;
    rect.shape = 1;
    rect.centerX = 64.0f;
    rect.centerY = 64.0f;
    rect.sizeX = 40.0f;
    rect.sizeY = 20.0f;
    rect.rotationDeg = 30.0f;
    rect.height = 10.0f;
    rect.op = 2; // flatten toward 10
    e.ApplyStamp(rect);
    Check(std::fabs(e.GetHeight(64, 64) - 10.0f) < 0.5f, "flatten pulls center to target height");

    e.StampToScratch(dome);
    float maxField = 0.0f;
    for (float v : e.ScratchMask()) maxField = std::max(maxField, v);
    Check(maxField > 0.99f, "stamp-to-mask rasterizes a full-strength field");
}

void TestSnowAndWater() {
    std::printf("Snow & water:\n");
    Titan::TerrainEngine e;
    e.Initialize(MakeParams(4242, 2, 128));
    e.GenerateHeightmap();

    Titan::SnowParams sp;
    e.ApplySnow(sp);

    float maxSnow = 0.0f;
    double lowSnow = 0.0;
    const int size = 128;
    const float heightRef = e.Params().heightMultiplier;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float snow = e.SnowMap()[static_cast<size_t>(y) * size + x];
            maxSnow = std::max(maxSnow, snow);
            if ((e.GetBedrock(x, y) + e.GetSediment(x, y)) / heightRef < 0.2f) {
                lowSnow += snow;
            }
        }
    }
    Check(maxSnow > 0.5f, "snow accumulates on high terrain");
    Check(lowSnow < 1.0, "lowlands stay essentially snow-free");
    Check(AllFinite(e.SnowMap()), "snow field finite");

    e.BuildMesh();
    Check(e.MeshSnow().size() == static_cast<size_t>(size) * size, "mesh snow channel present");

    e.ComputeWater();
    Check(AllFinite(e.WaterMap()), "water field finite");
    float maxWater = 0.0f;
    for (float w : e.WaterMap()) maxWater = std::max(maxWater, w);
    Check(maxWater >= 0.0f, "water depths non-negative");

    // A crater must fill with water.
    Titan::TerrainEngine c;
    c.Initialize(MakeParams(1, 0, 96));
    c.ClearTerrain();
    Titan::StampParams rim;
    rim.shape = 0;
    rim.centerX = 48;
    rim.centerY = 48;
    rim.sizeX = 30;
    rim.sizeY = 30;
    rim.height = 20;
    rim.op = 0;
    c.ApplyStamp(rim);
    Titan::StampParams bowl;
    bowl.shape = 0;
    bowl.centerX = 48;
    bowl.centerY = 48;
    bowl.sizeX = 15;
    bowl.sizeY = 15;
    bowl.height = 15;
    bowl.op = 1; // dig the middle out
    c.ApplyStamp(bowl);
    c.ComputeWater();
    float craterWater = 0.0f;
    for (float w : c.WaterMap()) craterWater = std::max(craterWater, w);
    Check(craterWater > 1.0f, "carved basin fills with water");
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
    TestChunkedErosionEquivalence();
    TestSpawnModes();
    TestModifiers();
    TestExporters();
    TestMasks();
    TestNoiseStacking();
    TestStamps();
    TestSnowAndWater();
    TestChunkSeams();

    std::printf("\n%s (%d failure%s)\n",
                g_Failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED",
                g_Failures, g_Failures == 1 ? "" : "s");
    return g_Failures == 0 ? 0 : 1;
}
