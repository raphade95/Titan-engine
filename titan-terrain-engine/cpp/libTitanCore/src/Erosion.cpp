#include "TitanCore.h"
#include "TitanRandom.h"

#include <algorithm>
#include <cmath>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <future>
#endif

namespace Titan {

// ---------------------------------------------------------------------------
// Hydraulic erosion — Lagrangian droplet model.
//
// Parallel *and* deterministic. Every droplet's RNG derives from
// (terrain seed, global droplet index), so spawn sequences don't depend on
// thread scheduling or how iterations are split across calls. Droplets run
// in fixed batches (kDropletBatch) grouped into rounds (kBatchesPerRound):
// all batches in a round read the same terrain snapshot and write private
// delta buffers, which merge in batch order at the end of the round. The
// result is bit-identical on 1 thread, 8 threads, or WASM.
// ---------------------------------------------------------------------------

namespace {

// Removes up to `amount` from base+delta with a linear-falloff brush.
// Returns the amount actually taken.
//
// `floorAtZero` distinguishes the two layers: sediment is a deposit, so a
// brush can take no more than is there, while bedrock is an elevation that
// may legitimately be cut below the zero datum (a river carving a gorge below
// sea level). The erosion amount is separately bounded by the local drop, so
// unfloored bedrock removal is still self-limiting.
float BrushTake(const std::vector<float>& base, std::vector<float>& delta,
                int size, float x, float y, float amount, float radius,
                bool floorAtZero) {
    // The cap bounds the fixed sample array below: a radius of r visits
    // (2r+1)^2 cells, so 31 needs 3969 slots. It also has to leave room for a
    // world-unit radius converted to cells on a fine grid — an erosion radius
    // of 3 world units is 24 cells at cellSize 0.125.
    radius = std::min(radius, 31.0f);
    const int centerX = static_cast<int>(std::floor(x));
    const int centerY = static_cast<int>(std::floor(y));
    const int r = static_cast<int>(radius);

    struct Sample { int idx; float w; };
    Sample samples[4096];
    int count = 0;
    float weightSum = 0.0f;

    for (int j = -r; j <= r; ++j) {
        for (int i = -r; i <= r; ++i) {
            const int px = centerX + i;
            const int py = centerY + j;
            if (px < 0 || px >= size || py < 0 || py >= size) continue;
            const float dx = static_cast<float>(px) - x;
            const float dy = static_cast<float>(py) - y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist >= radius) continue;
            samples[count++] = {py * size + px, 1.0f - dist / radius};
            weightSum += 1.0f - dist / radius;
        }
    }

    if (weightSum <= 0.0f) return 0.0f;

    float taken = 0.0f;
    for (int k = 0; k < count; ++k) {
        const float want = amount * (samples[k].w / weightSum);
        const float take = floorAtZero
            ? std::min(want, std::max(0.0f, base[samples[k].idx] + delta[samples[k].idx]))
            : want;
        delta[samples[k].idx] -= take;
        taken += take;
    }
    return taken;
}

void DepositToDelta(std::vector<float>& delta, int size, float x, float y, float amount) {
    const int nx = static_cast<int>(std::floor(x));
    const int ny = static_cast<int>(std::floor(y));
    const float u = x - nx;
    const float v = y - ny;

    auto add = [&](int px, int py, float a) {
        if (px < 0 || px >= size || py < 0 || py >= size) return;
        delta[py * size + px] += a;
    };
    add(nx, ny, amount * (1 - u) * (1 - v));
    add(nx + 1, ny, amount * u * (1 - v));
    add(nx, ny + 1, amount * (1 - u) * v);
    add(nx + 1, ny + 1, amount * u * v);
}

float SampleWithDelta(const std::vector<float>& base, const std::vector<float>& delta,
                      int size, float x, float y) {
    const int nx = static_cast<int>(std::floor(x));
    const int ny = static_cast<int>(std::floor(y));
    const float u = x - nx;
    const float v = y - ny;

    auto at = [&](int px, int py) -> float {
        px = std::clamp(px, 0, size - 1);
        py = std::clamp(py, 0, size - 1);
        const int i = py * size + px;
        return std::max(0.0f, base[i] + delta[i]);
    };
    return at(nx, ny) * (1 - u) * (1 - v) +
           at(nx + 1, ny) * u * (1 - v) +
           at(nx, ny + 1) * (1 - u) * v +
           at(nx + 1, ny + 1) * u * v;
}

} // namespace

void TerrainEngine::RunDropletBatch(uint64_t firstDroplet, int count, const HydraulicParams& p,
                                    std::vector<float>& sedimentDelta,
                                    std::vector<float>& bedrockDelta,
                                    std::vector<float>& flowDelta,
                                    double& exported) const {
    const int size = m_Params.size;
    const float maxCoord = static_cast<float>(size) - 1.001f;
    const auto spawnMode = static_cast<SpawnMode>(p.spawnMode);
    const bool usePrecip = spawnMode == SpawnMode::Precipitation && !m_Precipitation.empty();

    // The surface as this batch has already reshaped it.
    //
    // SampleHeight reads the committed field, which does not include the
    // deposits this batch is still holding in its delta buffers. The erosion
    // branch has always read sediment through SampleWithDelta for exactly that
    // reason; the deposition branch did not, and it is the one that needed it
    // more. A droplet would fill a hollow, and the next 2047 droplets of the
    // batch would still measure that hollow as empty and fill it again, with
    // nothing anywhere bounding the total. That is what the spikes were: after
    // 200k droplets, 788 cells stood more than a unit above every one of their
    // eight neighbours, the worst by 113 units on terrain whose median is 29.
    //
    // This does not weaken the determinism guarantee. Batches still run
    // against their own private deltas and merge in fixed batch order, so the
    // result is unchanged by thread count — the batch is merely consistent
    // with itself now, which it always claimed to be.
    auto LiveHeight = [&](float x, float y) -> float {
        const int nx = static_cast<int>(std::floor(x));
        const int ny = static_cast<int>(std::floor(y));
        const float u = x - nx;
        const float v = y - ny;
        auto at = [&](int px, int py) -> float {
            // Clamp to match GetHeight, so edges do not read as cliffs.
            px = std::clamp(px, 0, size - 1);
            py = std::clamp(py, 0, size - 1);
            const size_t i = static_cast<size_t>(py) * size + px;
            return m_Bedrock[i] + bedrockDelta[i] + m_Sediment[i] + sedimentDelta[i];
        };
        return at(nx, ny) * (1 - u) * (1 - v) + at(nx + 1, ny) * u * (1 - v)
             + at(nx, ny + 1) * (1 - u) * v + at(nx + 1, ny + 1) * u * v;
    };

    // --- Cell space vs. world space ----------------------------------------
    //
    // A droplet advances exactly one *cell* per step while heights are in
    // *world* units, so every quantity below that mixes the two is really a
    // function of the sample spacing. That never showed while cellSize was
    // pinned at 1.0. Once world size and resolution became independent it
    // meant refining the grid silently rewrote the physics: the same seed and
    // the same droplets over the same landscape produced a 137-unit
    // depositional peak at cellSize 1.0 and a 31-unit one at 0.25.
    //
    // The fix is to reference the world, not the grid:
    //   * slopes are rises per world unit, not per cell;
    //   * a droplet's life is a world distance, so a finer grid gets
    //     proportionally more (and proportionally smaller) steps;
    //   * per-step rates scale with the step length so the total along a given
    //     path is the same however finely it was walked;
    //   * the erosion brush is a world radius converted to cells.
    //
    // Every factor is normalized so cellSize 1.0 is exactly identity — `x *
    // 1.0f` and `x / 1.0f` are exact in floating point — which keeps every
    // existing project, preset and golden hash bit-for-bit unchanged.
    const float cs = m_Params.cellSize > 0.0f ? m_Params.cellSize : 1.0f;
    const float invCs = 1.0f / cs;

    // World radius -> cells.
    const float brushRadius = p.erosionRadius * invCs;
    // A fixed world distance to travel, walked in cell-sized steps.
    const int lifetimeSteps = std::max(1, static_cast<int>(
        std::lround(static_cast<double>(p.maxDropletLifetime) * invCs)));

    for (int d = 0; d < count; ++d) {
        // Per-droplet RNG stream: depends only on seed + global index.
        // MakeRng pins down the seeding order; see TitanRandom.h.
        Pcg32 rng = MakeRng((static_cast<uint64_t>(m_Params.seed) << 20) ^ (firstDroplet + d));

        float posX = rng.NextFloat() * maxCoord;
        float posY = rng.NextFloat() * maxCoord;

        // Weighted spawning via rejection sampling (bounded retries keep the
        // per-droplet cost fixed).
        if (spawnMode != SpawnMode::Uniform) {
            for (int attempt = 0; attempt < 8; ++attempt) {
                float weight;
                if (usePrecip) {
                    weight = std::clamp(SamplePrecipitation(posX, posY), 0.0f, 1.0f);
                } else {
                    const float h = SampleHeight(posX, posY);
                    weight = m_Params.heightMultiplier > 0.0f
                        ? std::clamp(h / m_Params.heightMultiplier, 0.05f, 1.0f)
                        : 1.0f;
                }
                if (rng.NextFloat() <= weight) break;
                posX = rng.NextFloat() * maxCoord;
                posY = rng.NextFloat() * maxCoord;
            }
        }

        float dirX = 0.0f, dirY = 0.0f;
        float speed = 1.0f;
        float water = 1.0f;
        float sediment = 0.0f;

        for (int lifetime = 0; lifetime < lifetimeSteps; ++lifetime) {
            const int nodeX = static_cast<int>(posX);
            const int nodeY = static_cast<int>(posY);

            float gx, gy;
            GradientAt(posX, posY, gx, gy);

            dirX = dirX * p.inertia - gx * (1.0f - p.inertia);
            dirY = dirY * p.inertia - gy * (1.0f - p.inertia);

            const float len = std::sqrt(dirX * dirX + dirY * dirY);
            if (len != 0.0f) {
                dirX /= len;
                dirY /= len;
            }

            const float oldPosX = posX;
            const float oldPosY = posY;
            posX += dirX;
            posY += dirY;

            if (posX < 0.0f || posX >= maxCoord || posY < 0.0f || posY >= maxCoord) {
                // The droplet leaves the map still carrying its load. That
                // material has genuinely left the system (transported to the
                // sea), so it is recorded rather than silently vanishing —
                // the mass-conservation test balances against it.
                exported += static_cast<double>(sediment);
                sediment = 0.0f;
                break;
            }

            const float newHeight = LiveHeight(posX, posY);
            const float oldHeight = LiveHeight(oldPosX, oldPosY);
            const float deltaHeight = newHeight - oldHeight;
            // Rise per world unit travelled, so the droplet reads the terrain's
            // real steepness rather than a figure that shrinks as the grid is
            // refined.
            const float slope = deltaHeight * invCs;

            const float capacity = std::max(-slope * speed * water * p.sedimentCapacityFactor,
                                            p.minSedimentCapacity);

            const float maskHere = SampleMask(oldPosX, oldPosY);

            if (sediment > capacity || deltaHeight > 0.0f) {
                // The uphill case is bounded by the actual rise, which already
                // scales with the step. The rate-driven case is per unit
                // distance, so it takes the step length explicitly.
                float amountToDeposit = (deltaHeight > 0.0f)
                    ? std::min(deltaHeight, sediment)
                    : (sediment - capacity) * p.depositSpeed * cs;
                amountToDeposit *= maskHere;
                sediment -= amountToDeposit;
                DepositToDelta(sedimentDelta, size, oldPosX, oldPosY, amountToDeposit);
            } else {
                const float amountToErode = maskHere
                    * std::min((capacity - sediment) * p.dissolveSpeed * cs, -deltaHeight);
                const float sedimentHere = SampleWithDelta(m_Sediment, sedimentDelta,
                                                           size, oldPosX, oldPosY);

                if (sedimentHere >= amountToErode) {
                    sediment += BrushTake(m_Sediment, sedimentDelta, size,
                                          oldPosX, oldPosY, amountToErode,
                                          brushRadius, true);
                } else {
                    const float hardness = HardnessAt(oldHeight);
                    const float bedrockDemand = (amountToErode - sedimentHere)
                        * (p.bedrockErosionSpeed / hardness);
                    sediment += BrushTake(m_Sediment, sedimentDelta, size,
                                          oldPosX, oldPosY, sedimentHere,
                                          brushRadius, true);
                    sediment += BrushTake(m_Bedrock, bedrockDelta, size,
                                          oldPosX, oldPosY, bedrockDemand,
                                          brushRadius, false);
                }
            }

            speed = std::sqrt(std::max(0.0f, speed * speed + slope * p.gravity));
            // Evaporation and flow accumulation are per unit distance too, so
            // a finer grid taking more, shorter steps loses the same water and
            // records the same flow over the same path.
            water *= (1.0f - p.evaporateSpeed * cs);

            if (nodeX >= 0 && nodeX < size && nodeY >= 0 && nodeY < size) {
                flowDelta[nodeY * size + nodeX] += water * 0.1f * cs;
            }

            if (speed == 0.0f) break;
        }
        // Whatever a droplet still carries when it stalls or times out also
        // leaves the system.
        exported += static_cast<double>(sediment);
    }
}

void TerrainEngine::ApplyHydraulicErosion(int iterations, const HydraulicParams& p) {
    const int size = m_Params.size;
    if (size < 2 || iterations <= 0) return;
    const size_t cellCount = static_cast<size_t>(size) * size;

    // Round up to a whole number of *rounds*, not merely whole batches.
    //
    // Batches inside a round share one terrain snapshot, so a round is the
    // real unit of work: only a whole number of rounds composes. Rounding to
    // batches instead made the outcome depend on how the caller happened to
    // slice the work — the web lab streams progress in 16384-droplet rounds
    // while TitanLab and the Unreal plugin pass the count in one call, so a
    // request for 20000 droplets simulated 32768 on the web and 20480 on the
    // desktop. Same project file, different terrain. Rounding here makes any
    // sequence of calls equivalent to one large call, unconditionally.
    const int rounds = (iterations + kDropletsPerRound - 1) / kDropletsPerRound;
    const int batchCount = rounds * kBatchesPerRound;

    // Per-batch delta buffers for one round.
    struct BatchDeltas {
        std::vector<float> sediment, bedrock, flow;
        double exported = 0.0;
    };
    std::vector<BatchDeltas> deltas(kBatchesPerRound);
    for (auto& b : deltas) {
        b.sediment.assign(cellCount, 0.0f);
        b.bedrock.assign(cellCount, 0.0f);
        b.flow.assign(cellCount, 0.0f);
    }

    int processed = 0;
    while (processed < batchCount) {
        const int inRound = std::min(kBatchesPerRound, batchCount - processed);

        auto runBatch = [&](int b) {
            auto& d = deltas[b];
            std::fill(d.sediment.begin(), d.sediment.end(), 0.0f);
            std::fill(d.bedrock.begin(), d.bedrock.end(), 0.0f);
            std::fill(d.flow.begin(), d.flow.end(), 0.0f);
            d.exported = 0.0;
            const uint64_t first = m_DropletCursor
                + static_cast<uint64_t>(processed + b) * kDropletBatch;
            RunDropletBatch(first, kDropletBatch, p, d.sediment, d.bedrock, d.flow,
                            d.exported);
        };

#if defined(__EMSCRIPTEN__)
        for (int b = 0; b < inRound; ++b) runBatch(b);
#else
        {
            std::vector<std::future<void>> jobs;
            jobs.reserve(inRound);
            for (int b = 0; b < inRound; ++b) {
                jobs.push_back(std::async(std::launch::async, runBatch, b));
            }
            for (auto& j : jobs) j.get();
        }
#endif

        // Merge in fixed batch order.
        for (int b = 0; b < inRound; ++b) {
            const auto& d = deltas[b];
            m_MassExported += d.exported;
            for (size_t i = 0; i < cellCount; ++i) {
                // Sediment is a deposit and floors at zero. That floor is the
                // one place hydraulic erosion can create mass: two batches in
                // a round can both draw from the same cell against the shared
                // snapshot, between them asking for more than was there. The
                // excess is recorded so the conservation test can bound it
                // rather than have it silently inflate the terrain.
                const float wantSed = m_Sediment[i] + d.sediment[i];
                if (wantSed < 0.0f) m_MassCreated += -static_cast<double>(wantSed);
                m_Sediment[i] = std::max(0.0f, wantSed);
                m_Bedrock[i] += d.bedrock[i];
                m_Flow[i] += d.flow[i];
            }
        }

        processed += inRound;
    }

    m_DropletCursor += static_cast<uint64_t>(batchCount) * kDropletBatch;
}

// ---------------------------------------------------------------------------
// Thermal weathering — angle-of-repose talus creep.
//
// Double-buffered (Jacobi) update: all moves are computed against the
// start-of-pass state and accumulated into delta buffers, then applied in one
// sweep. This removes scan-order bias and conserves mass exactly by
// construction.
// ---------------------------------------------------------------------------
void TerrainEngine::ApplyThermalWeathering(int passes, const ThermalParams& p) {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;

    const float pi = 3.14159265358979323846f;
    const float talusCardinal = std::tan(p.talusAngleDeg * pi / 180.0f) * m_Params.cellSize;

    // A pass moves material exactly one *cell*, so a pass count is a
    // cell-space travel distance: at half the sample spacing, the same number
    // of passes creeps half as far across the actual landscape and the terrain
    // comes out visibly less settled. The talus angle above was already
    // cellSize-aware; the iteration count was the last thing in the engine
    // still denominated in cells.
    //
    // Scaling by 1/cellSize makes a pass a fixed *world* distance. cellSize 1.0
    // is exactly identity (`x * 1.0f` is exact), so every existing project,
    // preset and golden hash is untouched.
    //
    // Capped, because this multiplies work: the cost is passes * size^2 and a
    // very fine grid over a small world could otherwise ask for hundreds of
    // full-grid relaxations. The cap is generous enough to cover the whole
    // resolution slider against a sane world size, and past it thermal simply
    // under-settles rather than hanging.
    const float cs = m_Params.cellSize > 0.0f ? m_Params.cellSize : 1.0f;
    const long long scaled = std::llround(static_cast<double>(passes) / cs);
    const long long capped = std::min<long long>(scaled,
                                                 std::max(1, passes) * kThermalPassScaleCap);
    const int effectivePasses = static_cast<int>(std::max<long long>(1, capped));

    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    const float sqrt2 = 1.41421356f;

    std::vector<float> sedimentDelta(count);
    std::vector<float> bedrockDelta(count);

    for (int pass = 0; pass < effectivePasses; ++pass) {
        std::fill(sedimentDelta.begin(), sedimentDelta.end(), 0.0f);
        std::fill(bedrockDelta.begin(), bedrockDelta.end(), 0.0f);

        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int i = Index(x, y);
                const float h = m_Bedrock[i] + m_Sediment[i];

                float excess[8];
                int nIdx[8];
                int nCount = 0;
                float totalExcess = 0.0f;
                float maxExcess = 0.0f;

                for (int k = 0; k < 8; ++k) {
                    const int nx = x + dx8[k];
                    const int ny = y + dy8[k];
                    if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;

                    const float talus = (k < 4) ? talusCardinal : talusCardinal * sqrt2;
                    const int ni = Index(nx, ny);
                    const float diff = h - (m_Bedrock[ni] + m_Sediment[ni]);
                    if (diff > talus) {
                        const float e = diff - talus;
                        excess[nCount] = e;
                        nIdx[nCount] = ni;
                        ++nCount;
                        totalExcess += e;
                        maxExcess = std::max(maxExcess, e);
                    }
                }

                if (nCount == 0) continue;

                float toMove = p.rate * 0.5f * maxExcess * MaskAt(i);

                // Availability is judged on start-of-pass state only, so
                // results are independent of scan order. Bedrock fractures
                // into sediment in place (conserves mass), then moves.
                float available = m_Sediment[i];
                if (available < toMove) {
                    const float breakdown = std::min((toMove - available) * p.bedrockBreakdownRate,
                                                     std::max(0.0f, m_Bedrock[i]));
                    bedrockDelta[i] -= breakdown;
                    sedimentDelta[i] += breakdown;
                    available += breakdown;
                }
                toMove = std::min(toMove, std::max(0.0f, available));
                if (toMove <= 0.0f) continue;

                sedimentDelta[i] -= toMove;
                for (int k = 0; k < nCount; ++k) {
                    sedimentDelta[nIdx[k]] += toMove * (excess[k] / totalExcess);
                }
            }
        }

        for (size_t i = 0; i < count; ++i) {
            m_Bedrock[i] += bedrockDelta[i];
            // toMove never exceeds the sediment present, but the delta also
            // carries incoming material and the bedrock breakdown, and summing
            // those in float can land a cell a few 1e-9 below zero. Sediment
            // is a deposit; hold the invariant rather than leave dust behind.
            m_Sediment[i] = std::max(0.0f, m_Sediment[i] + sedimentDelta[i]);
        }
    }
}

} // namespace Titan
