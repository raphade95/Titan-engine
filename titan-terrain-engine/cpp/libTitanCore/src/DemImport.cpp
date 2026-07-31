// Real-world elevation import: GeoTIFF / TIFF and SRTM .hgt.
//
// Lives in the engine rather than in each host for the usual reason — one
// decoder, identical results in the web lab, TitanLab and Unreal. It is also
// the only place in the codebase that parses genuinely untrusted third-party
// binary: a DEM is a file a user downloaded from USGS, Copernicus or a
// colleague. Every read below is bounds-checked and every malformed structure
// throws, which the C API's guards turn into titan_last_error rather than a
// crash inside someone's editor.
//
// Deliberately dependency-free, like the exporters. That means a small DEFLATE
// and LZW decoder here; between them plus PackBits and uncompressed they cover
// what DEM publishers actually ship.

#include "TitanCore.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace Titan {

namespace {

[[noreturn]] void Bad(const char* what) {
    throw std::runtime_error(std::string("DEM import: ") + what);
}

// --- Bounds-checked cursor over the file -----------------------------------

struct Reader {
    const uint8_t* data;
    size_t size;
    bool bigEndian = false;

    void Require(size_t off, size_t n) const {
        if (off > size || n > size - off) Bad("truncated file");
    }
    uint8_t U8(size_t off) const { Require(off, 1); return data[off]; }
    uint16_t U16(size_t off) const {
        Require(off, 2);
        return bigEndian ? static_cast<uint16_t>((data[off] << 8) | data[off + 1])
                         : static_cast<uint16_t>(data[off] | (data[off + 1] << 8));
    }
    uint32_t U32(size_t off) const {
        Require(off, 4);
        return bigEndian
            ? (static_cast<uint32_t>(data[off]) << 24 | static_cast<uint32_t>(data[off + 1]) << 16 |
               static_cast<uint32_t>(data[off + 2]) << 8 | data[off + 3])
            : (static_cast<uint32_t>(data[off + 3]) << 24 | static_cast<uint32_t>(data[off + 2]) << 16 |
               static_cast<uint32_t>(data[off + 1]) << 8 | data[off]);
    }
};

// --- DEFLATE (RFC 1951) ------------------------------------------------------
//
// Canonical-Huffman inflate, enough for TIFF's Deflate/AdobeDeflate and for
// zlib-wrapped streams. The engine already writes *stored* deflate for PNG;
// this is the reading half.

struct BitReader {
    const uint8_t* data;
    size_t size;
    size_t pos = 0;
    uint32_t bitBuf = 0;
    int bitCount = 0;

    int Bit() {
        if (bitCount == 0) {
            if (pos >= size) Bad("deflate stream ended early");
            bitBuf = data[pos++];
            bitCount = 8;
        }
        const int b = bitBuf & 1;
        bitBuf >>= 1;
        --bitCount;
        return b;
    }
    uint32_t Bits(int n) {
        uint32_t v = 0;
        for (int i = 0; i < n; ++i) v |= static_cast<uint32_t>(Bit()) << i;
        return v;
    }
    void AlignByte() { bitCount = 0; }
};

struct Huffman {
    // Canonical decoding by walking code lengths, per RFC 1951 section 3.2.2.
    std::vector<uint16_t> counts;   // codes per bit length
    std::vector<uint16_t> symbols;  // symbols ordered by code

    void Build(const uint8_t* lengths, int n) {
        counts.assign(16, 0);
        for (int i = 0; i < n; ++i) counts[lengths[i]]++;
        counts[0] = 0;
        std::vector<uint16_t> offs(16, 0);
        for (int i = 1; i < 16; ++i) offs[i] = static_cast<uint16_t>(offs[i - 1] + counts[i - 1]);
        symbols.assign(static_cast<size_t>(n), 0);
        for (int i = 0; i < n; ++i) {
            if (lengths[i]) symbols[offs[lengths[i]]++] = static_cast<uint16_t>(i);
        }
    }

    int Decode(BitReader& br) const {
        int code = 0, first = 0, index = 0;
        for (int len = 1; len < 16; ++len) {
            code |= br.Bit();
            const int count = counts[len];
            if (code - first < count) return symbols[index + (code - first)];
            index += count;
            first = (first + count) << 1;
            code <<= 1;
        }
        Bad("bad Huffman code");
    }
};

const uint16_t kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,
                               67,83,99,115,131,163,195,227,258};
const uint8_t kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const uint16_t kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,
                                1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const uint8_t kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

void InflateBlocks(BitReader& br, std::vector<uint8_t>& out, size_t limit) {
    Huffman fixedLit, fixedDist;
    {
        uint8_t l[288];
        for (int i = 0; i < 144; ++i) l[i] = 8;
        for (int i = 144; i < 256; ++i) l[i] = 9;
        for (int i = 256; i < 280; ++i) l[i] = 7;
        for (int i = 280; i < 288; ++i) l[i] = 8;
        fixedLit.Build(l, 288);
        uint8_t d[30];
        for (int i = 0; i < 30; ++i) d[i] = 5;
        fixedDist.Build(d, 30);
    }

    for (;;) {
        const int final = br.Bit();
        const uint32_t type = br.Bits(2);

        if (type == 0) { // stored
            br.AlignByte();
            if (br.pos + 4 > br.size) Bad("truncated stored block");
            const uint32_t len = static_cast<uint32_t>(br.data[br.pos]) |
                                 (static_cast<uint32_t>(br.data[br.pos + 1]) << 8);
            br.pos += 4; // LEN + NLEN
            if (br.pos + len > br.size) Bad("truncated stored block");
            if (out.size() + len > limit) Bad("decompressed size exceeds the image");
            out.insert(out.end(), br.data + br.pos, br.data + br.pos + len);
            br.pos += len;
        } else if (type == 1 || type == 2) {
            Huffman litTable, distTable;
            const Huffman* lit = &fixedLit;
            const Huffman* dist = &fixedDist;

            if (type == 2) { // dynamic Huffman
                const int hlit = static_cast<int>(br.Bits(5)) + 257;
                const int hdist = static_cast<int>(br.Bits(5)) + 1;
                const int hclen = static_cast<int>(br.Bits(4)) + 4;
                static const int order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
                uint8_t clen[19] = {0};
                for (int i = 0; i < hclen; ++i) clen[order[i]] = static_cast<uint8_t>(br.Bits(3));
                Huffman clTable;
                clTable.Build(clen, 19);

                std::vector<uint8_t> lens(static_cast<size_t>(hlit + hdist), 0);
                for (int i = 0; i < hlit + hdist;) {
                    const int sym = clTable.Decode(br);
                    if (sym < 16) {
                        lens[i++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        if (i == 0) Bad("bad code-length repeat");
                        const int rep = 3 + static_cast<int>(br.Bits(2));
                        const uint8_t prev = lens[i - 1];
                        for (int k = 0; k < rep && i < hlit + hdist; ++k) lens[i++] = prev;
                    } else if (sym == 17) {
                        i += 3 + static_cast<int>(br.Bits(3));
                    } else {
                        i += 11 + static_cast<int>(br.Bits(7));
                    }
                    if (i > hlit + hdist) Bad("code-length overrun");
                }
                litTable.Build(lens.data(), hlit);
                distTable.Build(lens.data() + hlit, hdist);
                lit = &litTable;
                dist = &distTable;
            }

            for (;;) {
                const int sym = lit->Decode(br);
                if (sym < 256) {
                    if (out.size() >= limit) Bad("decompressed size exceeds the image");
                    out.push_back(static_cast<uint8_t>(sym));
                } else if (sym == 256) {
                    break;
                } else {
                    const int li = sym - 257;
                    if (li < 0 || li >= 29) Bad("bad length symbol");
                    const size_t length = kLenBase[li] + br.Bits(kLenExtra[li]);
                    const int di = dist->Decode(br);
                    if (di < 0 || di >= 30) Bad("bad distance symbol");
                    const size_t distance = kDistBase[di] + br.Bits(kDistExtra[di]);
                    if (distance > out.size()) Bad("distance before start of stream");
                    if (out.size() + length > limit) Bad("decompressed size exceeds the image");
                    const size_t start = out.size() - distance;
                    for (size_t k = 0; k < length; ++k) out.push_back(out[start + k]);
                }
            }
        } else {
            Bad("reserved deflate block type");
        }
        if (final) break;
    }
}

std::vector<uint8_t> Inflate(const uint8_t* src, size_t n, size_t limit) {
    if (n < 1) Bad("empty compressed strip");
    size_t off = 0;
    // Skip a zlib wrapper if present (CMF/FLG with a valid check).
    if (n >= 2 && (src[0] & 0x0f) == 8 && ((src[0] << 8) | src[1]) % 31 == 0) off = 2;
    BitReader br{src + off, n - off};
    std::vector<uint8_t> out;
    out.reserve(std::min<size_t>(limit, 1u << 20));
    InflateBlocks(br, out, limit);
    return out;
}

// --- TIFF LZW (the variant with early code-width change) ---------------------

std::vector<uint8_t> LzwDecode(const uint8_t* src, size_t n, size_t limit) {
    std::vector<uint8_t> out;
    std::vector<std::vector<uint8_t>> table;
    auto reset = [&]() {
        table.clear();
        table.reserve(4096);
        for (int i = 0; i < 256; ++i) table.push_back({static_cast<uint8_t>(i)});
        table.push_back({}); // 256 clear
        table.push_back({}); // 257 EOI
    };
    reset();

    int codeWidth = 9;
    size_t bitPos = 0;
    std::vector<uint8_t> prev;

    auto next = [&]() -> int {
        if ((bitPos + codeWidth) > n * 8) return 257; // treat as EOI
        int v = 0;
        for (int i = 0; i < codeWidth; ++i) {
            const size_t b = bitPos + i;
            v = (v << 1) | ((src[b >> 3] >> (7 - (b & 7))) & 1);
        }
        bitPos += codeWidth;
        return v;
    };

    for (;;) {
        const int code = next();
        if (code == 257) break;
        if (code == 256) { reset(); codeWidth = 9; prev.clear(); continue; }

        std::vector<uint8_t> entry;
        if (code < static_cast<int>(table.size()) && !(code == 256 || code == 257)) {
            entry = table[code];
        } else if (!prev.empty()) {
            entry = prev;
            entry.push_back(prev[0]);
        } else {
            Bad("bad LZW code");
        }

        if (out.size() + entry.size() > limit) Bad("decompressed size exceeds the image");
        out.insert(out.end(), entry.begin(), entry.end());

        if (!prev.empty() && table.size() < 4096) {
            std::vector<uint8_t> added = prev;
            added.push_back(entry[0]);
            table.push_back(std::move(added));
        }
        prev = entry;

        // TIFF's LZW widens one code early relative to the GIF variant.
        if (table.size() + 1 >= (1u << codeWidth) && codeWidth < 12) ++codeWidth;
    }
    return out;
}

std::vector<uint8_t> PackBitsDecode(const uint8_t* src, size_t n, size_t limit) {
    std::vector<uint8_t> out;
    size_t i = 0;
    while (i < n && out.size() < limit) {
        const int8_t hdr = static_cast<int8_t>(src[i++]);
        if (hdr >= 0) {
            const size_t count = static_cast<size_t>(hdr) + 1;
            if (i + count > n) Bad("truncated PackBits run");
            out.insert(out.end(), src + i, src + i + count);
            i += count;
        } else if (hdr != -128) {
            const size_t count = static_cast<size_t>(-hdr) + 1;
            if (i >= n) Bad("truncated PackBits run");
            out.insert(out.end(), count, src[i++]);
        }
    }
    return out;
}

// --- TIFF ---------------------------------------------------------------

struct Ifd {
    uint32_t width = 0, height = 0;
    uint16_t bitsPerSample = 8;
    uint16_t sampleFormat = 1;   // 1 uint, 2 int, 3 float
    uint16_t samplesPerPixel = 1;
    uint16_t compression = 1;
    uint16_t predictor = 1;
    uint32_t rowsPerStrip = 0xffffffffu;
    uint32_t tileWidth = 0, tileHeight = 0;
    std::vector<uint64_t> offsets, byteCounts;
};

uint64_t EntryValue(const Reader& r, size_t entry, int index) {
    const uint16_t type = r.U16(entry + 2);
    const uint32_t count = r.U32(entry + 4);
    if (static_cast<uint32_t>(index) >= count) Bad("tag index out of range");
    const size_t typeSize = (type == 3) ? 2 : (type == 4 || type == 1) ? ((type == 1) ? 1 : 4) : 4;
    const size_t total = typeSize * count;
    const size_t base = (total <= 4) ? entry + 8 : r.U32(entry + 8);
    const size_t at = base + typeSize * static_cast<size_t>(index);
    switch (type) {
        case 1: return r.U8(at);
        case 3: return r.U16(at);
        case 4: return r.U32(at);
        default: return r.U32(at);
    }
}

void ReadArray(const Reader& r, size_t entry, std::vector<uint64_t>& out) {
    const uint32_t count = r.U32(entry + 4);
    if (count > (1u << 22)) Bad("implausible tag count");
    out.resize(count);
    for (uint32_t i = 0; i < count; ++i) out[i] = EntryValue(r, entry, static_cast<int>(i));
}

Ifd ReadIfd(const Reader& r, size_t ifdOff) {
    Ifd ifd;
    const uint16_t entries = r.U16(ifdOff);
    if (entries > 512) Bad("implausible IFD entry count");
    for (uint16_t e = 0; e < entries; ++e) {
        const size_t entry = ifdOff + 2 + static_cast<size_t>(e) * 12;
        const uint16_t tag = r.U16(entry);
        switch (tag) {
            case 256: ifd.width = static_cast<uint32_t>(EntryValue(r, entry, 0)); break;
            case 257: ifd.height = static_cast<uint32_t>(EntryValue(r, entry, 0)); break;
            case 258: ifd.bitsPerSample = static_cast<uint16_t>(EntryValue(r, entry, 0)); break;
            case 259: ifd.compression = static_cast<uint16_t>(EntryValue(r, entry, 0)); break;
            case 277: ifd.samplesPerPixel = static_cast<uint16_t>(EntryValue(r, entry, 0)); break;
            case 278: ifd.rowsPerStrip = static_cast<uint32_t>(EntryValue(r, entry, 0)); break;
            case 317: ifd.predictor = static_cast<uint16_t>(EntryValue(r, entry, 0)); break;
            case 339: ifd.sampleFormat = static_cast<uint16_t>(EntryValue(r, entry, 0)); break;
            case 322: ifd.tileWidth = static_cast<uint32_t>(EntryValue(r, entry, 0)); break;
            case 323: ifd.tileHeight = static_cast<uint32_t>(EntryValue(r, entry, 0)); break;
            case 273: case 324: ReadArray(r, entry, ifd.offsets); break;
            case 279: case 325: ReadArray(r, entry, ifd.byteCounts); break;
            default: break;
        }
    }
    return ifd;
}

float SampleAt(const uint8_t* buf, size_t bufLen, size_t index,
               uint16_t bits, uint16_t format, bool bigEndian) {
    const size_t bytes = bits / 8u;
    const size_t at = index * bytes;
    if (at + bytes > bufLen) Bad("pixel outside decoded strip");
    auto u16 = [&]() -> uint16_t {
        return bigEndian ? static_cast<uint16_t>((buf[at] << 8) | buf[at + 1])
                         : static_cast<uint16_t>(buf[at] | (buf[at + 1] << 8));
    };
    auto u32 = [&]() -> uint32_t {
        return bigEndian
            ? (static_cast<uint32_t>(buf[at]) << 24 | static_cast<uint32_t>(buf[at+1]) << 16 |
               static_cast<uint32_t>(buf[at+2]) << 8 | buf[at+3])
            : (static_cast<uint32_t>(buf[at+3]) << 24 | static_cast<uint32_t>(buf[at+2]) << 16 |
               static_cast<uint32_t>(buf[at+1]) << 8 | buf[at]);
    };
    switch (bits) {
        case 8:  return format == 2 ? static_cast<float>(static_cast<int8_t>(buf[at]))
                                    : static_cast<float>(buf[at]);
        case 16: return format == 2 ? static_cast<float>(static_cast<int16_t>(u16()))
                                    : static_cast<float>(u16());
        case 32: {
            const uint32_t v = u32();
            if (format == 3) { float f; std::memcpy(&f, &v, 4); return f; }
            return format == 2 ? static_cast<float>(static_cast<int32_t>(v))
                               : static_cast<float>(v);
        }
        default: Bad("unsupported bit depth (need 8, 16 or 32)");
    }
}

// Horizontal differencing predictor (TIFF tag 317 = 2), applied per row.
void UndoPredictor(std::vector<uint8_t>& buf, uint32_t rowPixels, uint32_t rows,
                   uint16_t bits, uint16_t spp, bool bigEndian) {
    const size_t bytes = bits / 8u;
    for (uint32_t y = 0; y < rows; ++y) {
        const size_t rowStart = static_cast<size_t>(y) * rowPixels * spp * bytes;
        for (uint32_t x = spp; x < rowPixels * spp; ++x) {
            const size_t cur = rowStart + static_cast<size_t>(x) * bytes;
            const size_t prev = cur - static_cast<size_t>(spp) * bytes;
            if (cur + bytes > buf.size()) return;
            if (bits == 8) {
                buf[cur] = static_cast<uint8_t>(buf[cur] + buf[prev]);
            } else if (bits == 16) {
                auto rd = [&](size_t a) {
                    return bigEndian ? static_cast<uint16_t>((buf[a] << 8) | buf[a + 1])
                                     : static_cast<uint16_t>(buf[a] | (buf[a + 1] << 8));
                };
                const uint16_t v = static_cast<uint16_t>(rd(cur) + rd(prev));
                if (bigEndian) { buf[cur] = v >> 8; buf[cur + 1] = v & 0xff; }
                else { buf[cur] = v & 0xff; buf[cur + 1] = v >> 8; }
            }
        }
    }
}

std::vector<uint8_t> DecodeChunk(const Reader& r, const Ifd& ifd, size_t idx, size_t expected) {
    if (idx >= ifd.offsets.size() || idx >= ifd.byteCounts.size()) Bad("missing strip/tile offset");
    const size_t off = static_cast<size_t>(ifd.offsets[idx]);
    const size_t len = static_cast<size_t>(ifd.byteCounts[idx]);
    r.Require(off, len);
    const uint8_t* src = r.data + off;

    switch (ifd.compression) {
        case 1: return std::vector<uint8_t>(src, src + len);
        case 5: return LzwDecode(src, len, expected);
        case 8: case 32946: return Inflate(src, len, expected);
        case 32773: return PackBitsDecode(src, len, expected);
        default: Bad("unsupported TIFF compression (need none, LZW, Deflate or PackBits)");
    }
}

// Decodes a TIFF/GeoTIFF into a width*height float field of real elevations.
void DecodeTiff(const Reader& r, std::vector<float>& out, int& width, int& height) {
    const uint16_t magic = r.U16(2);
    if (magic != 42) Bad("not a classic TIFF (BigTIFF is not supported)");
    const size_t ifdOff = r.U32(4);
    const Ifd ifd = ReadIfd(r, ifdOff);

    if (ifd.width == 0 || ifd.height == 0) Bad("image has no dimensions");
    if (ifd.width > 32768 || ifd.height > 32768) Bad("image is implausibly large");
    if (ifd.bitsPerSample != 8 && ifd.bitsPerSample != 16 && ifd.bitsPerSample != 32) {
        Bad("unsupported bit depth (need 8, 16 or 32)");
    }

    width = static_cast<int>(ifd.width);
    height = static_cast<int>(ifd.height);
    out.assign(static_cast<size_t>(width) * height, 0.0f);

    const size_t bytes = ifd.bitsPerSample / 8u;
    const uint16_t spp = std::max<uint16_t>(1, ifd.samplesPerPixel);

    if (ifd.tileWidth && ifd.tileHeight) {
        const uint32_t across = (ifd.width + ifd.tileWidth - 1) / ifd.tileWidth;
        const uint32_t down = (ifd.height + ifd.tileHeight - 1) / ifd.tileHeight;
        const size_t expected = static_cast<size_t>(ifd.tileWidth) * ifd.tileHeight * spp * bytes;
        for (uint32_t ty = 0; ty < down; ++ty) {
            for (uint32_t tx = 0; tx < across; ++tx) {
                auto buf = DecodeChunk(r, ifd, static_cast<size_t>(ty) * across + tx, expected);
                if (ifd.predictor == 2) {
                    UndoPredictor(buf, ifd.tileWidth, ifd.tileHeight, ifd.bitsPerSample, spp, r.bigEndian);
                }
                for (uint32_t y = 0; y < ifd.tileHeight; ++y) {
                    const uint32_t gy = ty * ifd.tileHeight + y;
                    if (gy >= ifd.height) break;
                    for (uint32_t x = 0; x < ifd.tileWidth; ++x) {
                        const uint32_t gx = tx * ifd.tileWidth + x;
                        if (gx >= ifd.width) break;
                        const size_t si = (static_cast<size_t>(y) * ifd.tileWidth + x) * spp;
                        out[static_cast<size_t>(gy) * width + gx] =
                            SampleAt(buf.data(), buf.size(), si, ifd.bitsPerSample,
                                     ifd.sampleFormat, r.bigEndian);
                    }
                }
            }
        }
        return;
    }

    const uint32_t rowsPerStrip = std::min(ifd.rowsPerStrip, ifd.height);
    if (rowsPerStrip == 0) Bad("zero rows per strip");
    const uint32_t strips = (ifd.height + rowsPerStrip - 1) / rowsPerStrip;
    for (uint32_t s = 0; s < strips; ++s) {
        const uint32_t rows = std::min(rowsPerStrip, ifd.height - s * rowsPerStrip);
        const size_t expected = static_cast<size_t>(ifd.width) * rows * spp * bytes;
        auto buf = DecodeChunk(r, ifd, s, expected);
        if (ifd.predictor == 2) {
            UndoPredictor(buf, ifd.width, rows, ifd.bitsPerSample, spp, r.bigEndian);
        }
        for (uint32_t y = 0; y < rows; ++y) {
            const uint32_t gy = s * rowsPerStrip + y;
            for (uint32_t x = 0; x < ifd.width; ++x) {
                const size_t si = (static_cast<size_t>(y) * ifd.width + x) * spp;
                out[static_cast<size_t>(gy) * width + x] =
                    SampleAt(buf.data(), buf.size(), si, ifd.bitsPerSample,
                             ifd.sampleFormat, r.bigEndian);
            }
        }
    }
}

// Replace non-finite samples with an average of their finite neighbours,
// spreading inward one ring per pass until the holes close.
//
// Voids are normal in real elevation data: SRTM marks them -32768 (radar
// shadow behind ridges, water it could not range) and float GeoTIFFs use NaN.
// They cannot be left in the field. A literal -32768 is a 32 km pit, and
// zeroing it is only better by accident — in a tile whose valid range is
// 3000-8000 m, a "flat" 0 is still a 3 km hole that erosion will happily
// route every droplet into, and it drags the reported elevation range down
// with it so the user scales their export off a number no ground sample
// produced. A NaN is worse still: it survives the bilinear resample and
// spreads over the neighbourhood of every hole.
//
// Double-buffered so the result cannot depend on traversal order, and the
// pass count is bounded — a void wider than 2*kMaxFillPasses cells keeps a
// flat core at the mean, which is honest about there being no data there.
void FillVoids(std::vector<float>& field, int width, int height) {
    constexpr int kMaxFillPasses = 256;

    std::vector<size_t> holes;
    double sum = 0.0;
    size_t valid = 0;
    for (size_t i = 0; i < field.size(); ++i) {
        if (std::isfinite(field[i])) {
            sum += field[i];
            ++valid;
        } else {
            holes.push_back(i);
        }
    }
    if (holes.empty() || valid == 0) return;

    std::vector<float> next;
    std::vector<size_t> remaining;
    for (int pass = 0; pass < kMaxFillPasses && !holes.empty(); ++pass) {
        next = field;
        remaining.clear();
        for (size_t idx : holes) {
            const int x = static_cast<int>(idx % static_cast<size_t>(width));
            const int y = static_cast<int>(idx / static_cast<size_t>(width));
            float acc = 0.0f;
            int n = 0;
            const int dx[4] = {-1, 1, 0, 0};
            const int dy[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; ++k) {
                const int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= width || ny >= height) continue;
                const float v = field[static_cast<size_t>(ny) * width + nx];
                if (!std::isfinite(v)) continue;
                acc += v;
                ++n;
            }
            if (n > 0) {
                next[idx] = acc / static_cast<float>(n);
            } else {
                remaining.push_back(idx);
            }
        }
        field.swap(next);
        holes.swap(remaining);
    }

    const auto mean = static_cast<float>(sum / static_cast<double>(valid));
    for (size_t idx : holes) field[idx] = mean;
}

// SRTM .hgt: raw big-endian int16, square, no header at all — the side is
// inferred from the file length (1201 for 3-arc-second, 3601 for 1-arc-second).
bool DecodeHgt(const Reader& r, std::vector<float>& out, int& width, int& height) {
    const size_t samples = r.size / 2;
    const auto side = static_cast<size_t>(std::llround(std::sqrt(static_cast<double>(samples))));

    // .hgt has no magic number, no header, nothing — the format *is* "the file
    // length is 2n^2". That makes it impossible to identify positively, so
    // this is only ever reached after TIFF detection fails, and a plausible
    // minimum side keeps small unrelated files from being read as elevation:
    // nine bytes of junk is arithmetically a valid 2x2 DEM. Real SRTM tiles
    // are 1201 or 3601 a side; 32 is far below anything genuine while still
    // admitting hand-made test data.
    constexpr size_t kMinHgtSide = 32;
    if (side < kMinHgtSide || side * side != samples) return false;

    width = height = static_cast<int>(side);
    out.assign(samples, 0.0f);
    for (size_t i = 0; i < samples; ++i) {
        const int16_t v = static_cast<int16_t>((r.data[i * 2] << 8) | r.data[i * 2 + 1]);
        // -32768 is SRTM's void marker. Mark it non-finite and let FillVoids
        // interpolate it from the surrounding terrain; taking the value
        // literally would punch a 32 km pit through the tile.
        out[i] = (v == -32768) ? std::numeric_limits<float>::quiet_NaN()
                               : static_cast<float>(v);
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

int TerrainEngine::DecodeDem(const uint8_t* data, size_t bytes) {
    if (!data || bytes < 8) Bad("file is too small to be a DEM");

    Reader r{data, bytes};
    std::vector<float> field;
    int width = 0, height = 0;

    if (data[0] == 'I' && data[1] == 'I') {
        r.bigEndian = false;
        DecodeTiff(r, field, width, height);
    } else if (data[0] == 'M' && data[1] == 'M') {
        r.bigEndian = true;
        DecodeTiff(r, field, width, height);
    } else if (!DecodeHgt(r, field, width, height)) {
        Bad("unrecognized format (expected a TIFF/GeoTIFF or a square SRTM .hgt)");
    }

    if (field.empty() || width <= 0 || height <= 0) Bad("no usable samples in the file");
    FillVoids(field, width, height);

    // Real elevations, before any normalization — this is what a user needs to
    // set a correct vertical scale, and it is the whole point of importing a
    // real-world DEM rather than an arbitrary greyscale image.
    float lo = 1e30f, hi = -1e30f;
    for (float v : field) {
        if (!std::isfinite(v)) continue;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    if (lo > hi) Bad("no usable samples in the file");
    m_DemMin = lo;
    m_DemMax = hi;
    m_DemSourceWidth = width;
    m_DemSourceHeight = height;

    // Resample to a square, matching what the existing image importer already
    // does — Titan's terrain is square, so a rectangular DEM is stretched
    // rather than cropped. Hosts report the source dimensions so the user can
    // see that the aspect was normalized.
    const int side = std::max(width, height);
    m_Dem.assign(static_cast<size_t>(side) * side, 0.0f);
    const float span = (hi - lo) > 0.0f ? (hi - lo) : 1.0f;
    for (int y = 0; y < side; ++y) {
        const float sy = (side > 1) ? static_cast<float>(y) * (height - 1) / (side - 1) : 0.0f;
        const int y0 = std::clamp(static_cast<int>(sy), 0, height - 1);
        const int y1 = std::min(y0 + 1, height - 1);
        const float fy = sy - static_cast<float>(y0);
        for (int x = 0; x < side; ++x) {
            const float sx = (side > 1) ? static_cast<float>(x) * (width - 1) / (side - 1) : 0.0f;
            const int x0 = std::clamp(static_cast<int>(sx), 0, width - 1);
            const int x1 = std::min(x0 + 1, width - 1);
            const float fx = sx - static_cast<float>(x0);
            const float v =
                field[static_cast<size_t>(y0) * width + x0] * (1 - fx) * (1 - fy) +
                field[static_cast<size_t>(y0) * width + x1] * fx * (1 - fy) +
                field[static_cast<size_t>(y1) * width + x0] * (1 - fx) * fy +
                field[static_cast<size_t>(y1) * width + x1] * fx * fy;
            // Normalized 0..1, the contract ApplyHeightfield expects.
            m_Dem[static_cast<size_t>(y) * side + x] = (v - lo) / span;
        }
    }
    return side;
}

} // namespace Titan
