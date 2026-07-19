#include "TitanCore.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

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

size_t TerrainEngine::ExportPNG16() {
    const int size = m_Params.size;
    float minH, maxH;
    CollectHeightRange(minH, maxH);
    const float range = (maxH - minH) > 0.0f ? (maxH - minH) : 1.0f;

    // Filter byte 0 per row + 2 bytes per pixel, big-endian.
    std::vector<uint8_t> raw;
    raw.reserve(static_cast<size_t>(size) * (size * 2 + 1));
    for (int y = 0; y < size; ++y) {
        raw.push_back(0);
        for (int x = 0; x < size; ++x) {
            const size_t i = static_cast<size_t>(y) * size + x;
            const float h = m_Bedrock[i] + m_Sediment[i];
            const uint16_t v = static_cast<uint16_t>(
                std::lround(((h - minH) / range) * 65535.0f));
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

// --- RAW heightmaps -----------------------------------------------------------

size_t TerrainEngine::ExportR16() {
    const int size = m_Params.size;
    float minH, maxH;
    CollectHeightRange(minH, maxH);
    const float range = (maxH - minH) > 0.0f ? (maxH - minH) : 1.0f;

    m_ExportBuffer.clear();
    m_ExportBuffer.reserve(static_cast<size_t>(size) * size * 2);
    for (size_t i = 0; i < m_Bedrock.size(); ++i) {
        const float h = m_Bedrock[i] + m_Sediment[i];
        PutU16LE(m_ExportBuffer, static_cast<uint16_t>(
            std::lround(((h - minH) / range) * 65535.0f)));
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
