// libTitanCore benchmark harness.
//
// Phase-0 target: 1024x1024 generate + 200k droplets + thermal + fluvial
// in under 1 second total on an M-series Mac.

#include "TitanCore.h"

#include <chrono>
#include <cstdio>

using Clock = std::chrono::steady_clock;

static double Ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main() {
    Titan::TerrainParams p;
    p.size = 1024;
    p.scale = 3.0f;
    p.heightMultiplier = 120.0f;
    p.seed = 1337;
    p.octaves = 8;
    p.exponent = 1.2f;
    p.noiseType = 2; // ridged
    p.warpStrength = 0.5f;

    Titan::TerrainEngine e;

    std::printf("=== libTitanCore benchmark (%dx%d) ===\n", p.size, p.size);

    auto t0 = Clock::now();
    e.Initialize(p);
    e.GenerateHeightmap();
    auto t1 = Clock::now();
    std::printf("generate (8 octaves, ridged, warped): %8.1f ms\n", Ms(t0, t1));

    e.ApplyHydraulicErosion(200000);
    auto t2 = Clock::now();
    std::printf("hydraulic erosion (200k droplets):    %8.1f ms\n", Ms(t1, t2));

    e.ApplyThermalWeathering(10);
    auto t3 = Clock::now();
    std::printf("thermal weathering (10 passes):       %8.1f ms\n", Ms(t2, t3));

    e.ApplyFluvialErosion(2, {});
    auto t4 = Clock::now();
    std::printf("fluvial erosion (2 passes):           %8.1f ms\n", Ms(t3, t4));

    e.BuildMesh();
    auto t5 = Clock::now();
    std::printf("mesh build (%zu verts):          %8.1f ms\n",
                e.MeshPositions().size() / 3, Ms(t4, t5));

    const double total = Ms(t0, t5);
    std::printf("---------------------------------------------\n");
    std::printf("TOTAL:                                %8.1f ms  (target < 1000 ms)\n", total);
    std::printf("%s\n", total < 1000.0 ? "TARGET MET" : "TARGET MISSED");
    return total < 1000.0 ? 0 : 1;
}
