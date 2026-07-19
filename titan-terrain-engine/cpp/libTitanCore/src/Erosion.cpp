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

// Removes up to `amount` from base+delta with a linear-falloff brush,
// clamped so base+delta never goes negative. Returns the amount taken.
float BrushTake(const std::vector<float>& base, std::vector<float>& delta,
                int size, float x, float y, float amount, float radius) {
    radius = std::min(radius, 15.0f);
    const int centerX = static_cast<int>(std::floor(x));
    const int centerY = static_cast<int>(std::floor(y));
    const int r = static_cast<int>(radius);

    struct Sample { int idx; float w; };
    Sample samples[1024];
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
        const float have = std::max(0.0f, base[samples[k].idx] + delta[samples[k].idx]);
        const float take = std::min(want, have);
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
                                    std::vector<float>& flowDelta) const {
    const int size = m_Params.size;
    const float maxCoord = static_cast<float>(size) - 1.001f;
    const auto spawnMode = static_cast<SpawnMode>(p.spawnMode);
    const bool usePrecip = spawnMode == SpawnMode::Precipitation && !m_Precipitation.empty();

    for (int d = 0; d < count; ++d) {
        // Per-droplet RNG stream: depends only on seed + global index.
        uint64_t state = (static_cast<uint64_t>(m_Params.seed) << 20) ^ (firstDroplet + d);
        Pcg32 rng(SplitMix64(state), SplitMix64(state));

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

        for (int lifetime = 0; lifetime < p.maxDropletLifetime; ++lifetime) {
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

            if (posX < 0.0f || posX >= maxCoord || posY < 0.0f || posY >= maxCoord) break;

            const float newHeight = SampleHeight(posX, posY);
            const float oldHeight = SampleHeight(oldPosX, oldPosY);
            const float deltaHeight = newHeight - oldHeight;

            const float capacity = std::max(-deltaHeight * speed * water * p.sedimentCapacityFactor,
                                            p.minSedimentCapacity);

            if (sediment > capacity || deltaHeight > 0.0f) {
                const float amountToDeposit = (deltaHeight > 0.0f)
                    ? std::min(deltaHeight, sediment)
                    : (sediment - capacity) * p.depositSpeed;
                sediment -= amountToDeposit;
                DepositToDelta(sedimentDelta, size, oldPosX, oldPosY, amountToDeposit);
            } else {
                const float amountToErode = std::min((capacity - sediment) * p.dissolveSpeed,
                                                     -deltaHeight);
                const float sedimentHere = SampleWithDelta(m_Sediment, sedimentDelta,
                                                           size, oldPosX, oldPosY);

                if (sedimentHere >= amountToErode) {
                    sediment += BrushTake(m_Sediment, sedimentDelta, size,
                                          oldPosX, oldPosY, amountToErode, p.erosionRadius);
                } else {
                    const float hardness = HardnessAt(oldHeight);
                    const float bedrockDemand = (amountToErode - sedimentHere)
                        * (p.bedrockErosionSpeed / hardness);
                    sediment += BrushTake(m_Sediment, sedimentDelta, size,
                                          oldPosX, oldPosY, sedimentHere, p.erosionRadius);
                    sediment += BrushTake(m_Bedrock, bedrockDelta, size,
                                          oldPosX, oldPosY, bedrockDemand, p.erosionRadius);
                }
            }

            speed = std::sqrt(std::max(0.0f, speed * speed + deltaHeight * p.gravity));
            water *= (1.0f - p.evaporateSpeed);

            if (nodeX >= 0 && nodeX < size && nodeY >= 0 && nodeY < size) {
                flowDelta[nodeY * size + nodeX] += water * 0.1f;
            }

            if (speed == 0.0f) break;
        }
    }
}

void TerrainEngine::ApplyHydraulicErosion(int iterations, const HydraulicParams& p) {
    const int size = m_Params.size;
    if (size < 2 || iterations <= 0) return;
    const size_t cellCount = static_cast<size_t>(size) * size;

    // Round up to whole batches — part of the determinism contract.
    const int batchCount = (iterations + kDropletBatch - 1) / kDropletBatch;

    // Per-batch delta buffers for one round.
    struct BatchDeltas {
        std::vector<float> sediment, bedrock, flow;
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
            const uint64_t first = m_DropletCursor
                + static_cast<uint64_t>(processed + b) * kDropletBatch;
            RunDropletBatch(first, kDropletBatch, p, d.sediment, d.bedrock, d.flow);
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

        // Merge in fixed batch order; clamp layers at zero (two batches can
        // both take from the same cell against the shared snapshot).
        for (int b = 0; b < inRound; ++b) {
            const auto& d = deltas[b];
            for (size_t i = 0; i < cellCount; ++i) {
                m_Sediment[i] = std::max(0.0f, m_Sediment[i] + d.sediment[i]);
                m_Bedrock[i] = std::max(0.0f, m_Bedrock[i] + d.bedrock[i]);
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

    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    const float sqrt2 = 1.41421356f;

    std::vector<float> sedimentDelta(count);
    std::vector<float> bedrockDelta(count);

    for (int pass = 0; pass < passes; ++pass) {
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

                float toMove = p.rate * 0.5f * maxExcess;

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
            m_Sediment[i] += sedimentDelta[i];
        }
    }
}

} // namespace Titan
