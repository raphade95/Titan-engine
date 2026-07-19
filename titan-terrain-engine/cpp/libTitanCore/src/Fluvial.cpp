#include "TitanCore.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <vector>

namespace Titan {

// ---------------------------------------------------------------------------
// Fluvial erosion — flow accumulation + stream power law.
//
// This is what droplet erosion alone can't give you: coherent, branching
// drainage networks. Per iteration:
//
//   1. Priority-flood fill depressions (Barnes et al. 2014) so every cell
//      has a monotone downhill path to the border — rivers never dead-end
//      in noise pits.
//   2. D8 flow routing on the filled surface.
//   3. Flow accumulation in descending height order (each cell passes its
//      drainage area downstream).
//   4. Stream power erosion E = K * A^m * S^n applied to the real surface,
//      sediment first, then hardness-scaled bedrock. A fraction of eroded
//      material re-deposits downstream; the rest exits the system as if
//      transported to the sea.
//
// Deterministic: no randomness anywhere in this pass.
// ---------------------------------------------------------------------------
void TerrainEngine::ApplyFluvialErosion(int iterations, const FluvialParams& p) {
    const int size = m_Params.size;
    if (size < 3) return;
    const int count = size * size;

    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};
    const float sqrt2 = 1.41421356f;

    std::vector<float> height(count);
    std::vector<float> filled(count);
    std::vector<int> downstream(count);
    std::vector<float> area(count);
    std::vector<int> order(count);
    std::vector<uint8_t> visited(count);

    for (int iter = 0; iter < iterations; ++iter) {
        for (int i = 0; i < count; ++i) height[i] = m_Bedrock[i] + m_Sediment[i];

        // -- 1. Priority-flood depression filling ---------------------------
        struct Cell {
            float h;
            int idx;
            bool operator>(const Cell& o) const { return h > o.h; }
        };
        std::priority_queue<Cell, std::vector<Cell>, std::greater<Cell>> pq;
        std::fill(visited.begin(), visited.end(), 0);

        for (int x = 0; x < size; ++x) {
            const int top = x, bottom = (size - 1) * size + x;
            if (!visited[top]) { visited[top] = 1; pq.push({height[top], top}); }
            if (!visited[bottom]) { visited[bottom] = 1; pq.push({height[bottom], bottom}); }
        }
        for (int y = 0; y < size; ++y) {
            const int left = y * size, right = y * size + size - 1;
            if (!visited[left]) { visited[left] = 1; pq.push({height[left], left}); }
            if (!visited[right]) { visited[right] = 1; pq.push({height[right], right}); }
        }

        const float epsilon = 1e-4f; // tiny gradient across filled lakes

        std::copy(height.begin(), height.end(), filled.begin());
        while (!pq.empty()) {
            const Cell c = pq.top();
            pq.pop();
            const int cx = c.idx % size;
            const int cy = c.idx / size;
            for (int k = 0; k < 8; ++k) {
                const int nx = cx + dx8[k];
                const int ny = cy + dy8[k];
                if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
                const int ni = ny * size + nx;
                if (visited[ni]) continue;
                visited[ni] = 1;
                filled[ni] = std::max(height[ni], c.h + epsilon);
                pq.push({filled[ni], ni});
            }
        }

        // -- 2. D8 routing on the filled surface ----------------------------
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                const int i = y * size + x;
                float bestDrop = 0.0f;
                int best = -1;
                for (int k = 0; k < 8; ++k) {
                    const int nx = x + dx8[k];
                    const int ny = y + dy8[k];
                    if (nx < 0 || nx >= size || ny < 0 || ny >= size) continue;
                    const int ni = ny * size + nx;
                    const float dist = (k < 4) ? 1.0f : sqrt2;
                    const float drop = (filled[i] - filled[ni]) / dist;
                    if (drop > bestDrop) {
                        bestDrop = drop;
                        best = ni;
                    }
                }
                downstream[i] = best; // -1 only at true outlets on the border
            }
        }

        // -- 3. Flow accumulation, highest cell first -----------------------
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(),
                  [&filled](int a, int b) { return filled[a] > filled[b]; });

        std::fill(area.begin(), area.end(), 1.0f); // each cell contributes one unit of rain
        for (int oi = 0; oi < count; ++oi) {
            const int i = order[oi];
            if (downstream[i] >= 0) area[downstream[i]] += area[i];
        }

        // -- 4. Stream power erosion on the real surface --------------------
        for (int oi = 0; oi < count; ++oi) {
            const int i = order[oi];
            const int d = downstream[i];
            if (d < 0) continue;

            const int xi = i % size, yi = i / size;
            const int xd = d % size, yd = d / size;
            const float dist = ((xi != xd) && (yi != yd)) ? sqrt2 * m_Params.cellSize
                                                          : m_Params.cellSize;

            // Slope on the *actual* surface; filled lakes have ~zero slope
            // and therefore deposit rather than erode.
            const float drop = height[i] - height[d];
            if (drop <= 0.0f) continue;
            const float slope = drop / dist;

            // Fast paths for the standard exponents (m=0.5, n=1) — pow() on
            // a million cells is measurable.
            const float areaTerm = (p.areaExponent == 0.5f)
                ? std::sqrt(area[i]) : std::pow(area[i], p.areaExponent);
            const float slopeTerm = (p.slopeExponent == 1.0f)
                ? slope : std::pow(slope, p.slopeExponent);
            float erode = p.strength * p.erodeConstant * areaTerm * slopeTerm;

            // Never invert the local gradient, never exceed the step cap.
            erode = std::min(erode, drop * 0.8f);
            erode = std::min(erode, p.maxStep);
            if (erode <= 0.0f) continue;

            // Sediment first, then hardness-scaled bedrock.
            float removed = 0.0f;
            const float sed = m_Sediment[i];
            if (sed >= erode) {
                m_Sediment[i] = sed - erode;
                removed = erode;
            } else {
                m_Sediment[i] = 0.0f;
                const float hardness = HardnessAt(height[i]);
                const float bedrockCut = std::min((erode - sed) / hardness, m_Bedrock[i]);
                m_Bedrock[i] -= bedrockCut;
                removed = sed + bedrockCut;
            }

            // Partial re-deposition downstream; the remainder leaves the map.
            m_Sediment[d] += removed * p.depositRatio;

            // Keep `height` coherent for cells processed later this pass.
            height[i] -= removed;
            height[d] += removed * p.depositRatio;
        }

        // -- 5. Write the flow map (log-scaled drainage area) ---------------
        const float logMax = std::log(static_cast<float>(count));
        for (int i = 0; i < count; ++i) {
            const float f = std::log(area[i]) / logMax; // [0, 1]
            m_Flow[i] = std::max(m_Flow[i] * 0.5f, f * 2.0f);
        }
    }
}

} // namespace Titan
