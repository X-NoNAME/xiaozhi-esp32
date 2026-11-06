#include "assets_png.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

#define TAG "AssetsPng"

namespace {

constexpr uint32_t kDirMagic = 0x31434E50;  // bytes 'P','N','C','1'
constexpr uint16_t kDirVersion = 1;
constexpr size_t kSectorSize = 4096;
constexpr int kMaxSources = 24;  // typical packs carry <= 24 emoji PNGs
constexpr size_t kMaxWriteChunk = 512;
constexpr uint16_t kMaxDimension = 128;  // safe: blob <= 48 KB, raw stream <= 66 KB

// Same layout as Assets::mmap_assets_table (assets.cc): 12-byte pack header
// [file_count][checksum][stored_len], then file-count table entries of
// 44 bytes each, then the data area. Every stored file starts with 'Z''Z'.
struct AssetTableEntry {
    char name[32];
    uint32_t size;
    uint32_t offset;
    uint16_t width;
    uint16_t height;
};
static_assert(sizeof(AssetTableEntry) == 44, "assets table entry layout changed");

struct DirEntry {
    char name[28];
    uint32_t crc;  // CRC32 of the PNG payload (bytes after the 'ZZ' prefix)
    uint16_t w;
    uint16_t h;
    uint32_t blob_off;  // absolute offset inside the assets partition
    uint32_t blob_len;  // total bytes: rgb565 plane + a8 plane
    uint32_t blob_crc;  // CRC32 of the stored blob
};
static_assert(sizeof(DirEntry) == 48, "png cache dir entry layout");

struct DirHeader {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    uint32_t stored_len;  // stored_len of the pack this directory was built for
    uint32_t reserved;
};
static_assert(sizeof(DirHeader) == 16, "png cache dir header layout");

struct SourceFile {
    char name[28];
    const uint8_t* data;
    size_t size;
    uint16_t w;
    uint16_t h;
    uint32_t crc;
    uint32_t raw_bytes;       // padded-filter rows bytes: (w*4+1)*h
    uint32_t blob_bytes;      // w*h*3
    uint32_t blob_occupancy;  // RoundUp4K(blob_bytes)
    uint32_t blob_off;        // absolute offset in the partition
};

// The registry keeps only fixed-size image descriptors; names and layout are
// read back from the (still memory-mapped) flash directory on demand, so the
// steady-state RAM cost of this module stays ≈ 700 bytes plus small headers.
constexpr int kMaxRegistrySlots = kMaxSources;

struct RegistrySlot {
    lv_img_dsc_t dsc;
    bool filled;
};

RegistrySlot registry[kMaxRegistrySlots];
const uint8_t* registry_mmap_root = nullptr;  // partition map base
const uint8_t* registry_entries = nullptr;    // mmap pointer to the first DirEntry
int registry_count = 0;
bool registry_ready = false;

uint32_t ReadLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint32_t ReadBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

uint32_t RoundUp4K(uint32_t v) {
    return (v + kSectorSize - 1) & ~static_cast<uint32_t>(kSectorSize - 1);
}

// Fills slot `index` from its flash directory entry (idempotent).
bool PrepareRegistrySlot(int index) {
    RegistrySlot& slot = registry[index];
    if (slot.filled) {
        return true;
    }
    const uint8_t* ep = registry_entries + static_cast<size_t>(index) * sizeof(DirEntry);
    const uint16_t w = static_cast<uint16_t>(ep[32] | (ep[33] << 8));
    const uint16_t h = static_cast<uint16_t>(ep[34] | (ep[35] << 8));
    const uint32_t blob_off = ReadLE32(ep + 36);
    const uint32_t blob_len = ReadLE32(ep + 40);
    if (blob_len != static_cast<uint32_t>(w) * h * 3u) {
        return false;
    }
    lv_img_dsc_t& dsc = slot.dsc;
    std::memset(&dsc, 0, sizeof(dsc));
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
    dsc.header.w = w;
    dsc.header.h = h;
    dsc.header.stride = static_cast<uint16_t>(w * 2);
    dsc.data_size = blob_len;
    dsc.data = registry_mmap_root + blob_off;
    slot.filled = true;
    return true;
}

// Points the registry at a validated/just-written directory (its entries stay
// inside the mmap). Individual descriptors are built lazily in AssetsPngGet.
void ActivateRegistry(const uint8_t* mmap_root, size_t entries_offset, int count) {
    registry_mmap_root = mmap_root;
    registry_entries = mmap_root + entries_offset;
    registry_count = count;
    registry_ready = true;
    for (int i = 0; i < kMaxRegistrySlots; i++) {
        registry[i].filled = false;
    }
}

// ---------------------------------------------------------------------------
// PNG inspection: only 8-bit RGBA (color type 6), no interlace, width multiple
// of 4 is converted; anything else keeps the previous fallback behaviour.

bool PngIsSupported(const uint8_t* data, size_t size, uint16_t* out_w, uint16_t* out_h) {
    if (size < 33 || size > (100u * 1024u) || std::memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) {
        return false;
    }
    const uint32_t w = ReadBE32(data + 16);
    const uint32_t h = ReadBE32(data + 20);
    if (w == 0 || h == 0 || w > kMaxDimension || h > kMaxDimension || (w % 4) != 0) {
        return false;
    }
    if (data[24] != 8 || data[25] != 6 || data[28] != 0) {
        return false;
    }
    *out_w = static_cast<uint16_t>(w);
    *out_h = static_cast<uint16_t>(h);
    return true;
}

bool NameEndsWithPng(const char* name) {
    const size_t len = std::strlen(name);
    return len > 4 && std::strcmp(name + len - 4, ".png") == 0;
}

// ---------------------------------------------------------------------------
// Minimal Deflate (RFC 1951) decoder. Output streams to flash; the sliding
// history window is satisfied by reading back the region this run wrote via
// esp_partition_read (always fresh; bypasses the mmap cache), so no RAM
// window is needed. Only a few hundred bytes of RAM are used.

constexpr int kMaxFragments = 8;

struct FragChain {
    struct Frag {
        const uint8_t* data;
        size_t len;
    };
    Frag frags[kMaxFragments];
    int count;

    FragChain() : count(0) {}
    bool Add(const uint8_t* data, size_t len) {
        if (count >= kMaxFragments || data == nullptr || len == 0) {
            return false;
        }
        frags[count++] = {data, len};
        return true;
    }
};

class BitReader {
public:
    explicit BitReader(const FragChain& chain) : chain_(chain) {}

    bool SkipBytes(int n) {
        for (int i = 0; i < n; i++) {
            uint32_t b;
            if (NextByte(&b) != 0) {
                return false;
            }
        }
        return true;
    }

    int ReadBits(int count, uint32_t* out) {
        while (bit_count_ < count) {
            uint32_t byte;
            if (NextByte(&byte) != 0) {
                return -1;
            }
            bit_buf_ |= byte << bit_count_;
            bit_count_ += 8;
        }
        uint32_t value = bit_buf_ & ((1u << count) - 1);
        bit_buf_ >>= count;
        bit_count_ -= count;
        *out = value;
        return 0;
    }

    int ReadOneBit(uint32_t* out) { return ReadBits(1, out); }

    // Already read bits are discarded; NextByte proceeds at a byte boundary.
    void AlignToByte() {
        bit_buf_ = 0;
        bit_count_ = 0;
    }

private:
    int NextByte(uint32_t* out) {
        if (frag_index_ >= chain_.count) {
            return -1;
        }
        const auto& frag = chain_.frags[frag_index_];
        if (frag_off_ >= frag.len) {
            frag_index_++;
            frag_off_ = 0;
            if (frag_index_ >= chain_.count) {
                return -1;
            }
            return NextByte(out);
        }
        *out = frag.data[frag_off_++];
        return 0;
    }

    const FragChain& chain_;
    int frag_index_ = 0;
    size_t frag_off_ = 0;
    uint32_t bit_buf_ = 0;
    int bit_count_ = 0;
};

struct HuffmanTable {
    uint16_t counts[16];
    uint16_t symbols[320] = {};
};

int BuildHuffman(HuffmanTable* table, const uint8_t* lengths, int n) {
    for (int i = 0; i < 16; i++) {
        table->counts[i] = 0;
    }
    for (int i = 0; i < n; i++) {
        table->counts[lengths[i]]++;
    }
    if (table->counts[0] == n) {
        return -1;  // no codes
    }
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= table->counts[len];
        if (left < 0) {
            return -1;
        }
    }
    uint16_t offsets[16];
    offsets[1] = 0;
    for (int len = 1; len < 15; len++) {
        offsets[len + 1] = offsets[len] + table->counts[len];
    }
    for (int i = 0; i < n; i++) {
        if (lengths[i]) {
            table->symbols[offsets[lengths[i]]++] = static_cast<uint16_t>(i);
        }
    }
    return 0;
}

int DecodeSymbol(BitReader& br, const HuffmanTable& table) {
    uint32_t code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= 15; len++) {
        uint32_t bit;
        if (br.ReadOneBit(&bit) != 0) {
            return -1;
        }
        code |= bit;
        const int count = table.counts[len];
        if (code - first < static_cast<uint32_t>(count)) {
            return table.symbols[index + (code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

constexpr uint16_t kLenBase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                   31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                   2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr uint16_t kDistBase[30] = {1,    2,    3,    4,    5,    7,    9,    13,    17,    25,
                                    33,   49,   65,   97,   129,  193,  257,  385,   513,   769,
                                    1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
constexpr uint8_t kCodeLenOrder[19] = {16, 17, 18, 0, 8,  7, 9,  6, 10, 5,
                                       11, 4,  12, 3, 13, 2, 14, 1, 15};

// Streams decoded bytes to the assets partition, 4-byte aligned writes.
class FlashWriter {
public:
    void Setup(const esp_partition_t* part, size_t dst_base) {
        part_ = part;
        base_ = dst_base;
        written_ = 0;
        buffered_ = 0;
        failed_ = false;
    }

    void WriteByte(uint8_t byte) {
        if (failed_) {
            return;
        }
        buffer_[buffered_++] = byte;
        logical_++;
        if (buffered_ == kMaxWriteChunk) {
            Flush();
        }
    }

    void CopyBytes(const uint8_t* src, size_t len) {
        for (size_t i = 0; i < len; i++) {
            WriteByte(src[i]);
        }
    }

    void Flush() {
        // Streamed flush: emit only complete 4-byte groups, keep the tail in
        // the buffer. No padding here: any pad byte would shift every later
        // write and corrupt the deflate history window.
        if (failed_ || buffered_ < 4) {
            return;
        }
        const size_t group = buffered_ & ~static_cast<size_t>(3);
        const size_t dst = base_ + written_;
        if ((dst & 3) != 0 || esp_partition_write(part_, dst, buffer_, group) != ESP_OK) {
            failed_ = true;
            return;
        }
        written_ += group;
        std::memmove(buffer_, buffer_ + group, buffered_ - group);
        buffered_ -= group;
    }

    // Final flush at the logical end of the stream. The 1-3 pad bytes go past
    // the logical stream end into the pre-erased temp area, where pass 2 never
    // reads them, so they are harmless.
    void FlushFinal() {
        if (failed_) {
            return;
        }
        while ((buffered_ & 3) != 0) {
            buffer_[buffered_++] = 0;
        }
        if (buffered_ > 0) {
            const size_t dst = base_ + written_;
            if ((dst & 3) != 0 || esp_partition_write(part_, dst, buffer_, buffered_) != ESP_OK) {
                failed_ = true;
                return;
            }
            written_ += buffered_;
            buffered_ = 0;
        }
    }

    // Copies the `len` logical bytes located `dist` before the current head
    // into dst. The first `written_` logical bytes live in flash, the unsent
    // tail still sits in the RAM buffer.
    bool ReadBefore(size_t dist, uint8_t* dst, size_t len) {
        if (dist == 0 || logical_ < dist || logical_ < len) {
            return false;
        }
        const size_t start = logical_ - dist;
        const size_t end = start + len;
        size_t pos = start;
        const size_t flash_end = std::min(end, written_);
        while (pos < flash_end) {
            const size_t n = std::min<size_t>(flash_end - pos, kMaxWriteChunk);
            if (esp_partition_read(part_, base_ + pos, dst + (pos - start), n) != ESP_OK) {
                return false;
            }
            pos += n;
        }
        if (pos < end) {
            std::memcpy(dst + (pos - start), buffer_ + (pos - written_), end - pos);
        }
        return true;
    }

    // Logical stream position (padding bytes are not part of it).
    size_t Written() const { return logical_; }
    bool Failed() const { return failed_; }
    const esp_partition_t* Partition() const { return part_; }
    size_t DstBase() const { return base_; }

private:
    const esp_partition_t* part_ = nullptr;
    size_t base_ = 0;
    size_t written_ = 0;
    size_t buffered_ = 0;
    size_t logical_ = 0;
    bool failed_ = false;
    uint8_t buffer_[kMaxWriteChunk];
};

// History window of the deflate stream that is written by `writer`. Must copy
// with LZ77 repetition semantics (a copy may read bytes the copy itself
// has already produced when dist < len).
bool FlashWindowCopy(FlashWriter& writer, size_t dist, uint8_t* scratch, size_t scratch_len,
                     size_t len) {
    if (writer.Failed() || dist == 0 || writer.Written() < dist) {
        return false;
    }
    size_t remaining = len;
#ifdef ASSETS_PNG_DEBUG
    printf("copy start: dist=%zu len=%zu pos=%zu\n", dist, len, writer.Written());
#endif
    while (remaining > 0) {
        size_t chunk = std::min(dist, remaining);
        chunk = std::min(chunk, scratch_len);
#ifdef ASSETS_PNG_DEBUG
        printf("   hw pos=%zu dist=%zu chunk=%zu\n", writer.Written(), dist, chunk);
#endif
        if (!writer.ReadBefore(dist, scratch, chunk)) {
            return false;
        }
        for (size_t i = 0; i < chunk; i++) {
            writer.WriteByte(scratch[i]);
        }
        remaining -= chunk;
    }
    return !writer.Failed();
}

// Turns the PNG payload into padded raw RGBA scan lines written at
// `temp_base`. Returns the number of bytes produced, 0 on failure.
size_t InflatePngRawRows(const esp_partition_t* part, size_t temp_base, const uint8_t* png,
                         size_t png_size, uint16_t w, uint16_t h) {
    // 1) Collect IDAT fragments.
    FragChain chain;
    size_t pos = 8;
    while (pos + 8 <= png_size) {
        const uint32_t len = ReadBE32(png + pos);
        if (pos + 12 + static_cast<size_t>(len) > png_size) {
            return 0;
        }
        const uint8_t* type = png + pos + 4;
        if (std::memcmp(type, "IDAT", 4) == 0) {
            if (!chain.Add(png + pos + 8, len)) {
                return 0;  // too many IDAT chunks
            }
        } else if (std::memcmp(type, "IEND", 4) == 0) {
            break;
        }
        pos += 12 + len;
    }
    if (chain.count == 0) {
        return 0;
    }

    BitReader br(chain);
    if (!br.SkipBytes(2)) {  // zlib CMF/FLG
        return 0;
    }

    const size_t expected = (static_cast<size_t>(w) * 4 + 1) * h;
    FlashWriter writer;
    writer.Setup(part, temp_base);

    HuffmanTable lit_tree{};
    HuffmanTable dist_tree{};
    bool have_trees = false;

    for (;;) {
        uint32_t final_block, block_type;
        if (br.ReadBits(1, &final_block) != 0 || br.ReadBits(2, &block_type) != 0) {
            return 0;
        }
        if (block_type == 3) {
            return 0;
        }
#ifdef ASSETS_PNG_DEBUG
        printf("block: type=%u final=%u written=%zu\n", block_type, final_block, writer.Written());
#endif
        if (block_type == 0) {  // stored
            br.AlignToByte();
            uint32_t lo, hi, nlen_lo, nlen_hi;
            if (br.ReadBits(8, &lo) != 0 || br.ReadBits(8, &hi) != 0 ||
                br.ReadBits(8, &nlen_lo) != 0 || br.ReadBits(8, &nlen_hi) != 0) {
                return 0;
            }
            const uint32_t stored_len = lo | (hi << 8);
            (void)nlen_lo;
            (void)nlen_hi;
            for (uint32_t i = 0; i < stored_len; i++) {
                uint32_t b;
                if (br.ReadBits(8, &b) != 0) {
                    return 0;
                }
                writer.WriteByte(static_cast<uint8_t>(b));
            }
            have_trees = false;
        } else {
            if (block_type == 1) {  // fixed Huffman
                uint8_t lit_lengths[288];
                for (int i = 0; i < 144; i++) {
                    lit_lengths[i] = 8;
                }
                for (int i = 144; i < 256; i++) {
                    lit_lengths[i] = 9;
                }
                for (int i = 256; i < 280; i++) {
                    lit_lengths[i] = 7;
                }
                for (int i = 280; i < 288; i++) {
                    lit_lengths[i] = 8;
                }
                uint8_t dist_lengths[30] = {5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
                                            5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5};
                if (BuildHuffman(&lit_tree, lit_lengths, 288) != 0 ||
                    BuildHuffman(&dist_tree, dist_lengths, 30) != 0) {
                    return 0;
                }
                have_trees = true;
            } else {  // dynamic Huffman
                uint32_t hlit, hdist, hclen;
                if (br.ReadBits(5, &hlit) != 0 || br.ReadBits(5, &hdist) != 0 ||
                    br.ReadBits(4, &hclen) != 0) {
                    return 0;
                }
                hlit += 257;
                hdist += 1;
                hclen += 4;
                if (hlit > 286 || hdist > 30) {
                    return 0;
                }
                uint8_t cl_lengths[19] = {};
                for (uint32_t i = 0; i < hclen; i++) {
                    uint32_t v;
                    if (br.ReadBits(3, &v) != 0) {
                        return 0;
                    }
                    cl_lengths[kCodeLenOrder[i]] = static_cast<uint8_t>(v);
                }
                HuffmanTable cl_tree{};
                if (BuildHuffman(&cl_tree, cl_lengths, 19) != 0) {
                    return 0;
                }
                uint8_t lengths[286 + 30] = {};
                size_t idx = 0;
                while (idx < hlit + hdist) {
                    int sym = DecodeSymbol(br, cl_tree);
                    if (sym < 0) {
                        return 0;
                    }
                    if (sym < 16) {
                        lengths[idx++] = static_cast<uint8_t>(sym);
                    } else if (sym == 16) {
                        if (idx == 0) {
                            return 0;
                        }
                        uint32_t rep;
                        if (br.ReadBits(2, &rep) != 0) {
                            return 0;
                        }
                        rep += 3;
                        const uint8_t prev = lengths[idx - 1];
                        while (rep-- > 0 && idx < 316) {
                            lengths[idx++] = prev;
                        }
                    } else {
                        uint32_t rep;
                        if (br.ReadBits(sym == 17 ? 3 : 7, &rep) != 0) {
                            return 0;
                        }
                        rep += (sym == 17) ? 3 : 11;
                        while (rep-- > 0 && idx < 316) {
                            lengths[idx++] = 0;
                        }
                    }
                    if (idx > hlit + hdist) {
                        return 0;
                    }
                }
                if (lengths[256] == 0) {
                    return 0;  // end-of-block code must exist
                }
                if (BuildHuffman(&lit_tree, lengths, hlit) != 0 ||
                    BuildHuffman(&dist_tree, lengths + hlit, hdist) != 0) {
                    return 0;
                }
                have_trees = true;
            }

            // Decode one block.
            while (have_trees) {
                int sym = DecodeSymbol(br, lit_tree);
                if (sym < 0) {
                    return 0;
                }
                if (sym < 256) {
                    writer.WriteByte(static_cast<uint8_t>(sym));
                    continue;
                }
                if (sym == 256) {
                    break;  // block end
                }
                if (sym > 285) {
                    return 0;
                }
                const int length_code = sym - 257;
                uint32_t extra;
                if (br.ReadBits(kLenExtra[length_code], &extra) != 0) {
                    return 0;
                }
                const size_t length = kLenBase[length_code] + extra;
                const int dist_code = DecodeSymbol(br, dist_tree);
                if (dist_code < 0 || dist_code >= 30) {
                    return 0;
                }
                if (br.ReadBits(kDistExtra[dist_code], &extra) != 0) {
                    return 0;
                }
                const size_t dist = kDistBase[dist_code] + extra;
                uint8_t scratch[kMaxWriteChunk];
                if (!FlashWindowCopy(writer, dist, scratch, sizeof(scratch), length)) {
                    return 0;
                }
            }
            have_trees = false;
        }
        if (writer.Failed()) {
            return 0;
        }
        if (final_block) {
            break;
        }
    }
    writer.FlushFinal();
#ifdef ASSETS_PNG_DEBUG
    printf("raw stream written=%zu expected=%zu failed=%d\n", writer.Written(), expected,
           writer.Failed());
#endif
    if (writer.Failed() || writer.Written() != expected) {
        return 0;
    }
    return expected;
}

// ---------------------------------------------------------------------------
// Pass 2: read the padded raw rows back from flash, unfilter, convert to
// RGB565 + A8 planes and store them in the blob area.

// Working buffers of pass 2, allocated in RAM only while a conversion is
// running, so the steady-state RAM cost of the module stays small.
struct BlobConvBuf {
    uint8_t prev[kMaxDimension * 4];
    uint8_t cur[kMaxDimension * 4];
    uint8_t rgb_acc[kMaxWriteChunk * 2];
    uint8_t a8_acc[kMaxWriteChunk];
    uint8_t read_buf[kMaxDimension * 4 + 8];
    uint8_t verify_buf[256];
};

bool ConvertRawRowsToBlob(const esp_partition_t* part, size_t temp_base, const SourceFile& src,
                          uint32_t blob_base, BlobConvBuf& buf, uint32_t* out_blob_crc) {
    const size_t row_stride = static_cast<size_t>(src.w) * 4 + 1;
    const size_t raw_stride = static_cast<size_t>(src.w) * 4;
    const uint32_t a8_off = blob_base + static_cast<uint32_t>(src.w) * src.h * 2;

    uint8_t* prev = buf.prev;
    uint8_t* cur = buf.cur;
    uint8_t* rgb_acc = buf.rgb_acc;
    uint8_t* a8_acc = buf.a8_acc;
    uint8_t* read_buf = buf.read_buf;
    std::memset(prev, 0, sizeof(buf.prev));

    size_t rgb_pending = 0;  // bytes of RGB565 in rgb_acc
    size_t a8_pending = 0;
    uint32_t rgb_written = 0;
    uint32_t a8_written = 0;

    auto flush_rgb = [&]() -> bool {
        if (rgb_pending == 0) {
            return true;
        }
        if ((rgb_pending & 3) != 0) {
            while ((rgb_pending & 3) != 0) {
                rgb_acc[rgb_pending++] = 0;
            }
        }
        if (esp_partition_write(part, blob_base + rgb_written, rgb_acc, rgb_pending) != ESP_OK) {
            return false;
        }
        rgb_written += rgb_pending;
        rgb_pending = 0;
        return true;
    };
    auto flush_a8 = [&]() -> bool {
        if (a8_pending == 0) {
            return true;
        }
        while ((a8_pending & 3) != 0) {
            a8_acc[a8_pending++] = 0;
        }
        if (esp_partition_write(part, a8_off + a8_written, a8_acc, a8_pending) != ESP_OK) {
            return false;
        }
        a8_written += a8_pending;
        a8_pending = 0;
        return true;
    };

    for (uint32_t y = 0; y < src.h; y++) {
        // The raw stream may be non-aligned; read a slightly larger aligned window.
        const size_t row_off = temp_base + static_cast<size_t>(y) * row_stride;
        const size_t aligned = row_off & ~3UL;
        const size_t pad = row_off - aligned;
        if (pad + row_stride > sizeof(buf.read_buf)) {
            return false;
        }
        const size_t want = (pad + row_stride + 3) & ~3UL;
        if (esp_partition_read(part, aligned, read_buf, want) != ESP_OK) {
            return false;
        }
        std::memcpy(cur, read_buf + pad + 1, raw_stride);  // first byte = filter type
        const uint8_t filter = read_buf[pad];
        if (filter <= 4) {
            if (filter == 1) {  // Sub
                for (size_t i = 4; i < raw_stride; i++) {
                    cur[i] = static_cast<uint8_t>(cur[i] + cur[i - 4]);
                }
            } else if (filter == 2) {  // Up
                for (size_t i = 0; i < raw_stride; i++) {
                    cur[i] = static_cast<uint8_t>(cur[i] + prev[i]);
                }
            } else if (filter == 3) {  // Average
                for (size_t i = 0; i < raw_stride; i++) {
                    const int a = (i >= 4) ? cur[i - 4] : 0;
                    cur[i] = static_cast<uint8_t>(cur[i] + ((a + prev[i]) >> 1));
                }
            } else if (filter == 4) {  // Paeth
                for (size_t i = 0; i < raw_stride; i++) {
                    const int a = (i >= 4) ? cur[i - 4] : 0;
                    const int b = prev[i];
                    const int c = (i >= 4) ? prev[i - 4] : 0;
                    const int p = a + b - c;
                    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    const int pr = (pa <= pb && pa <= pc) ? a : ((pb <= pc) ? b : c);
                    cur[i] = static_cast<uint8_t>(cur[i] + pr);
                }
            }
        }
        // Convert to RGB565 (low byte first, LVGL RGB565 native layout) + A8.
        for (uint32_t x = 0; x < src.w; x++) {
            const size_t px = static_cast<size_t>(x) * 4;
            const uint16_t v = static_cast<uint16_t>(
                ((cur[px] >> 3) << 11) | ((cur[px + 1] >> 2) << 5) | (cur[px + 2] >> 3));
            rgb_acc[rgb_pending++] = v & 0xFF;
            rgb_acc[rgb_pending++] = static_cast<uint8_t>(v >> 8);
            a8_acc[a8_pending++] = cur[px + 3];
        }
        if (rgb_pending == sizeof(buf.rgb_acc) && !flush_rgb()) {
            return false;
        }
        if (a8_pending == sizeof(buf.a8_acc) && !flush_a8()) {
            return false;
        }
        std::memcpy(prev, cur, raw_stride);
    }
    if (rgb_pending != 0 && !flush_rgb()) {
        return false;
    }
    if (a8_pending != 0 && !flush_a8()) {
        return false;
    }
    // Sanity: exact plane coverage.
    if (rgb_written != src.w * src.h * 2u || a8_written != src.w * src.h) {
        return false;
    }
    // Blob CRC is computed from what we just wrote (both planes).
    uint8_t* verify_buf = buf.verify_buf;
    uint32_t crc = 0;
    uint32_t scan = 0;
    while (scan < src.blob_bytes) {
        const uint32_t chunk = std::min<uint32_t>(src.blob_bytes - scan, sizeof(buf.verify_buf));
        if (esp_partition_read(part, blob_base + scan, verify_buf, chunk) != ESP_OK) {
            return false;
        }
        crc = esp_rom_crc32_le(crc, verify_buf, chunk);
        scan += chunk;
    }
    *out_blob_crc = crc;
    return true;
}

// ---------------------------------------------------------------------------
// Directory (1 sector) + blob area handling.

bool ReadDirHeader(const esp_partition_t* part, size_t cache_base, DirHeader* header) {
    uint8_t buf[16];
    if (esp_partition_read(part, cache_base, buf, sizeof(buf)) != ESP_OK) {
        return false;
    }
    std::memcpy(header, buf, sizeof(*header));
    return header->magic == kDirMagic && header->version == kDirVersion && header->count > 0 &&
           header->count <= kMaxSources;
}

bool WriteDir(const esp_partition_t* part, size_t cache_base, const SourceFile* sources, int count,
              const uint32_t* blob_crcs, uint32_t stored_len) {
    DirHeader header{.magic = kDirMagic,
                     .version = kDirVersion,
                     .count = static_cast<uint16_t>(count),
                     .stored_len = stored_len,
                     .reserved = 0};
    if (esp_partition_write(part, cache_base, &header, sizeof(header)) != ESP_OK) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        DirEntry entry;
        std::memset(&entry, 0, sizeof(entry));
        std::memcpy(entry.name, sources[i].name, sizeof(entry.name));  // truncates to 27 chars
        entry.crc = sources[i].crc;
        entry.w = sources[i].w;
        entry.h = sources[i].h;
        entry.blob_off = sources[i].blob_off;
        entry.blob_len = sources[i].blob_bytes;
        entry.blob_crc = blob_crcs[i];
        const uint32_t dst = cache_base + sizeof(DirHeader) + i * sizeof(DirEntry);
        if (esp_partition_write(part, dst, &entry, sizeof(entry)) != ESP_OK) {
            return false;
        }
    }
    return true;
}

bool YieldTick() {
    vTaskDelay(pdMS_TO_TICKS(2));
    return true;
}

}  // namespace

int AssetsPngEnsureCache(const uint8_t* mmap_root, const void* partition) {
    const esp_partition_t* part = static_cast<const esp_partition_t*>(partition);
    registry_ready = false;
    registry_count = 0;
    if (mmap_root == nullptr || part == nullptr || part->size < 3 * kSectorSize) {
        return 0;
    }
    const uint32_t file_count = ReadLE32(mmap_root);
    const uint32_t stored_len = ReadLE32(mmap_root + 8);
    if (file_count == 0 || stored_len == 0 ||
        static_cast<size_t>(12) + stored_len + 2 * kSectorSize > part->size) {
        return 0;
    }
    if (file_count > 256) {
        return 0;
    }

    // Collect PNG sources.
    SourceFile sources[kMaxSources];
    int source_count = 0;
    const size_t data_base = 12 + static_cast<size_t>(file_count) * sizeof(AssetTableEntry);
    for (uint32_t i = 0; i < file_count && source_count < kMaxSources; i++) {
        const uint8_t* entry_ptr = mmap_root + 12 + i * sizeof(AssetTableEntry);
        char name[32];
        std::memcpy(name, entry_ptr, 31);
        name[31] = '\0';
        const uint32_t nominal_size = ReadLE32(entry_ptr + 32);
        if (name[0] == '\0' || nominal_size > stored_len || !NameEndsWithPng(name)) {
            continue;
        }
        const uint32_t data_off = ReadLE32(entry_ptr + 36);
        if (static_cast<size_t>(data_off) > stored_len) {
            continue;
        }
        const uint8_t* payload = mmap_root + data_base + data_off + 2;
        if (payload + nominal_size > mmap_root + static_cast<size_t>(12) + stored_len) {
            continue;
        }
        uint16_t w, h;
        if (!PngIsSupported(payload, nominal_size, &w, &h)) {
            continue;
        }
        SourceFile& src = sources[source_count];
        std::memset(&src, 0, sizeof(src));
        std::memcpy(src.name, name, sizeof(src.name));
        src.name[sizeof(src.name) - 1] = '\0';
        src.data = payload;
        src.size = nominal_size;
        src.w = w;
        src.h = h;
        src.crc = esp_rom_crc32_le(0, payload, nominal_size);
        src.raw_bytes = (static_cast<uint32_t>(w) * 4 + 1) * h;
        src.blob_bytes = static_cast<uint32_t>(w) * h * 3;
        src.blob_occupancy = RoundUp4K(src.blob_bytes);
        source_count++;
    }
    if (source_count == 0) {
        ESP_LOGI(TAG, "No convertible PNG emoji found in the pack");
        return 0;
    }

    const uint32_t cache_base = RoundUp4K(12 + stored_len);
    const uint32_t blobs_base = cache_base + kSectorSize;
    uint32_t blobs_total = 0;
    uint32_t max_raw = 0;
    for (int i = 0; i < source_count; i++) {
        sources[i].blob_off = 0;
        max_raw = std::max(max_raw, sources[i].raw_bytes);
    }
    // Layout blobs in order, each 4 KiB aligned.
    uint32_t next_off = blobs_base;
    for (int i = 0; i < source_count; i++) {
        sources[i].blob_off = next_off;
        next_off += sources[i].blob_occupancy;
    }
    blobs_total = next_off - blobs_base;
    const uint32_t temp_area = RoundUp4K(max_raw);
    const uint32_t temp_base = part->size - temp_area;
    if (blobs_base + blobs_total + temp_area > part->size) {
        ESP_LOGW(TAG, "Not enough space for PNG cache (need %u, free %u)", blobs_total + temp_area,
                 static_cast<uint32_t>(part->size) - blobs_base);
        return 0;
    }

    // Fast path: directory present and every CRC still matches.
    {
        bool valid = true;
        DirHeader header{};
        if (!ReadDirHeader(part, cache_base, &header)) {
            valid = false;
        } else if (header.stored_len != stored_len) {
            valid = false;
        } else if (header.count != source_count) {
            valid = false;
        }
        bool matched[kMaxSources] = {};
        for (int e = 0; e < header.count && valid; e++) {
            uint8_t buf[sizeof(DirEntry)];
            if (esp_partition_read(part, cache_base + sizeof(DirHeader) + e * sizeof(DirEntry), buf,
                                   sizeof(buf)) != ESP_OK) {
                valid = false;
                break;
            }
            DirEntry entry;
            std::memcpy(&entry, buf, sizeof(entry));
            int s = -1;
            for (int i = 0; i < source_count; i++) {
                if (std::strcmp(sources[i].name, entry.name) == 0) {
                    s = i;
                    break;
                }
            }
            if (s < 0 || matched[s]) {  // unknown name or duplicate entry
                valid = false;
                break;
            }
            matched[s] = true;
            if (entry.crc != sources[s].crc || entry.w != sources[s].w || entry.h != sources[s].h ||
                entry.blob_len != sources[s].blob_bytes || entry.blob_off < blobs_base ||
                entry.blob_off + entry.blob_len > blobs_base + blobs_total) {
                valid = false;
                break;
            }
        }
        for (int i = 0; i < source_count && valid; i++) {
            if (!matched[i]) {
                valid = false;
            }
        }
        if (valid) {
            // Cache on flash is fully trustable: the registry reads the
            // directory straight from the mmap; descriptors are built lazily.
            ActivateRegistry(mmap_root, cache_base + sizeof(DirHeader), header.count);
            ESP_LOGI(TAG, "PNG emoji cache validated: %d images", registry_count);
            return registry_count;
        }
    }

    // Full conversion path.
    ESP_LOGI(TAG, "Converting %d PNG emoji to flash planes (one-time, may take ~10s)...",
             source_count);
    const uint32_t erase_start = cache_base;
    const uint32_t erase_len = blobs_base + blobs_total - cache_base;
    if (esp_partition_erase_range(part, erase_start, erase_len) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to erase PNG cache region");
        return 0;
    }

    uint32_t blob_crcs[kMaxSources];
    // Pass-2 working buffers live on the heap only while converting.
    auto* conv_buf =
        static_cast<BlobConvBuf*>(heap_caps_malloc(sizeof(BlobConvBuf), MALLOC_CAP_DEFAULT));
    if (conv_buf == nullptr) {
        ESP_LOGE(TAG, "Not enough RAM for PNG conversion buffers");
        return 0;
    }
    for (int i = 0; i < source_count; i++) {
        // Pass 1: inflate into the temporary raw RGBA area.
        const uint32_t raw_occupancy = RoundUp4K(sources[i].raw_bytes);
        if (esp_partition_erase_range(part, temp_base, raw_occupancy) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to erase temp region");
            free(conv_buf);
            return 0;
        }
        YieldTick();
        const size_t produced = InflatePngRawRows(part, temp_base, sources[i].data, sources[i].size,
                                                  sources[i].w, sources[i].h);
        if (produced != sources[i].raw_bytes) {
            ESP_LOGE(TAG, "Inflate failed for %s", sources[i].name);
            free(conv_buf);
            return 0;
        }
        // Pass 2: unfilter + convert into the blob planes.
        if (!ConvertRawRowsToBlob(part, temp_base, sources[i], sources[i].blob_off, *conv_buf,
                                  &blob_crcs[i])) {
            ESP_LOGE(TAG, "Convert failed for %s", sources[i].name);
            free(conv_buf);
            return 0;
        }
        YieldTick();
    }
    free(conv_buf);

    if (!WriteDir(part, cache_base, sources, source_count, blob_crcs, stored_len)) {
        ESP_LOGE(TAG, "Failed to write PNG cache directory");
        return 0;
    }

    ActivateRegistry(mmap_root, cache_base + sizeof(DirHeader), source_count);
    ESP_LOGI(TAG, "PNG emoji cache: %d images converted", registry_count);
    return registry_count;
}

void AssetsPngReset() {
    registry_ready = false;
    registry_count = 0;
    registry_entries = nullptr;
    registry_mmap_root = nullptr;
    for (int i = 0; i < kMaxRegistrySlots; i++) {
        registry[i].filled = false;
    }
}

const void* AssetsPngGet(const char* name) {
    if (!registry_ready || name == nullptr || name[0] == '\0') {
        return nullptr;
    }
    // Names live in the flash directory (still memory-mapped); slots hold
    // just the built descriptors.
    for (int i = 0; i < registry_count && i < kMaxRegistrySlots; i++) {
        const char* entry_name = reinterpret_cast<const char*>(
            registry_entries + static_cast<size_t>(i) * sizeof(DirEntry));
        if (std::strncmp(entry_name, name, sizeof(DirEntry{}.name)) == 0) {
            if (!PrepareRegistrySlot(i)) {
                return nullptr;
            }
            return &registry[i].dsc;
        }
    }
    return nullptr;
}
