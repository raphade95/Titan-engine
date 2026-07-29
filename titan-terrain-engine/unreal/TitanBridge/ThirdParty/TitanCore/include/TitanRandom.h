#pragma once

#include <cstdint>

namespace Titan {

// SplitMix64: expands one 64-bit seed into decorrelated stream seeds.
inline uint64_t SplitMix64(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

// PCG32: small, fast, and bit-exact on every platform/compiler we target
// (macOS arm64/x86_64, WASM, Windows/MSVC for the Unreal plugin).
class Pcg32 {
public:
    explicit Pcg32(uint64_t seed, uint64_t sequence = 0xDA3E39CB94B95BDBull) {
        m_State = 0u;
        m_Inc = (sequence << 1u) | 1u;
        NextUInt();
        m_State += seed;
        NextUInt();
    }

    uint32_t NextUInt() {
        uint64_t old = m_State;
        m_State = old * 6364136223846793005ull + m_Inc;
        uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        uint32_t rot = static_cast<uint32_t>(old >> 59u);
        return (xorshifted >> rot) | (xorshifted << ((0u - rot) & 31u));
    }

    // Uniform float in [0, 1). 24 mantissa bits, exactly representable.
    float NextFloat() {
        return static_cast<float>(NextUInt() >> 8) * (1.0f / 16777216.0f);
    }

    // Uniform integer in [0, bound) without modulo bias (Lemire).
    uint32_t NextRange(uint32_t bound) {
        return static_cast<uint32_t>((static_cast<uint64_t>(NextUInt()) * bound) >> 32);
    }

private:
    uint64_t m_State;
    uint64_t m_Inc;
};

// Builds a Pcg32 from a single seed by drawing two decorrelated SplitMix64
// values.
//
// ALWAYS construct engine RNGs through this helper. Writing the equivalent
// inline —
//     Pcg32 rng(SplitMix64(state), SplitMix64(state));
// — is a portability bug, not a style preference: C++17 [expr.call]/8 leaves
// the two parameter initializations *indeterminately sequenced*, so the two
// draws can be handed over in either order depending on the compiler. The two
// orders yield different streams (verified), which would mean the same seed
// produces different terrain on different platforms. Sequencing the draws
// through named locals is the only thing that pins the order down.
inline Pcg32 MakeRng(uint64_t seed) {
    uint64_t state = seed;
    const uint64_t s0 = SplitMix64(state);
    const uint64_t s1 = SplitMix64(state);
    return Pcg32(s0, s1);
}

// FNV-1a over the UTF-8 bytes of a NUL-terminated string.
//
// This is the one canonical seed-string hash for every Titan host: the web
// lab, TitanLab (macOS), and the Unreal plugin all route through
// titan_hash_seed() so that a seed a user types or shares maps to the same
// terrain everywhere. UTF-8 bytes are the unit — not UTF-16 code units, not
// truncated wide characters — so non-ASCII seeds are well defined.
inline uint32_t HashSeedUtf8(const char* utf8) {
    uint32_t h = 0x811c9dc5u;
    if (!utf8) return h;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(utf8); *p; ++p) {
        h ^= static_cast<uint32_t>(*p);
        h *= 0x01000193u;
    }
    return h;
}

} // namespace Titan
