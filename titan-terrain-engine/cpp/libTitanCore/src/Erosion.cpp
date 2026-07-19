#include "TitanCore.h"
#include "TitanRandom.h"

#include <algorithm>
#include <cmath>

namespace Titan {

// ---------------------------------------------------------------------------
// Hydraulic erosion — Lagrangian droplet model.
//
// Fully deterministic: droplet spawns come from a PCG32 stream derived from
// the terrain seed, so the same seed + iteration count always produces the
// identical terrain on every platform.
// ---------------------------------------------------------------------------
void TerrainEngine::ApplyHydraulicErosion(int iterations, const HydraulicParams& p) {
    const int size = m_Params.size;
    if (size < 2) return;

    uint64_t seedState = static_cast<uint64_t>(m_Params.seed) ^ 0x8BADF00D5EEDULL;
    Pcg32 rng(SplitMix64(seedState), SplitMix64(seedState));

    const float maxCoord = static_cast<float>(size) - 1.001f;

    for (int it = 0; it < iterations; ++it) {
        float posX = rng.NextFloat() * maxCoord;
        float posY = rng.NextFloat() * maxCoord;
        float dirX = 0.0f, dirY = 0.0f;
        float speed = 1.0f;
        float water = 1.0f;
        float sediment = 0.0f;

        for (int lifetime = 0; lifetime < p.maxDropletLifetime; ++lifetime) {
            const int nodeX = static_cast<int>(posX);
            const int nodeY = static_cast<int>(posY);

            float gx, gy;
            GradientAt(posX, posY, gx, gy);

            // Momentum blend: inertia lets droplets carve through small
            // ridges and form curved riverbeds instead of zigzagging.
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
                // Going uphill or over capacity: deposit.
                const float amountToDeposit = (deltaHeight > 0.0f)
                    ? std::min(deltaHeight, sediment)
                    : (sediment - capacity) * p.depositSpeed;
                sediment -= amountToDeposit;
                DepositSediment(oldPosX, oldPosY, amountToDeposit);
            } else {
                // Erode, but never more than the height difference (prevents
                // digging pits below the downstream level).
                const float amountToErode = std::min((capacity - sediment) * p.dissolveSpeed,
                                                     -deltaHeight);
                const float sedimentHere = SampleSediment(oldPosX, oldPosY);

                if (sedimentHere >= amountToErode) {
                    // Loose sediment covers the demand.
                    sediment += ErodeSedimentBrush(oldPosX, oldPosY, amountToErode, p.erosionRadius);
                } else {
                    // Take all loose sediment, then chew bedrock slowly,
                    // scaled by the local strata hardness.
                    const float hardness = HardnessAt(oldHeight);
                    const float bedrockDemand = (amountToErode - sedimentHere)
                        * (p.bedrockErosionSpeed / hardness);
                    sediment += ErodeSedimentBrush(oldPosX, oldPosY, sedimentHere, p.erosionRadius);
                    sediment += ErodeBedrockBrush(oldPosX, oldPosY, bedrockDemand, p.erosionRadius);
                }
            }

            speed = std::sqrt(std::max(0.0f, speed * speed + deltaHeight * p.gravity));
            water *= (1.0f - p.evaporateSpeed);

            if (InBounds(nodeX, nodeY)) {
                m_Flow[Index(nodeX, nodeY)] += water * 0.1f;
            }

            if (speed == 0.0f) break;
        }
    }
}

// ---------------------------------------------------------------------------
// Thermal weathering — angle-of-repose talus creep.
//
// Double-buffered (Jacobi) update: all moves are computed against the
// start-of-pass state and accumulated into delta buffers, then applied in one
// sweep. This removes the scan-order directional bias of in-place updates,
// and conserves mass exactly by construction (every subtraction has matching
// additions distributed across neighbours).
// ---------------------------------------------------------------------------
void TerrainEngine::ApplyThermalWeathering(int passes, const ThermalParams& p) {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;

    // Talus threshold as a height difference: tan(angle) * horizontal distance.
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

                // Collect neighbours below the angle of repose.
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

                // Move at most half the steepest excess per pass — moving the
                // full difference overshoots and oscillates.
                float toMove = p.rate * 0.5f * maxExcess;

                // Thermal creep moves loose sediment. If there isn't enough,
                // fracture bedrock into sediment (in place — conserves mass)
                // at a slower rate, then move what's available. Availability
                // is judged on start-of-pass state only, so results are
                // independent of scan order.
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
