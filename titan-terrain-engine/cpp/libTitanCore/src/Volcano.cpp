// Volcanism: the volcanic edifice stamp and the cellular lava flow model.
//
// Both are deterministic. The edifice is a pure function of its parameters and
// the terrain seed; the flow is a fixed-step relaxation with no randomness at
// all, so a given stack always produces the same streams down the same flanks.

#include "TitanCore.h"
#include "TitanNoise.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace Titan {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// Volume of a single-burst eruption, as a multiple of the eruption rate.
// Roughly what a continuous vent would deliver over 150 steps.
constexpr float kBurstVolume = 150.0f;

float SmoothStep01(float t) {
    t = std::clamp(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// Shortest angular distance between two bearings, in radians, in [0, pi].
float AngleDelta(float a, float b) {
    float d = std::fabs(a - b);
    while (d > 2.0f * kPi) d -= 2.0f * kPi;
    return d > kPi ? 2.0f * kPi - d : d;
}

// Angular noise sampled *around a circle* rather than along a line, so it is
// seamless at the +/-pi wrap. A 1D noise indexed by the raw angle would leave a
// visible seam down one flank of every volcano.
struct RingNoise {
    const SimplexNoise& n;
    float Sample(float angle, float freq) const {
        return n.Sample(std::cos(angle) * freq, std::sin(angle) * freq);
    }
    // Ridged variant: sharp crests, used for the jagged rim spikes.
    float Ridged(float angle, float freq) const {
        return 1.0f - std::fabs(Sample(angle, freq));
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Field allocation
// ---------------------------------------------------------------------------

void TerrainEngine::EnsureLavaFields() {
    const size_t count = static_cast<size_t>(m_Params.size) * m_Params.size;
    if (m_Lava.size() != count) m_Lava.assign(count, 0.0f);
    if (m_LavaRock.size() != count) m_LavaRock.assign(count, 0.0f);
    if (m_LavaHeat.size() != count) m_LavaHeat.assign(count, 0.0f);
    if (m_LavaGlow.size() != count) m_LavaGlow.assign(count, 0.0f);
}

void TerrainEngine::ClearLava() {
    m_Lava.clear();
    m_LavaRock.clear();
    m_LavaHeat.clear();
    m_LavaGlow.clear();
    m_Vents.clear();
}

// ---------------------------------------------------------------------------
// The edifice
// ---------------------------------------------------------------------------
//
// The radial profile is built in normalized units: r = distance / radius, and
// the returned value is a fraction of `height` above the local ground.
//
//   flanks (r >= rim)   (1 - r)^coneExponent, carved by radial barrancas
//   rim    (r ~= rim)   a jagged crest ring, notched open at the spillway
//   crater (r <  rim)   an inner wall falling to a flat floor
//
// coneExponent > 1 is what makes it read as a volcano rather than a hill: the
// profile is steepest at the summit and flares out toward the base, which is
// the concave-up signature of a stratovolcano. A Dome stamp is the opposite
// shape (flat on top, steep at the edge) and no amount of parameter tuning
// turns one into the other.
void TerrainEngine::ApplyVolcano(const VolcanoParams& in) {
    const int size = m_Params.size;
    if (size < 3) return;
    const size_t count = static_cast<size_t>(size) * size;

    // Clamp into ranges that cannot produce a degenerate or inverted profile.
    VolcanoParams p = in;
    p.radius = std::clamp(std::isfinite(p.radius) ? p.radius : 48.0f, 2.0f, 1e5f);
    p.height = std::isfinite(p.height) ? p.height : 60.0f;
    p.coneExponent = std::clamp(std::isfinite(p.coneExponent) ? p.coneExponent : 1.75f, 0.5f, 6.0f);
    p.craterRadius = std::clamp(std::isfinite(p.craterRadius) ? p.craterRadius : 0.16f, 0.02f, 0.6f);
    p.craterDepth = std::clamp(std::isfinite(p.craterDepth) ? p.craterDepth : 0.16f, 0.0f, 0.95f);
    p.rimJaggedness = std::clamp(std::isfinite(p.rimJaggedness) ? p.rimJaggedness : 0.6f, 0.0f, 1.0f);
    p.roughness = std::clamp(std::isfinite(p.roughness) ? p.roughness : 0.5f, 0.0f, 1.0f);
    p.breachWidthDeg = std::clamp(std::isfinite(p.breachWidthDeg) ? p.breachWidthDeg : 46.0f, 5.0f, 180.0f);
    if (!std::isfinite(p.centerX)) p.centerX = size * 0.5f;
    if (!std::isfinite(p.centerY)) p.centerY = size * 0.5f;

    // Offset into a distinct region of the seed space so a volcano's rim noise
    // never correlates with a noise layer that happens to share a variant.
    const uint64_t seed = static_cast<uint64_t>(m_Params.seed)
        + (static_cast<uint64_t>(p.seedOffset) << 32 | p.seedOffset)
        + 0x5CA1DEB0A7ULL;

    const SimplexNoise ring(seed);
    const RingNoise rn{ring};

    // Surface detail: two octaves of world-space noise for ash-and-lava
    // layering, so the flanks are never a mathematically clean surface.
    FractalParams dp;
    dp.type = NoiseType::Standard;
    dp.octaves = 5;
    dp.persistence = 0.5f;
    dp.lacunarity = 2.1f;
    const FractalNoise detail(seed ^ 0x9E3779B97F4A7C15ULL, dp);

    // Spillway bearing. A negative input means "pick one from the seed", which
    // keeps volcanoes from all breaching the same way on a multi-vent map.
    const float breach = (p.breachAngleDeg >= 0.0f)
        ? p.breachAngleDeg * kPi / 180.0f
        : rn.Sample(0.37f, 1.7f) * kPi;
    const float breachHalfWidth = p.breachWidthDeg * 0.5f * kPi / 180.0f;

    // --- Local ground level -------------------------------------------------
    // The cone is built on top of whatever is already here, so it has to know
    // what "here" is. Averaging the ring at the volcano's own base radius is
    // what makes it merge into a slope instead of floating above a valley or
    // burying itself in a ridge.
    const int cx = static_cast<int>(std::floor(p.centerX));
    const int cy = static_cast<int>(std::floor(p.centerY));
    double ringSum = 0.0;
    int ringCount = 0;
    for (int k = 0; k < 64; ++k) {
        const float a = (static_cast<float>(k) / 64.0f) * 2.0f * kPi;
        const int sx = static_cast<int>(std::lround(p.centerX + std::cos(a) * p.radius));
        const int sy = static_cast<int>(std::lround(p.centerY + std::sin(a) * p.radius));
        if (!InBounds(sx, sy)) continue;
        ringSum += m_Bedrock[Index(sx, sy)] + m_Sediment[Index(sx, sy)];
        ++ringCount;
    }
    const float baseElev = ringCount > 0
        ? static_cast<float>(ringSum / ringCount)
        : (InBounds(cx, cy) ? m_Bedrock[Index(cx, cy)] + m_Sediment[Index(cx, cy)] : 0.0f);

    // --- Rasterize ----------------------------------------------------------
    // Only the footprint is touched; a volcano is a local feature and iterating
    // the whole grid for it would make placing several of them needlessly slow.
    const float reach = p.radius * 1.08f;
    const int x0 = std::max(0, static_cast<int>(std::floor(p.centerX - reach)));
    const int x1 = std::min(size - 1, static_cast<int>(std::ceil(p.centerX + reach)));
    const int y0 = std::max(0, static_cast<int>(std::floor(p.centerY - reach)));
    const int y1 = std::min(size - 1, static_cast<int>(std::ceil(p.centerY + reach)));

    std::vector<float> total(count);
    for (size_t i = 0; i < count; ++i) total[i] = m_Bedrock[i] + m_Sediment[i];

    const float invRadius = 1.0f / p.radius;

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            const float dx = static_cast<float>(x) - p.centerX;
            const float dy = static_cast<float>(y) - p.centerY;
            const float dist = std::sqrt(dx * dx + dy * dy);
            const float r = dist * invRadius;
            if (r >= 1.0f) continue;

            const float angle = std::atan2(dy, dx);

            // Crater rim radius wobbles with angle — an ellipse-free, organic
            // crown rather than a machined circle.
            const float rimWobble = rn.Sample(angle, 2.6f);
            const float rimR = std::clamp(
                p.craterRadius * (1.0f + p.rimJaggedness * rimWobble * 0.45f),
                0.015f, 0.75f);

            // Barranca gullies: radial erosion channels down the flanks. They
            // are strongest mid-flank and fade out at both the summit and the
            // skirt, which is where they die on a real cone.
            const float gully = rn.Ridged(angle, 9.0f);
            const float gullyBand = SmoothStep01((r - rimR) / 0.35f)
                                  * (1.0f - SmoothStep01((r - 0.72f) / 0.28f));
            const float flankMod = 1.0f - p.roughness * 0.22f * gully * gullyBand;

            auto coneAt = [&](float rr) {
                return std::pow(std::clamp(1.0f - rr, 0.0f, 1.0f), p.coneExponent);
            };

            const float rimTop = coneAt(rimR) * flankMod;
            const float floorH = rimTop - p.craterDepth;

            float profile;
            if (r >= rimR) {
                profile = coneAt(r) * flankMod;
            } else {
                // Inner crater wall: steep just under the rim, flattening onto
                // the floor. The exponent is what gives the funnel its shape.
                const float t = r / rimR;
                profile = floorH + (rimTop - floorH) * std::pow(t, 2.4f);
            }

            // Jagged rim crest: a narrow spiked ring sitting on the rim.
            // High-frequency ridged angular noise makes the spikes irregular,
            // so the crater edge is broken and toothed the way a collapsed
            // rim is, not a smooth lip.
            if (p.rimJaggedness > 0.0f) {
                const float band = (r - rimR) / (0.085f + 0.05f * p.rimJaggedness);
                const float ringMask = std::exp(-(band * band));
                const float teeth = rn.Ridged(angle, 17.0f) * 0.6f
                                  + rn.Ridged(angle, 31.0f) * 0.4f;
                profile += ringMask * p.rimJaggedness * 0.11f * teeth;
            }

            // Spillway: a breached rim, cut down to the crater floor and
            // continuing as a channel down the flank — the horseshoe crater of
            // a volcano that has already erupted through one side.
            //
            // This is a *ceiling* on the profile across the sector, not a bump
            // subtracted from it. Subtracting a fixed amount cannot guarantee
            // an open channel: the rim radius wobbles with angle and carries a
            // jagged crest on top, so the notch landed a few units above the
            // crater floor and left a sill. Lava then filled the crater, froze
            // in place, and never flowed at all.
            const float da = AngleDelta(angle, breach);
            if (da < breachHalfWidth && r < 0.62f) {
                const float across = 1.0f - SmoothStep01(da / breachHalfWidth);
                // Level with the crater floor at the rim, then falling away so
                // the channel keeps draining instead of ponding on the flank.
                const float sill = floorH - std::max(0.0f, r - rimR) * 0.55f;
                // Fades out where it would undercut the natural flank anyway.
                const float taper = 1.0f - SmoothStep01((r - 0.42f) / 0.20f);
                const float capped = std::min(profile, sill);
                profile += (capped - profile) * across * taper;
            }

            // Surface detail in world space, faded out inside the crater so
            // the floor stays a floor.
            if (p.roughness > 0.0f) {
                const float wx = (static_cast<float>(x) - size * 0.5f) * m_Params.cellSize;
                const float wy = (static_cast<float>(y) - size * 0.5f) * m_Params.cellSize;
                const float freq = 3.5f / std::max(1.0f, p.radius * m_Params.cellSize);
                const float d = detail.Sample(wx * freq, wy * freq) - 0.5f;
                profile += d * p.roughness * 0.085f * SmoothStep01(r / (rimR + 0.05f));
            }

            const float target = baseElev + profile * p.height;

            // Skirt: the outermost slice of the footprint fades the edit out,
            // so the cone joins the terrain instead of ending on a step.
            const float skirt = 1.0f - SmoothStep01((r - 0.86f) / 0.14f);
            const size_t i = static_cast<size_t>(Index(x, y));
            const float existing = total[i];

            // Flanks build up (union), the crater cuts down (assign). Union on
            // the flanks is what lets a volcano be dropped onto an existing
            // mountainside without shaving it off.
            const float combined = (r < rimR)
                ? target
                : std::max(existing, target);

            const float w = skirt * MaskAt(static_cast<int>(i));
            total[i] = existing + (combined - existing) * w;
        }
    }

    ResplitHeight(total);

    // Register the vent. Multiple volcano layers stack up here and a single
    // lava pass erupts all of them into one shared flow field.
    Vent v;
    v.x = p.centerX;
    v.y = p.centerY;
    v.craterRadius = p.craterRadius * p.radius;
    v.seed = p.seedOffset;
    m_Vents.push_back(v);
}

// ---------------------------------------------------------------------------
// Lava flow
// ---------------------------------------------------------------------------

void TerrainEngine::SimulateLava(const LavaParams& in) {
    const int size = m_Params.size;
    if (size < 3 || m_Vents.empty()) return;

    LavaParams p = in;
    p.eruptionRate = std::clamp(std::isfinite(p.eruptionRate) ? p.eruptionRate : 0.6f, 0.0f, 100.0f);
    p.steps = std::clamp(p.steps, 0, 20000);
    p.viscosity = std::clamp(std::isfinite(p.viscosity) ? p.viscosity : 0.45f, 0.0f, 1.0f);
    p.solidifyRate = std::clamp(std::isfinite(p.solidifyRate) ? p.solidifyRate : 0.05f, 0.0f, 1.0f);
    p.coolRate = std::clamp(std::isfinite(p.coolRate) ? p.coolRate : 0.012f, 0.0f, 1.0f);
    // Vent radius is a world distance; convert to cells. cellSize 1.0 is
    // exactly identity, so existing eruptions are untouched.
    p.ventRadius = std::clamp(std::isfinite(p.ventRadius) ? p.ventRadius : 2.5f, 0.5f, 64.0f)
                 / (m_Params.cellSize > 0.0f ? m_Params.cellSize : 1.0f);
    if (p.steps == 0) return;

    EnsureLavaFields();
    const size_t count = static_cast<size_t>(size) * size;

    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    const float sqrt2 = 1.41421356f;

    // Yield strength expressed as a slope the lava can hold without moving.
    // Water holds none; fluid basalt holds a little and runs out into long
    // sheets; silicic lava holds a lot and piles into stubby domes. The
    // viscosity slider walks that range — roughly 0.2 deg to 8 deg of surface
    // slope, which is where real flows actually sit. An earlier scale reached
    // 15 deg at the midpoint, stiff enough that a flow never left its own
    // crater: it just built a talus cone inside it and froze.
    const float baseTalus = (0.004f + 0.13f * p.viscosity) * m_Params.cellSize;

    // Chilled lava is far stiffer than fresh lava. This one term is what
    // produces levees: the margins of a flow cool first, stop moving, and pen
    // the still-molten core into a channel that runs much further than an
    // unconfined sheet would.
    const float chillFactor = 4.0f;

    // Inflow and outflow are tracked separately rather than as one net delta.
    // Heat is a mass-weighted average, so it needs the mass that actually
    // *arrived*: a cell that both receives and sends sees a net delta smaller
    // than its true inflow, and dividing the incoming heat by that net figure
    // inflates it without bound. Lava was coming out five times hotter than
    // the vent that erupted it.
    std::vector<float> inflow(count, 0.0f);
    std::vector<float> outflow(count, 0.0f);
    std::vector<float> heatFlux(count, 0.0f);

    // Only the region that has ever held lava is iterated. Early steps touch a
    // handful of cells around each vent; without this the cost would be the
    // whole grid from step one for a flow that covers a few percent of it.
    int bx0 = size, bx1 = -1, by0 = size, by1 = -1;
    auto touch = [&](int x, int y) {
        bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
        by0 = std::min(by0, y); by1 = std::max(by1, y);
    };

    // Seed the window from lava already on the map. The bounds are per-call
    // state but the lava field is not: hosts slice a long eruption into
    // several calls to animate it, and a continuation that started its window
    // empty would simply stop simulating every flow outside the vent's
    // immediate surroundings — the run would silently depend on how it was
    // chunked.
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (m_Lava[static_cast<size_t>(Index(x, y))] > 1e-6f) touch(x, y);
        }
    }

    // Ground under the lava. Chilled lava was folded straight into the bedrock
    // when it froze, so it is already in here — m_LavaRock is only a record of
    // *where* that happened, kept for shading, and adding it again would count
    // every solidified flow twice.
    auto groundAt = [&](size_t i) {
        return m_Bedrock[i] + m_Sediment[i];
    };

    // Emission points: the lowest cell inside each crater. Recomputed each
    // eruption step so that once a crater floods, the vent tracks the rising
    // pond surface rather than injecting underneath it.
    const int ventR = std::max(1, static_cast<int>(std::ceil(p.ventRadius)));

    for (int step = 0; step < p.steps; ++step) {
        // -- 1. Erupt ------------------------------------------------------
        // A single burst releases a fixed multiple of the rate rather than
        // something scaled by p.steps. Hosts slice a long eruption into
        // several calls so the viewport can animate it, and a step-dependent
        // burst would make the total volume depend on how finely it was
        // sliced. With this, a host runs the burst on its first slice and
        // passes rate 0 afterwards, and any slicing gives the same result.
        const bool erupting = (p.sustain != 0 || step == 0) && p.eruptionRate > 0.0f;
        if (erupting) {
            const float pulse = (p.sustain != 0)
                ? p.eruptionRate
                : p.eruptionRate * kBurstVolume;

            for (const Vent& v : m_Vents) {
                const int vcx = static_cast<int>(std::lround(v.x));
                const int vcy = static_cast<int>(std::lround(v.y));
                const int cr = std::max(1, static_cast<int>(std::ceil(v.craterRadius)));

                // Lowest point of the crater floor — where a real conduit
                // surfaces, and the point a pond drains toward.
                int ex = vcx, ey = vcy;
                float lowest = 1e30f;
                for (int j = -cr; j <= cr; ++j) {
                    for (int i = -cr; i <= cr; ++i) {
                        const int sx = vcx + i, sy = vcy + j;
                        if (!InBounds(sx, sy)) continue;
                        if (static_cast<float>(i * i + j * j) > v.craterRadius * v.craterRadius) continue;
                        const size_t si = static_cast<size_t>(Index(sx, sy));
                        const float h = groundAt(si);
                        if (h < lowest) { lowest = h; ex = sx; ey = sy; }
                    }
                }

                // Spread the pulse over a small disc so the vent does not
                // become a single-cell spike the flow rule has to unwind.
                float weightSum = 0.0f;
                for (int j = -ventR; j <= ventR; ++j) {
                    for (int i = -ventR; i <= ventR; ++i) {
                        const int sx = ex + i, sy = ey + j;
                        if (!InBounds(sx, sy)) continue;
                        const float d = std::sqrt(static_cast<float>(i * i + j * j));
                        if (d > p.ventRadius) continue;
                        weightSum += 1.0f - d / p.ventRadius;
                    }
                }
                if (weightSum <= 0.0f) weightSum = 1.0f;

                for (int j = -ventR; j <= ventR; ++j) {
                    for (int i = -ventR; i <= ventR; ++i) {
                        const int sx = ex + i, sy = ey + j;
                        if (!InBounds(sx, sy)) continue;
                        const float d = std::sqrt(static_cast<float>(i * i + j * j));
                        if (d > p.ventRadius) continue;
                        const size_t si = static_cast<size_t>(Index(sx, sy));
                        const float add = pulse * (1.0f - d / p.ventRadius) / weightSum;
                        if (add <= 0.0f) continue;
                        // Fresh lava arrives at full heat and reheats whatever
                        // it lands on, so a sustained vent keeps its channel open.
                        const float before = m_Lava[si];
                        const float after = before + add;
                        m_LavaHeat[si] = std::min(1.0f, (m_LavaHeat[si] * before + add) / after);
                        m_Lava[si] = after;
                        touch(sx, sy);
                    }
                }
            }
        }

        if (bx1 < bx0 || by1 < by0) continue;

        // Grow the active window by one ring so the flow can advance into it.
        const int ax0 = std::max(0, bx0 - 1), ax1 = std::min(size - 1, bx1 + 1);
        const int ay0 = std::max(0, by0 - 1), ay1 = std::min(size - 1, by1 + 1);

        // -- 2. Flow -------------------------------------------------------
        // Jacobi transfer against a single snapshot: every cell computes its
        // outflow from the same surface, then all deltas merge. Order-free,
        // and therefore identical whatever the traversal.
        for (int y = ay0; y <= ay1; ++y) {
            for (int x = ax0; x <= ax1; ++x) {
                const size_t i = static_cast<size_t>(Index(x, y));
                inflow[i] = 0.0f;
                outflow[i] = 0.0f;
                heatFlux[i] = 0.0f;
            }
        }

        for (int y = ay0; y <= ay1; ++y) {
            for (int x = ax0; x <= ax1; ++x) {
                const size_t i = static_cast<size_t>(Index(x, y));
                const float here = m_Lava[i];
                if (here <= 1e-6f) continue;

                const float heat = m_LavaHeat[i];
                // Cold lava resists motion; hot lava barely does.
                const float talus = baseTalus * (1.0f + chillFactor * (1.0f - heat));
                const float surface = groundAt(i) + here;

                float excess[8];
                size_t nIdx[8];
                int nCount = 0;
                float totalExcess = 0.0f;
                float maxExcess = 0.0f;

                for (int k = 0; k < 8; ++k) {
                    const int nx = x + dx8[k];
                    const int ny = y + dy8[k];
                    if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
                    const size_t ni = static_cast<size_t>(Index(nx, ny));
                    const float t = (k < 4) ? talus : talus * sqrt2;
                    const float diff = surface - (groundAt(ni) + m_Lava[ni]);
                    if (diff > t) {
                        const float e = diff - t;
                        // Routing weight, not the surplus itself. Splitting in
                        // proportion to the raw surplus sends real volume down
                        // every slightly-downhill neighbour, and the flow leaves
                        // the cone as a broad apron. Squaring biases it onto the
                        // steepest line — the same multiple-flow-direction
                        // exponent drainage models use to make rivers converge —
                        // so the lava picks a course and stays in it.
                        excess[nCount] = e * e;
                        nIdx[nCount] = ni;
                        ++nCount;
                        totalExcess += e * e;
                        maxExcess = std::max(maxExcess, e);
                    }
                }
                if (nCount == 0) continue;

                // Half the largest surplus per step keeps the explicit scheme
                // stable while still advancing a front about a cell per step.
                const float toMove = std::min(here, 0.5f * maxExcess);
                if (toMove <= 0.0f) continue;

                outflow[i] += toMove;
                for (int k = 0; k < nCount; ++k) {
                    const float share = toMove * (excess[k] / totalExcess);
                    inflow[nIdx[k]] += share;
                    // Heat rides along with the mass it belongs to.
                    heatFlux[nIdx[k]] += share * heat;
                    const int nx = static_cast<int>(nIdx[k]) % size;
                    const int ny = static_cast<int>(nIdx[k]) / size;
                    touch(nx, ny);
                }
            }
        }

        // -- 3. Apply, mix heat, cool, chill -------------------------------
        for (int y = ay0; y <= ay1; ++y) {
            for (int x = ax0; x <= ax1; ++x) {
                const size_t i = static_cast<size_t>(Index(x, y));
                const float before = m_Lava[i];
                const float kept = std::max(0.0f, before - outflow[i]);
                const float after = kept + inflow[i];
                if (after <= 1e-6f && before <= 1e-6f) continue;

                // Mass-weighted heat: what stayed keeps this cell's heat, what
                // arrived brings its donors'. heatFlux is already the sum of
                // (mass * heat) over every donor, so it divides out by `after`.
                if (after > 1e-6f) {
                    m_LavaHeat[i] = (m_LavaHeat[i] * kept + heatFlux[i]) / after;
                } else {
                    m_LavaHeat[i] = 0.0f;
                }
                m_Lava[i] = after;

                if (after <= 1e-6f) continue;

                // Thin flows lose heat fast; a deep channel holds it, which is
                // why lava tubes run for kilometres and sheets do not. The
                // contrast between the two is deliberately wide — it is the
                // whole levee mechanism. A flow's thin margins chill and stop
                // while its deep core stays molten and keeps running, so the
                // channel walls itself in and the stream outlives the sheet
                // it would otherwise have spread into.
                const float thinness = 1.0f / (1.0f + after * 2.0f);
                m_LavaHeat[i] = std::max(0.0f, m_LavaHeat[i] - p.coolRate * (0.25f + 2.0f * thinness));

                // Chilled lava turns to rock. It goes into the bedrock, which
                // is what makes it real terrain: later flows have to climb over
                // it or route around it, subsequent erosion layers cut it, and
                // it lands in every export. m_LavaRock records where it was
                // laid down purely so the shaders can paint it as basalt.
                //
                // Two ways to freeze. Cooling is the obvious one. The other is
                // simply being thin: a film a few centimetres deep spreading
                // over cold ground has no thermal mass to lose and quenches on
                // contact however hot it arrived. Without that term the flow
                // front runs ahead of itself as a wide molten veneer instead of
                // stopping in a sharp lobe, and the whole apron reads as
                // glowing lava rather than a stream with edges.
                const float chill = 1.0f - m_LavaHeat[i];
                const float quench = 1.0f / (1.0f + after * 6.0f);
                float frozen = std::min(after,
                    after * p.solidifyRate * (chill * chill + quench));
                // Fully cold lava is rock, not lava.
                if (m_LavaHeat[i] <= 0.0f) frozen = m_Lava[i];
                if (frozen > 0.0f) {
                    m_Lava[i] -= frozen;
                    m_LavaRock[i] += frozen;
                    m_Bedrock[i] += frozen;
                }

                touch(x, y);
            }
        }

        // -- 4. Drain off the map ------------------------------------------
        // Lava that reaches a border edge leaves the domain, exactly as a
        // droplet does in the hydraulic model. Without this it would pond
        // against the boundary and back the flow up the slope it came down.
        for (int x = ax0; x <= ax1; ++x) {
            if (ay0 == 0) { const size_t i = static_cast<size_t>(Index(x, 0)); m_Lava[i] = 0.0f; m_LavaHeat[i] = 0.0f; }
            if (ay1 == size - 1) { const size_t i = static_cast<size_t>(Index(x, size - 1)); m_Lava[i] = 0.0f; m_LavaHeat[i] = 0.0f; }
        }
        for (int y = ay0; y <= ay1; ++y) {
            if (ax0 == 0) { const size_t i = static_cast<size_t>(Index(0, y)); m_Lava[i] = 0.0f; m_LavaHeat[i] = 0.0f; }
            if (ax1 == size - 1) { const size_t i = static_cast<size_t>(Index(size - 1, y)); m_Lava[i] = 0.0f; m_LavaHeat[i] = 0.0f; }
        }
    }

    UpdateLavaGlow();
}

// Blurs the molten field into a soft emission term. Lava is the brightest
// thing in a volcanic scene and rock beside it should show that; this is what
// the shaders read to warm up the ground around a channel instead of leaving a
// glowing ribbon on unlit black basalt.
void TerrainEngine::UpdateLavaGlow() {
    if (m_Lava.empty()) return;
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;
    if (m_LavaGlow.size() != count) m_LavaGlow.assign(count, 0.0f);

    std::vector<float> src(count);
    for (size_t i = 0; i < count; ++i) {
        src[i] = m_Lava[i] > 1e-4f ? std::clamp(m_LavaHeat[i], 0.0f, 1.0f) : 0.0f;
    }

    // Separable box blur, two passes for a gaussian-ish falloff.
    const int radius = std::max(1, size / 64);
    std::vector<float> tmp(count, 0.0f);
    for (int pass = 0; pass < 2; ++pass) {
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                int n = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int sx = std::clamp(x + k, 0, size - 1);
                    sum += src[static_cast<size_t>(y) * size + sx];
                    ++n;
                }
                tmp[static_cast<size_t>(y) * size + x] = sum / n;
            }
        }
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                float sum = 0.0f;
                int n = 0;
                for (int k = -radius; k <= radius; ++k) {
                    const int sy = std::clamp(y + k, 0, size - 1);
                    sum += tmp[static_cast<size_t>(sy) * size + x];
                    ++n;
                }
                src[static_cast<size_t>(y) * size + x] = sum / n;
            }
        }
    }

    // Keep the flow itself at full strength — the blur is for its surroundings.
    for (size_t i = 0; i < count; ++i) {
        const float direct = m_Lava[i] > 1e-4f ? std::clamp(m_LavaHeat[i], 0.0f, 1.0f) : 0.0f;
        m_LavaGlow[i] = std::max(direct, std::min(1.0f, src[i] * 1.6f));
    }
}

} // namespace Titan
