// libTitanCore cross-platform golden checks.
//
// test_core.cpp checks that a build agrees with *itself*. This binary checks
// that every build agrees with the same checked-in numbers, which is the only
// way to actually test the product's headline promise about seeds.
//
// It runs in every CI matrix job (macOS / Linux / Windows) and, via
// tools/wasm_golden.mjs, against the Emscripten build the web lab ships.
//
// -------------------------------------------------------------------------
// WHY THERE ARE TWO LAYERS
// -------------------------------------------------------------------------
//
// LAYER 1 - integer determinism. STRICT, IDENTICAL EVERYWHERE.
//   Seed hashing, SplitMix64, PCG32 streams, and the simplex permutation
//   tables are pure integer arithmetic, so they are bit-exact on any
//   conforming compiler. One set of constants, asserted on every platform.
//   This is the layer that catches the argument-evaluation-order class of
//   bug: if the two SplitMix64 draws seeding a Pcg32 are ever handed over in
//   the wrong order, every permutation table here changes.
//
// LAYER 2 - terrain fields. STRICT PER BUILD, TOLERANCED ACROSS BUILDS.
//   IEEE-754 pins down + - * / and sqrt, and the build pins fast-math and FP
//   contraction off, so those are portable. What is NOT portable is libm:
//   pow, sin, tan, exp, log, and tanh are permitted to differ in the last ulp
//   between implementations, and they measurably do. Verified on one machine,
//   one compiler, one set of flags, differing only in target architecture:
//
//       powf   arm64 50a532a50c8a1dd5   x86_64 55d2dfcb015fdf8b
//       sinf   arm64 dfff5ed94a50b528   x86_64 0defb67c746feb4f
//       expf   arm64 f76cabea1318260e   x86_64 52c0c6c7e8852f29
//       logf   arm64 0cc764e52d88ecda   x86_64 2a25e618673cb4b6
//       tanhf  arm64 b22978cf7ad67505   x86_64 4febbba92071c63c
//       tanf   arm64 86d26b83f1581423   x86_64 ad842b5c786c26ea
//       (sqrtf and atanf agree; sqrt is IEEE-mandated)
//
//   Height shaping calls pow() on every cell, so a bit-exact hash simply
//   cannot hold across architectures today. Pretending otherwise would mean
//   a permanently red or permanently ignored CI job. So layer 2 asserts:
//
//     2a. EXACT hashes, on the reference configuration only (arm64 + Apple
//         libm). Catches any unintended engine change, to the bit.
//     2b. A TOLERANCED statistical fingerprint, asserted on EVERY platform.
//         This is the honest, testable form of "same seed, same terrain
//         everywhere": the surfaces must agree far more tightly than any
//         real defect would allow, while absorbing last-ulp libm noise.
//
//   Both determinism bugs this harness was built for blow through the
//   tolerance by orders of magnitude, so 2b has real teeth. See
//   docs/determinism.md for what it would take to make 2a hold everywhere.
//
// To regenerate after an intentional engine change, on the reference config:
//     ./build/titan_golden --print
// Never regenerate to turn a red build green without understanding why it
// went red: producing that signal is the entire point of this binary.

#include "TitanCAPI.h"
#include "TitanCore.h"
#include "TitanNoise.h"
#include "TitanRandom.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_Failures = 0;
bool g_Print = false;

// The configuration whose exact hashes are checked in. Everywhere else, the
// exact hashes are reported but only the toleranced fingerprints are enforced.
#if defined(__aarch64__) || defined(_M_ARM64)
#if defined(__APPLE__)
constexpr bool kReferenceConfig = true;
#else
constexpr bool kReferenceConfig = false;
#endif
#else
constexpr bool kReferenceConfig = false;
#endif

// --- FNV-1a 64 ------------------------------------------------------------

// The real FNV-1a 64 parameters. Spelled in hex because the decimal form of
// the offset basis is 20 digits and a dropped digit is invisible on review —
// which is exactly what happened here first time round, and it only surfaced
// because the WASM runner used the correct value and disagreed.
constexpr uint64_t kFnvOffset = 0xcbf29ce484222325ull; // 14695981039346656037
constexpr uint64_t kFnvPrime = 0x00000100000001b3ull; // 1099511628211

uint64_t HashBytes(const void* data, size_t n, uint64_t h = kFnvOffset) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) {
        h ^= static_cast<uint64_t>(p[i]);
        h *= kFnvPrime;
    }
    return h;
}

// Hash values rather than their memory, so results are endian-independent.
uint64_t HashU32(uint32_t v, uint64_t h = kFnvOffset) {
    for (int i = 0; i < 4; ++i) {
        h ^= static_cast<uint64_t>((v >> (8 * i)) & 0xFFu);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t HashU64(uint64_t v, uint64_t h = kFnvOffset) {
    for (int i = 0; i < 8; ++i) {
        h ^= (v >> (8 * i)) & 0xFFull;
        h *= kFnvPrime;
    }
    return h;
}

// Hashes a float field by bit pattern. Negative zero folds to positive zero:
// the two are the same terrain, and which one an expression yields for an
// empty cell is not worth pinning down.
uint64_t HashFloats(const std::vector<float>& v, uint64_t h = kFnvOffset) {
    for (float f : v) {
        if (f == 0.0f) f = 0.0f;
        uint32_t bits;
        std::memcpy(&bits, &f, 4);
        h = HashU32(bits, h);
    }
    return h;
}

std::vector<float> Surface(const Titan::TerrainEngine& e) {
    const auto& b = e.BedrockMap();
    const auto& s = e.SedimentMap();
    std::vector<float> out(b.size());
    for (size_t i = 0; i < b.size(); ++i) out[i] = b[i] + s[i];
    return out;
}

// --- toleranced fingerprint ------------------------------------------------

// A coarse, order-independent summary of a height field: global extent plus a
// 4x4 grid of tile means. Accumulated in double so the summary itself does not
// introduce ordering noise. Sensitive enough that reordering an erosion pass
// or reseeding the RNG moves it by percent, loose enough that a last-ulp libm
// difference does not.
constexpr int kTileGrid = 4;
constexpr int kFingerprintSize = 3 + kTileGrid * kTileGrid;

struct Fingerprint {
    double v[kFingerprintSize];
};

Fingerprint Summarize(const std::vector<float>& field, int size) {
    Fingerprint fp{};
    double sum = 0.0;
    double lo = 1e300, hi = -1e300;
    double tileSum[kTileGrid * kTileGrid] = {};
    long tileCount[kTileGrid * kTileGrid] = {};

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const double h = field[static_cast<size_t>(y) * size + x];
            sum += h;
            if (h < lo) lo = h;
            if (h > hi) hi = h;
            const int tx = std::min(kTileGrid - 1, x * kTileGrid / size);
            const int ty = std::min(kTileGrid - 1, y * kTileGrid / size);
            tileSum[ty * kTileGrid + tx] += h;
            ++tileCount[ty * kTileGrid + tx];
        }
    }

    const double n = static_cast<double>(size) * size;
    fp.v[0] = sum / n;
    fp.v[1] = lo;
    fp.v[2] = hi;
    for (int i = 0; i < kTileGrid * kTileGrid; ++i) {
        fp.v[3 + i] = tileCount[i] ? tileSum[i] / static_cast<double>(tileCount[i]) : 0.0;
    }
    return fp;
}

// --- check plumbing --------------------------------------------------------

// Tolerances are per recipe, because the recipes amplify libm noise by wildly
// different amounts and a single loose global bound would blind the tight
// ones. Measured arm64 vs x86_64 drift, same compiler and flags:
//
//   base noise / filters / water             2e-10 .. 1e-8   -> bound 1e-6
//   fluvial over a clamped plateau           2.8e-7          -> bound 1e-5
//   droplet erosion                          6.8e-5          -> bound 5e-3
//
// Droplet erosion is chaotic: a last-ulp height difference can flip which
// neighbour a droplet descends into, and its whole path diverges from there.
// Measured per-cell consequence on the pipeline recipe: mean |dh| 6.9e-4 on a
// 25-unit range (0.003%), worst cell 1.2%, 2 of 16384 cells over 1%. Visually
// identical, not bit-identical. Every bound below still leaves 50x or more of
// margin against a real ordering or seeding defect, which move these numbers
// by 1e-2 and up (verified by mutation testing - see tools/mutation_test.sh).
struct Recipe {
    const char* name;
    uint64_t exactHash;                 // reference config only
    double fingerprint[kFingerprintSize]; // every platform, toleranced
    double tolerance;
    const char* libm;                   // libm exposure, for triage
};

std::vector<std::pair<std::string, uint64_t>> g_ExactActual;
std::vector<std::pair<std::string, Fingerprint>> g_FpActual;

void Check(bool ok, const char* label) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++g_Failures;
}

void CheckExact(const char* name, uint64_t expected, uint64_t actual, const char* libm = "") {
    g_ExactActual.push_back({name, actual});
    if (g_Print) return;
    if (actual == expected) {
        std::printf("  PASS  %-36s %016llx\n", name, (unsigned long long)actual);
    } else {
        std::printf("  FAIL  %-36s got %016llx want %016llx%s%s\n", name,
                    (unsigned long long)actual, (unsigned long long)expected,
                    libm[0] ? "  | libm: " : "", libm);
        ++g_Failures;
    }
}

double RelDiff(double a, double b) {
    const double scale = std::max(1.0, std::max(std::fabs(a), std::fabs(b)));
    return std::fabs(a - b) / scale;
}

void CheckRecipe(const Recipe& r, const std::vector<float>& field, int size) {
    const Fingerprint actual = Summarize(field, size);
    g_FpActual.push_back({r.name, actual});
    const uint64_t hash = HashFloats(field);
    g_ExactActual.push_back({r.name, hash});
    if (g_Print) return;

    // 2a - exact, reference configuration only.
    if (kReferenceConfig) {
        if (hash != r.exactHash) {
            std::printf("  FAIL  %-36s exact %016llx want %016llx  | libm: %s\n",
                        r.name, (unsigned long long)hash,
                        (unsigned long long)r.exactHash, r.libm);
            ++g_Failures;
        }
    }

    // 2b - toleranced, every platform.
    double worst = 0.0;
    int worstIdx = 0;
    for (int i = 0; i < kFingerprintSize; ++i) {
        const double d = RelDiff(actual.v[i], r.fingerprint[i]);
        if (d > worst) { worst = d; worstIdx = i; }
    }

    if (worst <= r.tolerance) {
        std::printf("  PASS  %-36s drift %.2e (bound %.0e)%s\n", r.name, worst, r.tolerance,
                    (kReferenceConfig && hash == r.exactHash) ? "  (exact)" : "");
    } else {
        std::printf("  FAIL  %-36s drift %.2e at [%d] got %.9g want %.9g  | libm: %s\n",
                    r.name, worst, worstIdx, actual.v[worstIdx],
                    r.fingerprint[worstIdx], r.libm);
        ++g_Failures;
    }
}

// ===========================================================================
// LAYER 1 - integer determinism. No floating point in this section.
// ===========================================================================

constexpr uint64_t kG_SplitMix64 = 0x23021dea6d1c7a8bull;
constexpr uint64_t kG_Pcg32      = 0xb5f8ba07f5dddcabull;
constexpr uint64_t kG_PermTables = 0x5e55aaabc4819b4full;
constexpr uint64_t kG_SeedHash   = 0xd1547926f7a4cf8bull;

void TestRngPrimitives() {
    std::printf("Layer 1 - integer determinism (strict on every platform):\n");

    {
        uint64_t state = 0xDEADBEEFCAFEF00Dull;
        uint64_t h = kFnvOffset;
        for (int i = 0; i < 64; ++i) h = HashU64(Titan::SplitMix64(state), h);
        CheckExact("splitmix64 stream", kG_SplitMix64, h);
    }

    // The value that moves if the two SplitMix64 draws are ever reordered.
    {
        uint64_t h = kFnvOffset;
        for (uint64_t seed : {0ull, 1ull, 42ull, 0xFFFFFFFFull, 0x123456789ABCDEFull}) {
            Titan::Pcg32 rng = Titan::MakeRng(seed);
            for (int i = 0; i < 32; ++i) h = HashU32(rng.NextUInt(), h);
        }
        CheckExact("pcg32 stream (via MakeRng)", kG_Pcg32, h);
    }

    // Simplex permutation tables - the direct fingerprint of noise seeding.
    {
        uint64_t h = kFnvOffset;
        for (uint64_t seed : {1ull, 42ull, 1234ull, 31337ull, 0xA5A5A5A5ull}) {
            Titan::Pcg32 rng = Titan::MakeRng(seed);
            unsigned char table[256];
            for (int i = 0; i < 256; ++i) table[i] = static_cast<unsigned char>(i);
            for (int i = 255; i > 0; --i) {
                const uint32_t j = rng.NextRange(static_cast<uint32_t>(i + 1));
                const unsigned char t = table[i];
                table[i] = table[j];
                table[j] = t;
            }
            h = HashBytes(table, sizeof(table), h);
        }
        CheckExact("simplex permutation tables", kG_PermTables, h);
    }

    // Canonical seed hashing, including the non-ASCII cases where the three
    // hosts used to disagree.
    {
        uint64_t h = kFnvOffset;
        const char* seeds[] = {
            "", "a", "titan", "alpine-7", "0123456789",
            "caf\xC3\xA9",                    // cafe with acute  (2 UTF-8 bytes)
            "\xE5\xB1\xB1",                   // CJK mountain     (3 bytes)
            "\xF0\x9F\x8F\x94",               // snow-capped peak (4 bytes)
            ("se\xC3\xB1or-monta\xC3\xB1" "a"), // senor-montana (split: \xB1 would eat the 'a')
        };
        for (const char* s : seeds) h = HashU32(titan_hash_seed(s), h);
        h = HashU32(titan_hash_seed(nullptr), h);
        CheckExact("titan_hash_seed (utf-8)", kG_SeedHash, h);
    }
}

// Direct regression guard for the argument-evaluation-order bug. Depends on
// no golden constant: it asserts the invariant itself.
void TestRngSeedingOrder() {
    std::printf("\nRNG seeding order (regression guard):\n");

    const uint64_t s = 0x0123456789ABCDEFull;
    uint64_t probe = s;
    const uint64_t first = Titan::SplitMix64(probe);
    const uint64_t second = Titan::SplitMix64(probe);

    Titan::Pcg32 viaHelper = Titan::MakeRng(s);
    Titan::Pcg32 inOrder(first, second);
    Titan::Pcg32 swapped(second, first);

    bool matchesInOrder = true;
    bool differsFromSwapped = false;
    for (int i = 0; i < 16; ++i) {
        const uint32_t a = viaHelper.NextUInt();
        if (a != inOrder.NextUInt()) matchesInOrder = false;
        if (a != swapped.NextUInt()) differsFromSwapped = true;
    }

    Check(matchesInOrder, "MakeRng consumes SplitMix64 draws in call order");
    // If this ever fails the two orders have become equivalent, meaning the
    // golden hashes above would no longer detect a reordering.
    Check(differsFromSwapped, "swapping the draws diverges (guard has teeth)");
}

// ===========================================================================
// LAYER 2 - terrain fields.
// ===========================================================================

const Recipe kR_BaseNoise = {
    "base fields, all 9 noise types", 0xb531c30df5d96f91ull,
    {13.585997628329, 0, 40, 9.3924506820686577, 8.8813087526165564, 9.2919320526675122, 9.5115531000644999, 13.28759078819227, 13.695171784952334, 14.620704473468424, 14.117111501545878, 12.831712769667824, 13.76690240032945, 13.515626387702973, 12.598132491732636, 17.765463745827219, 18.456401440464287, 17.946802032195404, 17.697097649768086},
    1e-6, "pow"};

// Regenerated when droplets started reading their own batch's deposits (see
// the LiveHeight note in Erosion.cpp). The old numbers described a surface
// carrying single-cell sediment towers up to 113 units above their
// neighbours; these describe the same recipe without them. Everything else in
// this file is unchanged, which is the point — the move is confined to the
// erosion pass that was actually fixed.
const Recipe kR_Pipeline = {
    "full erosion pipeline", 0xc4da8e0c33dddec3ull,
    {16.78528819634812, 5.371610164642334, 29.31580924987793, 13.687583587132394, 17.607313165906817, 14.676699017174542, 15.537511678878218, 21.21087472140789, 20.666806922294199, 15.134610102977604, 24.426344033330679, 17.728109618648887, 13.926159515045583, 17.13333244714886, 17.680605706758797, 12.611658820416778, 16.244706056080759, 16.422357715666294, 13.869938032701612},
    5e-3, "pow, sin, tan, log"};

const Recipe kR_FluvialTie = {
    "fluvial after clamp (tie-break)", 0x4d6bc8134fb08965ull,
    {17.180619833190576, 9.9999990463256836, 25.000001907348633, 18.723480331711471, 21.366655733203515, 15.909776960965246, 15.038464709417894, 15.48375357594341, 16.457827897975221, 13.371921137906611, 17.347575774649158, 19.190429148729891, 21.671478321775794, 18.513719606678933, 19.251466677524149, 14.122096260078251, 16.963378053857014, 14.507967442972586, 16.969925697660074},
    1e-5, "pow, sin, log"};

const Recipe kR_Filters = {
    "filter chain", 0x301ae8cbe13b3a16ull,
    {11.894929656016757, 2.3171355724334717, 33.679275512695312, 9.0028116072062403, 11.505017440300435, 14.548295707441866, 5.6955845607444644, 12.529248918872327, 10.002049994422123, 13.783907823031768, 15.651796608231962, 12.614242303650826, 17.374536151997745, 14.147499116603285, 6.4510355666279793, 11.080469822278246, 9.522328689461574, 10.520345617784187, 15.88970456761308},
    1e-6, "pow, tanh"};

const Recipe kR_Water = {
    "priority-flood water fill", 0x015bf5b81ee51308ull,
    {0.58856794657185674, 0, 19.769737243652344, 0.32211565272882581, 0.1039994889870286, 0.49122905870899558, 0.25342641584575176, 0.11162542086094618, 2.0237070433795452, 0.46033473964780569, 0.8546695951372385, 0.053969397209584713, 3.0979574103839695, 0.72697646217420697, 0.074587401002645493, 0.15081779379397631, 0.036177824251353741, 0.1548683475703001, 0.50062509346753359},
    1e-6, "pow"};

Titan::TerrainParams GoldenParams(uint32_t seed, int noiseType, int size = 128) {
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

void TestTerrainGoldens() {
    std::printf("\nLayer 2 - terrain fields (exact on reference config, "
                "toleranced everywhere):\n");

    // Every noise type, base generation only, tiled into one field so the
    // fingerprint covers all nine.
    {
        const int size = 128;
        std::vector<float> combined;
        for (int type = 0; type <= 8; ++type) {
            Titan::TerrainEngine e;
            e.Initialize(GoldenParams(2024u + type, type, size));
            e.GenerateHeightmap();
            const std::vector<float> s = Surface(e);
            combined.insert(combined.end(), s.begin(), s.end());
        }
        // Nine 128x128 tiles concatenated is exactly 384x384, so the
        // fingerprint's tile grid covers all nine in one pass.
        static_assert(9 * 128 * 128 == 384 * 384, "tile packing assumption");
        CheckRecipe(kR_BaseNoise, combined, size * 3);
    }

    {
        Titan::TerrainEngine e;
        e.Initialize(GoldenParams(1234, 1));
        e.GenerateHeightmap();
        e.ApplyHydraulicErosion(Titan::kDropletsPerRound);
        e.ApplyThermalWeathering(10);
        e.ApplyFluvialErosion(2, {});
        CheckRecipe(kR_Pipeline, Surface(e), 128);
    }

    // Fluvial over a clamped (deliberately very flat) surface. A single Clamp
    // leaves thousands of cells at exactly one height, so this is the recipe
    // that goes wrong if the sort/heap tie-breaks are dropped.
    {
        Titan::TerrainEngine e;
        e.Initialize(GoldenParams(4242, 1, 256));
        e.GenerateHeightmap();
        e.ApplyClamp(10.0f, 25.0f);
        e.ApplyFluvialErosion(3, {});
        CheckRecipe(kR_FluvialTie, Surface(e), 256);
    }

    {
        Titan::TerrainEngine e;
        e.Initialize(GoldenParams(77, 2));
        e.GenerateHeightmap();
        e.ApplyBlur(3.0f, 0.6f);
        e.ApplySharpen(2.0f, 0.5f);
        const float xs[5] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
        const float ys[5] = {0.0f, 0.10f, 0.5f, 0.90f, 1.0f};
        e.ApplyCurve(xs, ys, 5);
        e.ApplyTerrace(8.0f, 0.7f, 2.0f);
        e.ApplyPlateau(30.0f, 6.0f);
        e.ApplyTransform(1.1f, 2.0f, false);
        CheckRecipe(kR_Filters, Surface(e), 128);
    }

    // Priority-flood water fill - flat lakes are all ties by construction.
    {
        Titan::TerrainEngine e;
        e.Initialize(GoldenParams(555, 1));
        e.GenerateHeightmap();
        e.Carve(64.0f, 64.0f, 20.0f, 15.0f);
        e.ComputeWater();
        CheckRecipe(kR_Water, e.WaterMap(), 128);
    }
}

void PrintConstants() {
    std::printf("// --- exact hashes (reference config only) ---\n");
    for (const auto& kv : g_ExactActual) {
        std::printf("//   %-36s 0x%016llxull\n", kv.first.c_str(),
                    (unsigned long long)kv.second);
    }
    std::printf("\n// --- fingerprints (all platforms, toleranced) ---\n");
    for (const auto& kv : g_FpActual) {
        std::printf("// %s\n    {", kv.first.c_str());
        for (int i = 0; i < kFingerprintSize; ++i) {
            std::printf("%s%.17g", i ? ", " : "", kv.second.v[i]);
        }
        std::printf("},\n");
    }
}

} // namespace

int main(int argc, char** argv) {
    bool requireReference = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--print") == 0) g_Print = true;
        // CI passes this on the job that is supposed to BE the reference
        // configuration. Without it, exact-hash enforcement could silently
        // switch itself off — if the macOS runner ever changed architecture,
        // every job would quietly fall back to the toleranced path and no
        // build anywhere would be checking hashes to the bit.
        if (std::strcmp(argv[i], "--require-reference") == 0) requireReference = true;
    }

    if (requireReference && !kReferenceConfig) {
        std::printf("ERROR: --require-reference given, but this build is not the "
                    "reference configuration (expected Apple arm64).\n"
                    "Exact golden hashes would not be enforced anywhere. Either "
                    "run this job on macOS arm64 or move the reference\n"
                    "configuration in tests/test_golden.cpp and regenerate.\n");
        return 2;
    }

    std::printf("=== libTitanCore golden checks (%s, api v%d) ===\n",
                titan_version(), titan_api_version());
    std::printf("reference configuration: %s\n\n",
                kReferenceConfig ? "yes - exact hashes enforced"
                                 : "no - fingerprints enforced, exact hashes reported");

    TestRngPrimitives();
    TestRngSeedingOrder();
    TestTerrainGoldens();

    if (g_Print) {
        std::printf("\n");
        PrintConstants();
        return 0;
    }

    std::printf("\n%s (%d failure%s)\n",
                g_Failures == 0 ? "GOLDEN CHECKS PASSED" : "GOLDEN CHECKS FAILED",
                g_Failures, g_Failures == 1 ? "" : "s");
    return g_Failures == 0 ? 0 : 1;
}
