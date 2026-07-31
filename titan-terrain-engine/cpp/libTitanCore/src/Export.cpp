#include "TitanCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#if !defined(__EMSCRIPTEN__)
#include <future>
#endif

// Dependency-free exporters. PNG uses a valid zlib stream built from
// *stored* (uncompressed) deflate blocks — every PNG reader accepts it, and
// heightmaps are usually zipped downstream anyway. EXR is written as
// uncompressed 32-bit float scanlines (compression=NONE), RGB channels
// triplicated for maximum tool compatibility.

namespace Titan {

namespace {

// --- byte helpers -----------------------------------------------------------

void PutU8(std::vector<uint8_t>& out, uint8_t v) { out.push_back(v); }

void PutU32BE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void PutU16LE(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

void PutU32LE(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 24));
}

void PutU64LE(std::vector<uint8_t>& out, uint64_t v) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void PutF32LE(std::vector<uint8_t>& out, float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    PutU32LE(out, v);
}

void PutBytes(std::vector<uint8_t>& out, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + n);
}

void PutCStr(std::vector<uint8_t>& out, const char* s) {
    PutBytes(out, s, std::strlen(s) + 1); // includes NUL
}

// --- checksums ---------------------------------------------------------------

uint32_t Crc32(const uint8_t* data, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

uint32_t Adler32(const uint8_t* data, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// zlib stream from stored deflate blocks.
void ZlibStored(std::vector<uint8_t>& out, const std::vector<uint8_t>& raw) {
    out.push_back(0x78);
    out.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size()) {
        const size_t chunk = std::min<size_t>(65535, raw.size() - pos);
        const bool last = (pos + chunk == raw.size());
        out.push_back(last ? 1 : 0);
        PutU16LE(out, static_cast<uint16_t>(chunk));
        PutU16LE(out, static_cast<uint16_t>(~chunk));
        PutBytes(out, raw.data() + pos, chunk);
        pos += chunk;
    }
    PutU32BE(out, Adler32(raw.data(), raw.size()));
}

void PngChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& payload) {
    PutU32BE(out, static_cast<uint32_t>(payload.size()));
    std::vector<uint8_t> body(type, type + 4);
    body.insert(body.end(), payload.begin(), payload.end());
    PutBytes(out, body.data(), body.size());
    PutU32BE(out, Crc32(body.data(), body.size()));
}

} // namespace

// --- PNG, 16-bit grayscale ----------------------------------------------------

// See the header for why the exported range is not always the surface range.
//
// The test is scale-free and symmetric: a bound is trimmed only when it sits
// more than a quarter of the robust span (p0.1..p99.9) beyond that percentile.
// Measured across realistic stacks, well-behaved terrain sits at 1.03-1.07x its
// p99.9 and is left exactly alone, while a hydraulic pass without a following
// thermal settle sits at 2.0-2.6x and is trimmed. Thermal weathering flattens
// the towers itself, which is why preset stacks ending in it never trip this.
void TerrainEngine::ExportHeightRange(float& outMin, float& outMax) const {
    CollectHeightRange(outMin, outMax);

    const size_t count = m_Bedrock.size();
    // Percentiles are meaningless on a handful of cells.
    if (count < 1024) return;

    std::vector<float> h(count);
    for (size_t i = 0; i < count; ++i) h[i] = m_Bedrock[i] + m_Sediment[i];

    const size_t hiIdx = static_cast<size_t>(static_cast<double>(count) * 0.999);
    const size_t loIdx = static_cast<size_t>(static_cast<double>(count) * 0.001);
    std::nth_element(h.begin(), h.begin() + hiIdx, h.end());
    const float pHi = h[hiIdx];
    std::nth_element(h.begin(), h.begin() + loIdx, h.begin() + hiIdx);
    const float pLo = h[loIdx];

    const float robustSpan = pHi - pLo;
    if (!(robustSpan > 0.0f)) return;
    const float allowance = robustSpan * 0.25f;

    if (outMax - pHi > allowance) outMax = pHi + allowance;
    if (pLo - outMin > allowance) outMin = pLo - allowance;
}

size_t TerrainEngine::ExportPNG16() {
    const int size = m_Params.size;
    float minH, maxH;
    ExportHeightRange(minH, maxH);
    const float range = (maxH - minH) > 0.0f ? (maxH - minH) : 1.0f;

    // Filter byte 0 per row + 2 bytes per pixel, big-endian.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(size) * (size * 2 + 1));
    for (int y = 0; y < size; ++y) {
        raw.push_back(0);
        for (int x = 0; x < size; ++x) {
            const size_t i = static_cast<size_t>(y) * size + x;
            const float h = m_Bedrock[i] + m_Sediment[i];
            // Clamped: a trimmed tail leaves a few cells outside the range,
            // and an unclamped cast would wrap them to the bottom of the map.
            const uint16_t v = static_cast<uint16_t>(std::lround(
                std::clamp((h - minH) / range, 0.0f, 1.0f) * 65535.0f));
            raw.push_back(static_cast<uint8_t>(v >> 8));
            raw.push_back(static_cast<uint8_t>(v));
        }
    }

    m_ExportBuffer.clear();
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    PutBytes(m_ExportBuffer, sig, 8);

    std::vector<uint8_t> ihdr;
    PutU32BE(ihdr, static_cast<uint32_t>(size));
    PutU32BE(ihdr, static_cast<uint32_t>(size));
    PutU8(ihdr, 16); // bit depth
    PutU8(ihdr, 0);  // grayscale
    PutU8(ihdr, 0);  // compression
    PutU8(ihdr, 0);  // filter
    PutU8(ihdr, 0);  // no interlace
    PngChunk(m_ExportBuffer, "IHDR", ihdr);

    std::vector<uint8_t> idat;
    ZlibStored(idat, raw);
    PngChunk(m_ExportBuffer, "IDAT", idat);
    PngChunk(m_ExportBuffer, "IEND", {});

    return m_ExportBuffer.size();
}

namespace {

// Assemble a complete PNG (bit depth 8) from filtered scanlines.
// colorType: 0 = grayscale (1 byte/px), 2 = truecolor RGB (3 bytes/px),
// 6 = RGBA (4 bytes/px).
void WritePNG8(std::vector<uint8_t>& out, int size, int colorType,
               const std::vector<uint8_t>& raw) {
    out.clear();
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    PutBytes(out, sig, 8);

    std::vector<uint8_t> ihdr;
    PutU32BE(ihdr, static_cast<uint32_t>(size));
    PutU32BE(ihdr, static_cast<uint32_t>(size));
    PutU8(ihdr, 8);  // bit depth
    PutU8(ihdr, static_cast<uint8_t>(colorType));
    PutU8(ihdr, 0);  // compression
    PutU8(ihdr, 0);  // filter
    PutU8(ihdr, 0);  // no interlace
    PngChunk(out, "IHDR", ihdr);

    std::vector<uint8_t> idat;
    ZlibStored(idat, raw);
    PngChunk(out, "IDAT", idat);
    PngChunk(out, "IEND", {});
}

} // namespace

// --- Normal map, 8-bit RGB PNG -------------------------------------------------
// World-space normals: R = +X (east), G = +Y (south / +row), B = +Z (up).

size_t TerrainEngine::ExportNormalPNG() {
    const int size = m_Params.size;
    const float c2 = 2.0f * m_Params.cellSize;

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(size) * (size * 3 + 1));
    for (int y = 0; y < size; ++y) {
        raw.push_back(0); // filter byte
        for (int x = 0; x < size; ++x) {
            const float dhdx = (GetHeight(x + 1, y) - GetHeight(x - 1, y)) / c2;
            const float dhdy = (GetHeight(x, y + 1) - GetHeight(x, y - 1)) / c2;
            float nx = -dhdx, ny = -dhdy, nz = 1.0f;
            const float invLen = 1.0f / std::sqrt(nx * nx + ny * ny + nz * nz);
            nx *= invLen; ny *= invLen; nz *= invLen;
            raw.push_back(static_cast<uint8_t>(std::lround((nx * 0.5f + 0.5f) * 255.0f)));
            raw.push_back(static_cast<uint8_t>(std::lround((ny * 0.5f + 0.5f) * 255.0f)));
            raw.push_back(static_cast<uint8_t>(std::lround((nz * 0.5f + 0.5f) * 255.0f)));
        }
    }

    WritePNG8(m_ExportBuffer, size, 2, raw);
    return m_ExportBuffer.size();
}

// --- Ambient occlusion, 8-bit grayscale PNG ------------------------------------
// Horizon-based: sky openness averaged over 8 directions.

size_t TerrainEngine::ExportAOPNG() {
    const int size = m_Params.size;
    ComputeAOField();

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(size) * (size + 1));
    for (int y = 0; y < size; ++y) {
        raw.push_back(0); // filter byte
        for (int x = 0; x < size; ++x) {
            const float ao = m_AO[static_cast<size_t>(y) * size + x];
            raw.push_back(static_cast<uint8_t>(std::lround(
                std::clamp(ao, 0.0f, 1.0f) * 255.0f)));
        }
    }

    WritePNG8(m_ExportBuffer, size, 0, raw);
    return m_ExportBuffer.size();
}

// Horizon-based ambient occlusion.
//
// For each cell, march eight compass directions and record the highest horizon
// angle each one reaches; openness is the mean of the unoccluded fraction. This
// is what gives terrain its readability — valleys darken, ridges catch light —
// and it used to exist only inside the AO exporter, so the viewport shaded
// everything as though it sat on an open plain.
//
// Steps grow geometrically rather than one cell at a time. A linear march
// needed 48 samples per direction to reach any distance worth sampling, which
// is ~100M height lookups on a 512 grid — fine once, in an export the user
// waits for, but far too slow to share with a mesh build. Distant occluders
// subtend small angles and need no fine sampling, so a geometric ladder covers
// the same reach in about a dozen samples with no visible difference.
void TerrainEngine::ComputeAOField() {
    const int size = m_Params.size;
    const size_t count = static_cast<size_t>(size) * size;
    m_AO.assign(count, 1.0f);
    if (size < 2) return;

    const float pi = 3.14159265358979323846f;
    static const int dx8[8] = {-1, 1, 0, 0, -1, 1, -1, 1};
    static const int dy8[8] = {0, 0, -1, 1, -1, -1, 1, 1};

    // Geometric ladder out to a reach that is a fraction of the grid rather
    // than a fixed cell count.
    //
    // What AO should sample is a fixed *world* distance. The terrain spans
    // size * cellSize world units, so a fixed world reach is by definition a
    // fixed fraction of the grid — resolving the same terrain more finely then
    // sweeps the same ground rather than a shrinking sliver of it. The ladder
    // is geometric, so widening the reach eightfold costs about two extra
    // samples per direction.
    const int reach = std::clamp(size / 8, 8, 256);
    std::vector<int> steps;
    for (int s = 1; s < size; s = std::max(s + 1, static_cast<int>(s * 1.45f))) {
        steps.push_back(s);
        if (s >= reach) break;
    }

    auto rows = [&](int yBegin, int yEnd) {
        for (int y = yBegin; y < yEnd; ++y) {
            for (int x = 0; x < size; ++x) {
                const float h0 = GetHeight(x, y);
                float openness = 0.0f;
                for (int d = 0; d < 8; ++d) {
                    const float stepLen = (d < 4 ? 1.0f : 1.41421356f) * m_Params.cellSize;
                    float maxAngle = 0.0f;
                    for (int s : steps) {
                        const int px = x + dx8[d] * s;
                        const int py = y + dy8[d] * s;
                        if (px < 0 || px >= size || py < 0 || py >= size) break;
                        const float rise = GetHeight(px, py) - h0;
                        if (rise <= 0.0f) continue;
                        maxAngle = std::max(maxAngle, std::atan(rise / (stepLen * s)));
                    }
                    openness += 1.0f - maxAngle / (pi * 0.5f);
                }
                m_AO[static_cast<size_t>(y) * size + x] =
                    std::clamp(openness / 8.0f, 0.0f, 1.0f);
            }
        }
    };

#if defined(__EMSCRIPTEN__)
    rows(0, size);
#else
    // Rows are independent and read-only against the heightfield, so the split
    // cannot change the result.
    const int bands = std::min(8, size);
    std::vector<std::future<void>> jobs;
    jobs.reserve(bands);
    for (int b = 0; b < bands; ++b) {
        jobs.push_back(std::async(std::launch::async, rows,
                                  size * b / bands, size * (b + 1) / bands));
    }
    for (auto& j : jobs) j.get();
#endif
}

// --- Splatmap, 8-bit RGBA PNG ---------------------------------------------
// R rock (slope), G normalized height, B flow/wetness, A sediment depth.
// Channels come from SplatAt, the same function that fills the mesh's vertex
// colours, so an exported splatmap always matches the shaded viewport.

size_t TerrainEngine::ExportSplatPNG() {
    const int size = m_Params.size;
    // Same height span the mesh measures against, so the exported masks still
    // match the viewport exactly.
    RefreshSplatRange();

    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(size) * (static_cast<size_t>(size) * 4 + 1));
    for (int y = 0; y < size; ++y) {
        raw.push_back(0); // filter byte
        for (int x = 0; x < size; ++x) {
            float rock, height, flow, sediment;
            SplatAt(x, y, rock, height, flow, sediment);
            raw.push_back(static_cast<uint8_t>(std::lround(rock * 255.0f)));
            raw.push_back(static_cast<uint8_t>(std::lround(height * 255.0f)));
            raw.push_back(static_cast<uint8_t>(std::lround(flow * 255.0f)));
            raw.push_back(static_cast<uint8_t>(std::lround(sediment * 255.0f)));
        }
    }

    WritePNG8(m_ExportBuffer, size, 6, raw); // colour type 6 = RGBA
    return m_ExportBuffer.size();
}

// --- RAW heightmaps -----------------------------------------------------------

size_t TerrainEngine::ExportR16() {
    const int size = m_Params.size;
    float minH, maxH;
    ExportHeightRange(minH, maxH);
    const float range = (maxH - minH) > 0.0f ? (maxH - minH) : 1.0f;

    m_ExportBuffer.clear();
    m_ExportBuffer.reserve(static_cast<size_t>(size) * size * 2);
    for (size_t i = 0; i < m_Bedrock.size(); ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        PutU16LE(m_ExportBuffer, static_cast<uint16_t>(std::lround(
            std::clamp((h - minH) / range, 0.0f, 1.0f) * 65535.0f)));
    }
    return m_ExportBuffer.size();
}

size_t TerrainEngine::ExportR32() {
    m_ExportBuffer.clear();
    m_ExportBuffer.reserve(m_Bedrock.size() * 4);
    for (size_t i = 0; i < m_Bedrock.size(); ++i) {
        PutF32LE(m_ExportBuffer, m_Bedrock[i] + m_Sediment[i]);
    }
    return m_ExportBuffer.size();
}

// --- OpenEXR, uncompressed float RGB -------------------------------------------

size_t TerrainEngine::ExportEXR() {
    const int size = m_Params.size;
    m_ExportBuffer.clear();

    PutU32LE(m_ExportBuffer, 20000630);  // magic
    PutU32LE(m_ExportBuffer, 2);         // version 2, scanline

    auto attr = [&](const char* name, const char* type, const std::vector<uint8_t>& value) {
        PutCStr(m_ExportBuffer, name);
        PutCStr(m_ExportBuffer, type);
        PutU32LE(m_ExportBuffer, static_cast<uint32_t>(value.size()));
        PutBytes(m_ExportBuffer, value.data(), value.size());
    };

    // channels: B, G, R (alphabetical, required), FLOAT.
    std::vector<uint8_t> chlist;
    for (const char* name : {"B", "G", "R"}) {
        PutCStr(chlist, name);
        PutU32LE(chlist, 2); // FLOAT
        PutU8(chlist, 0);    // pLinear
        PutU8(chlist, 0); PutU8(chlist, 0); PutU8(chlist, 0); // reserved
        PutU32LE(chlist, 1); // xSampling
        PutU32LE(chlist, 1); // ySampling
    }
    chlist.push_back(0); // end of list
    attr("channels", "chlist", chlist);

    attr("compression", "compression", {0}); // NONE

    std::vector<uint8_t> box;
    PutU32LE(box, 0); PutU32LE(box, 0);
    PutU32LE(box, static_cast<uint32_t>(size - 1));
    PutU32LE(box, static_cast<uint32_t>(size - 1));
    attr("dataWindow", "box2i", box);
    attr("displayWindow", "box2i", box);

    attr("lineOrder", "lineOrder", {0}); // increasing Y

    std::vector<uint8_t> f;
    PutF32LE(f, 1.0f);
    attr("pixelAspectRatio", "float", f);

    std::vector<uint8_t> v2;
    PutF32LE(v2, 0.0f); PutF32LE(v2, 0.0f);
    attr("screenWindowCenter", "v2f", v2);

    attr("screenWindowWidth", "float", f);

    m_ExportBuffer.push_back(0); // end of header

    // Scanline offset table.
    const size_t tableStart = m_ExportBuffer.size();
    const size_t scanlineBytes = 8 + static_cast<size_t>(size) * 4 * 3;
    const uint64_t dataStart = tableStart + static_cast<uint64_t>(size) * 8;
    for (int y = 0; y < size; ++y) {
        PutU64LE(m_ExportBuffer, dataStart + static_cast<uint64_t>(y) * scanlineBytes);
    }

    // Scanlines: y, byte count, then B row, G row, R row (all same value).
    for (int y = 0; y < size; ++y) {
        PutU32LE(m_ExportBuffer, static_cast<uint32_t>(y));
        PutU32LE(m_ExportBuffer, static_cast<uint32_t>(size * 4 * 3));
        for (int c = 0; c < 3; ++c) {
            for (int x = 0; x < size; ++x) {
                const size_t i = static_cast<size_t>(y) * size + x;
                PutF32LE(m_ExportBuffer, m_Bedrock[i] + m_Sediment[i]);
            }
        }
    }

    return m_ExportBuffer.size();
}

// --- Wavefront OBJ --------------------------------------------------------------

size_t TerrainEngine::ExportOBJ() {
    BuildMesh();
    const int size = m_Params.size;
    const size_t vertexCount = static_cast<size_t>(size) * size;

    m_ExportBuffer.clear();
    m_ExportBuffer.reserve(vertexCount * 96);

    char line[160];
    auto put = [&](int n) { PutBytes(m_ExportBuffer, line, static_cast<size_t>(n)); };

    put(std::snprintf(line, sizeof(line), "# Titan terrain export (%dx%d)\no terrain\n", size, size));

    for (size_t i = 0; i < vertexCount; ++i) {
        put(std::snprintf(line, sizeof(line), "v %.4f %.4f %.4f\n",
                          m_MeshPositions[i * 3], m_MeshPositions[i * 3 + 1], m_MeshPositions[i * 3 + 2]));
    }
    for (size_t i = 0; i < vertexCount; ++i) {
        put(std::snprintf(line, sizeof(line), "vt %.5f %.5f\n",
                          m_MeshUVs[i * 2], 1.0f - m_MeshUVs[i * 2 + 1]));
    }
    for (size_t i = 0; i < vertexCount; ++i) {
        put(std::snprintf(line, sizeof(line), "vn %.4f %.4f %.4f\n",
                          m_MeshNormals[i * 3], m_MeshNormals[i * 3 + 1], m_MeshNormals[i * 3 + 2]));
    }
    for (size_t i = 0; i + 2 < m_MeshIndices.size(); i += 3) {
        const uint32_t a = m_MeshIndices[i] + 1;
        const uint32_t b = m_MeshIndices[i + 1] + 1;
        const uint32_t c = m_MeshIndices[i + 2] + 1;
        put(std::snprintf(line, sizeof(line), "f %u/%u/%u %u/%u/%u %u/%u/%u\n",
                          a, a, a, b, b, b, c, c, c));
    }

    return m_ExportBuffer.size();
}

} // namespace Titan
