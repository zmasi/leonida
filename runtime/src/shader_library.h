#pragma once

// Finds the translated SPIR-V for a shader the game hands to D3D12 (docs/d3d12_vulkan.md section 3.6).
//
// A pipeline's bytecode is a DXBC container. Its DXIL part holds an XDAT table whose entry 0x0D is an XBCH header
// followed by a zlib stream; the stream inflates to the FSSM blob, the native RDNA2 program the translator read
// (tools/fssm_unpack.py does the same in Python). The SHA-256 of that blob is the `fssm_sha256` field the translator
// writes into `spirv/index.json`, next to `<name>.spv`. This header needs no Windows, Vulkan or third-party code:
// it carries its own inflate (RFC 1950/1951), SHA-256 (FIPS 180-4) and a minimal JSON reader.

#include "descriptor_table.h"
#include "mesh_compute_pair.h"
#include "spirv_words.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace leonida {

// ---------------------------------------------------------------------------------------------------------------
// zlib inflate

// Why zlib_inflate failed, for a caller that reports the reason (runtime/shims/dstorage.cpp turns each into its
// E_DSTORAGE_ZLIB_* code). `limit`: the output would pass the caller's limit; the stream was not read to its end.
enum class ZlibError { none, header, data, checksum, limit };

namespace detail {

class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> in) : in_(in) {}

    // Refill up to 56..63 buffered bits without failing at EOF; bits >= count_ stay zero.
    void fill() {
        if constexpr (std::endian::native == std::endian::little) {
            if (count_ < 56 && in_.size() - pos_ >= 8) {
                std::uint64_t next;
                std::memcpy(&next, in_.data() + pos_, 8);
                const int take = (63 - count_) >> 3;
                const int next_count = count_ | 56;
                buffer_ = (buffer_ | (next << count_)) & ((std::uint64_t(1) << next_count) - 1);
                pos_ += std::size_t(take);
                count_ = next_count;
                return;
            }
        }
        while (count_ <= 55 && pos_ < in_.size()) {
            buffer_ |= std::uint64_t(in_[pos_++]) << count_;
            count_ += 8;
        }
    }

    std::uint32_t bits(int need) {
        if (count_ < need) {
            fill();
            if (count_ < need) {
                failed_ = true;
                buffer_ = 0;
                count_ = 0;
                return 0;
            }
        }
        const std::uint32_t out = std::uint32_t(buffer_ & ((std::uint64_t(1) << need) - 1));
        buffer_ >>= need;
        count_ -= need;
        return out;
    }

    std::uint64_t peek() const { return buffer_; }
    int available() const { return count_; }
    void drop(int n) {
        buffer_ >>= n;
        count_ -= n;
    }
    void fail() {
        failed_ = true;
        buffer_ = 0;
        count_ = 0;
    }

    void align() {
        pos_ -= std::size_t(count_ / 8);
        buffer_ = 0;
        count_ = 0;
    }

    bool byte(std::uint8_t& out) {
        if (pos_ >= in_.size()) return failed_ = true, false;
        out = in_[pos_++];
        return true;
    }

    std::size_t position() const { return pos_ - std::size_t(count_ / 8); }
    bool failed() const { return failed_; }

private:
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    std::uint64_t buffer_ = 0;
    int count_ = 0;
    bool failed_ = false;
};

struct Huffman {
    static constexpr int kFastBits = 9;
    static constexpr std::size_t kFastSize = std::size_t(1) << kFastBits;

    std::array<std::int16_t, 16> count{};
    std::array<std::int16_t, 288> symbol{};
    int fast_bits = 0;
    std::uint32_t fast_mask = 0;
    // Direct lookup for canonical codes of length 1..fast_bits, indexed by LSB-first bits.
    // Entry encoding: (symbol << 4) | length; 0 means length > fast_bits or an unassigned code.
    std::array<std::uint16_t, kFastSize> fast{};
};

// Canonical Huffman table from code lengths. Returns false for an over-subscribed set.
inline bool build_huffman(Huffman& h, const std::int16_t* lengths, int n) {
    static constexpr std::uint8_t kRev4[16] = {0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15};
    h.count.fill(0);
    int max_len = 0;
    for (int i = 0; i < n; ++i) {
        const int len = lengths[i];
        ++h.count[len];
        if (len > max_len) max_len = len;
    }
    if (h.count[0] == n) {  // no codes: valid only if never used
        h.fast_bits = 0;
        h.fast_mask = 0;
        h.fast[0] = 0;
        return true;
    }
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) {
            h.fast_bits = 0;
            h.fast_mask = 0;
            h.fast[0] = 0;
            return false;
        }
    }
    std::array<std::int16_t, 16> offsets{};
    for (int len = 1; len < 15; ++len) offsets[len + 1] = std::int16_t(offsets[len] + h.count[len]);
    for (int i = 0; i < n; ++i)
        if (lengths[i]) h.symbol[offsets[lengths[i]]++] = std::int16_t(i);
    const int fast_bits = max_len < Huffman::kFastBits ? max_len : Huffman::kFastBits;
    const std::size_t fast_size = std::size_t(1) << fast_bits;
    h.fast_bits = fast_bits;
    h.fast_mask = std::uint32_t(fast_size - 1);
    std::fill_n(h.fast.data(), fast_size, std::uint16_t(0));
    int code = 0;
    int idx = 0;
    for (int len = 1; len <= fast_bits; ++len) {
        const int c = h.count[len];
        const std::size_t step = std::size_t(1) << len;
        const int shift = Huffman::kFastBits - len;
        for (int k = 0; k < c; ++k) {
            const std::uint16_t sym = std::uint16_t(h.symbol[idx++]);
            const std::uint16_t packed = std::uint16_t((sym << 4) | std::uint16_t(len));
            const std::uint32_t v = std::uint32_t(code) << shift;
            const std::uint32_t rev =
                (std::uint32_t(kRev4[v & 15u]) << 5) | (std::uint32_t(kRev4[(v >> 4) & 15u]) << 1) | (v >> 8);
            for (std::size_t slot = rev; slot < fast_size; slot += step) h.fast[slot] = packed;
            ++code;
        }
        code <<= 1;
    }
    return true;
}

inline int decode(BitReader& in, const Huffman& h) {
    if (in.available() < h.fast_bits) in.fill();
    const std::uint16_t entry = h.fast[in.peek() & h.fast_mask];
    if (entry != 0) {
        const int len = int(entry & 0x0fu);
        if (len > in.available()) {
            in.fail();
            return -1;
        }
        in.drop(len);
        return int(entry >> 4);
    }
    if (in.available() < 15) in.fill();
    const int avail = in.available();
    std::uint64_t bits = in.peek();
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        if (len > avail) {
            in.fail();
            return -1;
        }
        code |= int(bits & 1u);
        bits >>= 1;
        const int count = h.count[len];
        if (code - count < first) {
            in.drop(len);
            return h.symbol[index + (code - first)];
        }
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

inline ZlibError inflate_codes(BitReader& in, std::vector<std::uint8_t>& out, const Huffman& lengths,
                          const Huffman& distances, std::size_t limit, bool bulk = true) {
    static constexpr std::int16_t length_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                     31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr std::int16_t length_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                                      2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static constexpr std::int16_t distance_base[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,  25,
                                                       33,   49,   65,   97,   129,  193,   257,   385,   513, 769,
                                                       1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
    static constexpr std::int16_t distance_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                                        6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int symbol = decode(in, lengths);
        if (symbol < 0 || in.failed()) return ZlibError::data;
        if (symbol < 256) {
            if (out.size() >= limit) return ZlibError::limit;
            out.push_back(std::uint8_t(symbol));
            continue;
        }
        if (symbol == 256) return ZlibError::none;
        symbol -= 257;
        if (symbol >= 29) return ZlibError::data;
        const std::size_t length = std::size_t(length_base[symbol]) + in.bits(length_extra[symbol]);
        const int d = decode(in, distances);
        if (d < 0 || d >= 30) return ZlibError::data;
        const std::size_t distance = std::size_t(distance_base[d]) + in.bits(distance_extra[d]);
        if (in.failed() || distance > out.size()) return ZlibError::data;
        if (out.size() > limit || length > limit - out.size()) return ZlibError::limit;
        const std::size_t start = out.size();
        if (!bulk) {
            for (std::size_t i = 0; i < length; ++i) out.push_back(out[start - distance + i]);
            continue;
        }
        out.resize(start + length);
        auto* dest = out.data() + start;
        if (distance == 1) {
            std::fill_n(dest, length, dest[-1]);
        } else {
            // Overlapping matches repeat their first distance bytes. Each copy reads only completed output.
            std::size_t copied = std::min(distance, length);
            std::memcpy(dest, dest - distance, copied);
            while (copied < length) {
                const auto count = std::min(copied, length - copied);
                std::memcpy(dest + copied, dest, count);
                copied += count;
            }
        }
    }
}

} // namespace detail

// Inflates a zlib stream (RFC 1950: header, deflate data, Adler-32). Returns false for a malformed stream, a
// preset dictionary, a checksum mismatch, or output beyond `limit` bytes, and then stores the reason in `*why` when
// it is given: `header` for the two header bytes (not deflate, a window over 32 KiB, a bad check value, a preset
// dictionary), `data` for the deflate data or a missing trailer, `checksum` for an Adler-32 mismatch, `limit` for
// output beyond `limit`. Bytes after the Adler-32 trailer are not read.
inline bool zlib_inflate(std::span<const std::uint8_t> in, std::vector<std::uint8_t>& out,
                         std::size_t limit = std::size_t(1) << 30, ZlibError* why = nullptr, bool bulk = true) {
    out.clear();
    const auto fail = [why](ZlibError reason) {
        if (why) *why = reason;
        return false;
    };
    if (why) *why = ZlibError::none;
    if (in.size() < 2) return fail(ZlibError::header);
    const unsigned cmf = in[0], flg = in[1];
    if ((cmf & 0x0f) != 8 || (cmf >> 4) > 7 || ((cmf << 8) | flg) % 31 != 0 || (flg & 0x20))
        return fail(ZlibError::header);
    if (in.size() < 6) return fail(ZlibError::data);
    detail::BitReader reader(in.subspan(2));
    for (bool last = false; !last;) {
        last = reader.bits(1) != 0;
        const std::uint32_t type = reader.bits(2);
        if (reader.failed()) return fail(ZlibError::data);
        if (type == 0) {  // stored
            reader.align();
            std::uint8_t b[4];
            for (auto& x : b)
                if (!reader.byte(x)) return fail(ZlibError::data);
            const unsigned length = b[0] | (b[1] << 8), complement = b[2] | (b[3] << 8);
            if (length != (~complement & 0xffff)) return fail(ZlibError::data);
            if (out.size() > limit || length > limit - out.size()) return fail(ZlibError::limit);
            for (unsigned i = 0; i < length; ++i) {
                std::uint8_t x;
                if (!reader.byte(x)) return fail(ZlibError::data);
                out.push_back(x);
            }
        } else if (type == 1) {  // fixed Huffman
            static const auto tables = [] {
                std::pair<detail::Huffman, detail::Huffman> t;
                std::int16_t lengths[288];
                for (int i = 0; i < 144; ++i) lengths[i] = 8;
                for (int i = 144; i < 256; ++i) lengths[i] = 9;
                for (int i = 256; i < 280; ++i) lengths[i] = 7;
                for (int i = 280; i < 288; ++i) lengths[i] = 8;
                detail::build_huffman(t.first, lengths, 288);
                for (int i = 0; i < 30; ++i) lengths[i] = 5;
                detail::build_huffman(t.second, lengths, 30);
                return t;
            }();
            if (const ZlibError e = detail::inflate_codes(reader, out, tables.first, tables.second, limit, bulk); e != ZlibError::none)
                return fail(e);
        } else if (type == 2) {  // dynamic Huffman
            const int literal_count = int(reader.bits(5)) + 257, distance_count = int(reader.bits(5)) + 1;
            const int code_count = int(reader.bits(4)) + 4;
            if (literal_count > 286 || distance_count > 30) return fail(ZlibError::data);
            static constexpr int order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
            std::int16_t lengths[320] = {};
            for (int i = 0; i < code_count; ++i) lengths[order[i]] = std::int16_t(reader.bits(3));
            detail::Huffman code_lengths;
            if (!detail::build_huffman(code_lengths, lengths, 19)) return fail(ZlibError::data);
            for (int i = 0; i < literal_count + distance_count;) {
                const int symbol = detail::decode(reader, code_lengths);
                if (symbol < 0) return fail(ZlibError::data);
                if (symbol < 16) {
                    lengths[i++] = std::int16_t(symbol);
                    continue;
                }
                std::int16_t value = 0;
                int repeat;
                if (symbol == 16) {
                    if (i == 0) return fail(ZlibError::data);
                    value = lengths[i - 1];
                    repeat = 3 + int(reader.bits(2));
                } else if (symbol == 17) {
                    repeat = 3 + int(reader.bits(3));
                } else {
                    repeat = 11 + int(reader.bits(7));
                }
                if (i + repeat > literal_count + distance_count) return fail(ZlibError::data);
                while (repeat--) lengths[i++] = value;
            }
            if (lengths[256] == 0) return fail(ZlibError::data);  // no end-of-block code
            detail::Huffman literal, distance;
            if (!detail::build_huffman(literal, lengths, literal_count) ||
                !detail::build_huffman(distance, lengths + literal_count, distance_count))
                return fail(ZlibError::data);
            if (const ZlibError e = detail::inflate_codes(reader, out, literal, distance, limit, bulk); e != ZlibError::none)
                return fail(e);
        } else {
            return fail(ZlibError::data);
        }
        if (reader.failed()) return fail(ZlibError::data);
    }
    // Adler-32, big-endian, after the deflate data's last whole byte.
    const std::size_t at = 2 + reader.position();
    if (at > in.size() || in.size() - at < 4) return fail(ZlibError::data);
    std::uint32_t a = 1, b = 0;
    if (bulk) {
        // At most 5,552 bytes of 0xff fit in both 32-bit accumulators between reductions.
        for (std::span<const std::uint8_t> tail(out); !tail.empty();) {
            const auto block = tail.first(std::min<std::size_t>(tail.size(), 5552));
            for (const auto x : block) {
                a += x;
                b += a;
            }
            a %= 65521;
            b %= 65521;
            tail = tail.subspan(block.size());
        }
    } else {
        for (const std::uint8_t x : out) {
            a = (a + x) % 65521;
            b = (b + a) % 65521;
        }
    }
    const std::uint32_t expected = (std::uint32_t(in[at]) << 24) | (std::uint32_t(in[at + 1]) << 16) |
                                   (std::uint32_t(in[at + 2]) << 8) | in[at + 3];
    return ((b << 16) | a) == expected || fail(ZlibError::checksum);
}

// ---------------------------------------------------------------------------------------------------------------
// SHA-256

inline std::string sha256_hex(std::span<const std::uint8_t> data) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    std::vector<std::uint8_t> message(data.begin(), data.end());
    const std::uint64_t bits = std::uint64_t(data.size()) * 8;
    message.push_back(0x80);
    while (message.size() % 64 != 56) message.push_back(0);
    for (int i = 7; i >= 0; --i) message.push_back(std::uint8_t(bits >> (8 * i)));
    for (std::size_t block = 0; block < message.size(); block += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(message[block + 4 * i]) << 24) | (std::uint32_t(message[block + 4 * i + 1]) << 16) |
                   (std::uint32_t(message[block + 4 * i + 2]) << 8) | message[block + 4 * i + 3];
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const std::uint32_t word : h)
        for (int shift = 28; shift >= 0; shift -= 4) out.push_back(hex[(word >> shift) & 0xf]);
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// JSON (the subset the translator's index uses, which is all of JSON except number precision beyond double)

struct JsonObjectEntry;

struct Json {
    enum class Kind { null, boolean, number, string, array, object } kind = Kind::null;
    bool boolean = false;
    double number = 0;
    std::string string;
    std::vector<Json> array;
    // Declared through the incomplete JsonObjectEntry and destroyed out of line: a vector element type that holds a
    // Json by value cannot be instantiated while Json is still being defined. GCC tolerates it, Clang with libstdc++
    // does not (the pair<string, Json> form used to fail to build there). Structured bindings still work.
    std::vector<JsonObjectEntry> object;

    ~Json();
    const Json* find(std::string_view key) const;
};

struct JsonObjectEntry {
    std::string key;
    Json value;
};

inline Json::~Json() = default;

inline const Json* Json::find(std::string_view key) const {
    for (const auto& [k, v] : object)
        if (k == key) return &v;
    return nullptr;
}

class JsonReader {
public:
    explicit JsonReader(std::string_view text) : text_(text) {}

    bool parse(Json& out) {
        if (!value(out, 0)) return false;
        space();
        return pos_ == text_.size();
    }

private:
    void space() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\n' || text_[pos_] == '\r' ||
                                       text_[pos_] == '\t'))
            ++pos_;
    }

    bool literal(std::string_view word) {
        if (text_.substr(pos_, word.size()) != word) return false;
        pos_ += word.size();
        return true;
    }

    bool value(Json& out, int depth) {
        if (depth > 64) return false;
        space();
        if (pos_ >= text_.size()) return false;
        const char c = text_[pos_];
        if (c == '{') {
            out.kind = Json::Kind::object;
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == '}') return ++pos_, true;
            for (;;) {
                space();
                std::string key;
                if (!string(key)) return false;
                space();
                if (pos_ >= text_.size() || text_[pos_++] != ':') return false;
                Json item;
                if (!value(item, depth + 1)) return false;
                out.object.emplace_back(std::move(key), std::move(item));
                space();
                if (pos_ >= text_.size()) return false;
                if (text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                return text_[pos_++] == '}';
            }
        }
        if (c == '[') {
            out.kind = Json::Kind::array;
            ++pos_;
            space();
            if (pos_ < text_.size() && text_[pos_] == ']') return ++pos_, true;
            for (;;) {
                Json item;
                if (!value(item, depth + 1)) return false;
                out.array.push_back(std::move(item));
                space();
                if (pos_ >= text_.size()) return false;
                if (text_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                return text_[pos_++] == ']';
            }
        }
        if (c == '"') {
            out.kind = Json::Kind::string;
            return string(out.string);
        }
        if (literal("true")) return out.kind = Json::Kind::boolean, out.boolean = true, true;
        if (literal("false")) return out.kind = Json::Kind::boolean, out.boolean = false, true;
        if (literal("null")) return out.kind = Json::Kind::null, true;
        // Numbers through strtod: Apple's libc++ does not provide the floating-point from_chars overload. The
        // token is already limited to JSON number characters, and the end check requires all of it to be consumed.
        const std::size_t start = pos_;
        while (pos_ < text_.size() && std::strchr("+-0123456789.eE", text_[pos_])) ++pos_;
        if (pos_ == start) return false;
        out.kind = Json::Kind::number;
        const std::string token(text_.substr(start, pos_ - start));
        char* end = nullptr;
        errno = 0;
        out.number = std::strtod(token.c_str(), &end);
        return errno != ERANGE && end == token.c_str() + token.size();
    }

    static int hex_digit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool string(std::string& out) {
        if (pos_ >= text_.size() || text_[pos_++] != '"') return false;
        while (pos_ < text_.size()) {
            const char c = text_[pos_++];
            if (c == '"') return true;
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (pos_ >= text_.size()) return false;
            const char e = text_[pos_++];
            switch (e) {
            case '"': case '\\': case '/': out.push_back(e); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u': {
                if (pos_ + 4 > text_.size()) return false;
                unsigned code = 0;
                for (int i = 0; i < 4; ++i) {  // exactly four hex digits
                    const int digit = hex_digit(text_[pos_++]);
                    if (digit < 0) return false;
                    code = code << 4 | unsigned(digit);
                }
                if (code < 0x80) {
                    out.push_back(char(code));
                } else if (code < 0x800) {
                    out.push_back(char(0xc0 | (code >> 6)));
                    out.push_back(char(0x80 | (code & 0x3f)));
                } else {  // surrogate pairs are kept as two 3-byte sequences; index names are ASCII
                    out.push_back(char(0xe0 | (code >> 12)));
                    out.push_back(char(0x80 | ((code >> 6) & 0x3f)));
                    out.push_back(char(0x80 | (code & 0x3f)));
                }
                break;
            }
            default: return false;
            }
        }
        return false;
    }

    std::string_view text_;
    std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// DXBC containers and the FSSM program

// The payload of one DXBC part ("DXIL", "PRIV", ...), or an empty span.
inline std::span<const std::uint8_t> dxbc_part(std::span<const std::uint8_t> container, const char (&fourcc)[5]) {
    const auto u32 = [&](std::size_t at) -> std::uint32_t {
        if (at + 4 > container.size()) return 0;
        std::uint32_t v;
        std::memcpy(&v, container.data() + at, 4);
        return v;
    };
    if (container.size() < 32 || std::memcmp(container.data(), "DXBC", 4) != 0) return {};
    const std::uint32_t parts = u32(28);
    if (parts > (container.size() - 32) / 4) return {};
    for (std::uint32_t i = 0; i < parts; ++i) {
        const std::uint32_t at = u32(32 + 4 * std::size_t(i));
        if (std::size_t(at) + 8 > container.size()) return {};
        const std::uint32_t size = u32(std::size_t(at) + 4);
        if (std::size_t(at) + 8 + size > container.size()) return {};
        if (std::memcmp(container.data() + at, fourcc, 4) == 0) return container.subspan(at + 8, size);
    }
    return {};
}

// The FSSM blob of a shader: inflated from the container's DXIL/XDAT/XBCH entry, or the bytecode itself when it is
// already a bare FSSM blob. Empty when neither applies.
inline std::vector<std::uint8_t> fssm_program(std::span<const std::uint8_t> bytecode) {
    if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "FSSM", 4) == 0)
        return {bytecode.begin(), bytecode.end()};
    const auto dxil = dxbc_part(bytecode, "DXIL");
    const auto u32 = [&](std::size_t at) -> std::uint32_t {
        if (at + 4 > dxil.size()) return 0;
        std::uint32_t v;
        std::memcpy(&v, dxil.data() + at, 4);
        return v;
    };
    std::size_t xdat = 0;
    for (; xdat + 4 <= dxil.size(); ++xdat)
        if (std::memcmp(dxil.data() + xdat, "XDAT", 4) == 0) break;
    if (xdat + 12 > dxil.size()) return {};
    // Some containers carry two different 0x0D payloads (195 of 26,503, tools/fssm_unpack.py). The translator's
    // fssm_sha256 is taken from the last one (fssm_unpack.py's default, which make_game.py uses), so this does too.
    const std::uint32_t count = u32(xdat + 4);
    std::span<const std::uint8_t> blob;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::size_t entry = xdat + 12 + 12 * std::size_t(i);
        if (entry + 12 > dxil.size()) return {};
        const std::uint32_t tag = u32(entry), offset = u32(entry + 4), size = u32(entry + 8);
        if (tag != 0x0d) continue;
        if (std::size_t(offset) + size > dxil.size() || size < 24) return {};
        blob = dxil.subspan(offset, size);
    }
    if (blob.empty() || std::memcmp(blob.data(), "XBCH", 4) != 0) return {};
    std::uint32_t unpacked;
    std::memcpy(&unpacked, blob.data() + 16, 4);
    std::vector<std::uint8_t> out;
    if (!zlib_inflate(blob.subspan(24), out, unpacked) || out.size() != unpacked) return {};
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// FSPX payloads: the FidelityFX DLL's compute programs (docs/fspx.md)

// The two entries of an FSPX payload the runtime reads: kind 0 (dispatch state) and kind 6 (the program).
struct FspxParts {
    std::span<const std::uint8_t> state, code;
};

// Kinds 0 and 6 of an FSPX payload, both empty when the payload is not one the DLL's reader accepts. Same rules as
// that reader and tools/fspx_unpack.py's parse_fspx: the total is clamped to the buffer, the directory must fit it,
// an entry whose range starts inside the directory or ends past the total is skipped, the last entry of a kind wins,
// and kind 0 and kind 6 must both be there and non-empty.
inline FspxParts fspx_parts(std::span<const std::uint8_t> payload) {
    const auto u32 = [&](std::size_t at) {
        std::uint32_t v;
        std::memcpy(&v, payload.data() + at, 4);
        return v;
    };
    if (payload.size() < 16 || std::memcmp(payload.data(), "FSPX", 4) != 0) return {};
    const std::uint64_t total = std::min<std::uint64_t>(u32(12), payload.size());
    const std::uint64_t end_dir = 16 + 16 * std::uint64_t(u32(8));
    if (total < end_dir) return {};
    std::uint64_t code_offset = 0, code_size = 0, state_offset = 0, state_size = 0;
    for (std::uint64_t at = 16; at < end_dir; at += 16) {
        const std::uint32_t kind = u32(std::size_t(at)), offset = u32(std::size_t(at) + 4),
                            size = u32(std::size_t(at) + 8);
        if (offset < end_dir || std::uint64_t(offset) + size > total) continue;  // the DLL skips it too
        if (kind == 6) code_offset = offset, code_size = size;
        if (kind == 0) state_offset = offset, state_size = size;
    }
    if (!state_size || !code_size) return {};
    return {payload.subspan(std::size_t(state_offset), std::size_t(state_size)),
            payload.subspan(std::size_t(code_offset), std::size_t(code_size))};
}

// The kind-6 program of an FSPX payload, or an empty span (fspx_parts).
inline std::span<const std::uint8_t> fspx_program(std::span<const std::uint8_t> payload) {
    return fspx_parts(payload).code;
}

// One pointer and size pair of the description device slot 67 receives (docs/fspx.md, "How the DLL reads it").
struct FspxPart {
    std::uint32_t kind;
    const void* data;
    std::uint64_t size;
};

// The largest entry fspx_payload_from_parts takes: far above the 1.0.2.1 DLL's largest (a 41,536-byte program), and
// small enough that the payload's u32 offsets cannot overflow.
inline constexpr std::uint64_t kFspxPartLimit = 16u << 20;

// An FSPX payload built from the pairs of a slot-67 description, in the layout fspx_parts() reads: the header, one
// directory entry per present pair and the entries' bytes after it, each at a 16-byte boundary. A pair is present
// when its pointer is non-null and its size non-zero; a pointer without a size or a size without a pointer, a size
// over kFspxPartLimit, a kind other than 0, 1, 6 and 7, the same kind twice, or a missing kind 0 or kind 6 gives an
// empty result, and nothing is read. Every pointer is read only for its own size.
inline std::vector<std::uint8_t> fspx_payload_from_parts(std::span<const FspxPart> parts) {
    bool seen[8] = {};
    std::vector<const FspxPart*> present;
    for (const FspxPart& part : parts) {
        if (part.kind > 7 || (part.kind != 0 && part.kind != 1 && part.kind != 6 && part.kind != 7)) return {};
        if (seen[part.kind]) return {};
        seen[part.kind] = true;
        if (!part.data && !part.size) continue;
        if (!part.data || !part.size || part.size > kFspxPartLimit) return {};
        present.push_back(&part);
    }
    const auto has = [&](std::uint32_t kind) {
        return std::any_of(present.begin(), present.end(), [&](const FspxPart* p) { return p->kind == kind; });
    };
    if (!has(0) || !has(6)) return {};
    const auto align = [](std::uint64_t v) { return (v + 15) & ~std::uint64_t(15); };
    std::uint64_t at = align(16 + 16 * std::uint64_t(present.size()));
    std::vector<std::uint64_t> offsets;
    for (const FspxPart* p : present) {
        offsets.push_back(at);
        at = align(at + p->size);
    }
    std::vector<std::uint8_t> out(std::size_t(at), 0);
    const auto put = [&](std::size_t where, std::uint32_t v) { std::memcpy(out.data() + where, &v, 4); };
    std::memcpy(out.data(), "FSPX", 4);
    put(4, 1);
    put(8, std::uint32_t(present.size()));
    put(12, std::uint32_t(at));
    for (std::size_t i = 0; i < present.size(); ++i) {
        put(16 + 16 * i, present[i]->kind);
        put(16 + 16 * i + 4, std::uint32_t(offsets[i]));
        put(16 + 16 * i + 8, std::uint32_t(present[i]->size));
        std::memcpy(out.data() + offsets[i], present[i]->data, std::size_t(present[i]->size));
    }
    return out;
}

// Whether `code` looks like a bare kind-6 program: the 36-byte prologue of three VOP3 words with opcode 0x180
// (bits 31..16 = 0xD580) at words 0, 3 and 6, which all 29 programs of the 1.0.2.1 DLL have (docs/fspx.md).
inline bool fspx_bare_program(std::span<const std::uint8_t> code) {
    if (code.size() < 36 || code.size() % 4) return false;
    for (const std::size_t word : {0u, 3u, 6u}) {
        std::uint32_t v;
        std::memcpy(&v, code.data() + 4 * word, 4);
        if (v >> 16 != 0xD580) return false;
    }
    return true;
}

// What a compute shader's bytecode is looked up by in the translator's index.
struct ComputeProgram {
    enum class Kind { none, fssm, fspx } kind = Kind::none;
    // The bytes whose SHA-256 is the index key (fssm_sha256 or fspx_code_sha256), and which the pipeline copies to
    // the GPU address arena as its code base: the program's code offsets are into them.
    std::vector<std::uint8_t> bytes;
    // An FSPX program from a container or a bare payload: its kind-0 entry, whose SHA-256 (fspx_state_sha256) picks
    // among translations of the same kind-6 bytes. Empty for a bare kind-6 program, which comes without it.
    std::vector<std::uint8_t> state;
};

// The program in a compute shader's bytecode:
//   - an FSSM blob, bare or in a DXBC container's DXIL part (fssm_program), the game's own shaders;
//   - the kind-6 program of an FSPX payload in a DXBC container's ACCC part, what the FidelityFX DLL passes to device
//     slot 62, or of a bare FSPX payload;
//   - a bare kind-6 program, what the DLL passes in its slot-67 description at +0x28 (with its exact size at +0x30).
// A DXBC container whose ACCC part is not a usable FSPX payload, and anything else, gives Kind::none.
inline ComputeProgram compute_program(std::span<const std::uint8_t> bytecode) {
    ComputeProgram out;
    out.bytes = fssm_program(bytecode);
    if (!out.bytes.empty()) {
        out.kind = ComputeProgram::Kind::fssm;
        return out;
    }
    FspxParts parts;
    if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "DXBC", 4) == 0)
        parts = fspx_parts(dxbc_part(bytecode, "ACCC"));
    else if (bytecode.size() >= 4 && std::memcmp(bytecode.data(), "FSPX", 4) == 0)
        parts = fspx_parts(bytecode);
    else if (fspx_bare_program(bytecode))
        parts.code = bytecode;
    if (parts.code.empty()) return out;
    out.kind = ComputeProgram::Kind::fspx;
    out.bytes.assign(parts.code.begin(), parts.code.end());
    out.state.assign(parts.state.begin(), parts.state.end());
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// The translator's spirv/index.json

// One element of a shader's DXBC input signature (ISG1), as tools/shader_meta.py records it. For a vertex shader
// the register is the Vulkan input location the translator gave it (docs/design.md, "Vertex fetch").
struct SignatureElement {
    std::string semantic;
    std::uint32_t semantic_index = 0;
    std::uint32_t register_index = 0;
    std::uint32_t system_value = 0;  // 0: an ordinary input fed by the input layout
    std::string component_type;      // "float", "uint", "sint"
};

struct ShaderEntry {
    std::string name;
    std::string spv_file;  // optional "<prefix><spv>" filename when the index key is a hash
    std::string stage;     // "vs", "ps", "cs", "ms", "gs"
    std::string fssm_sha256;
    std::string fspx_code_sha256;   // an FSPX program (docs/fspx.md): the SHA-256 of its kind-6 bytes
    std::string fspx_state_sha256;  // and of its kind-0 entry (dispatch state)
    std::size_t binding_count = 0;  // set-0 bindings the module declares
    std::uint32_t push_constant_bytes = 0;
    std::array<std::uint32_t, 3> local_size{1, 1, 1};
    // Workgroup memory the module declares: its LDS array plus what --wave64-emulate adds (SHADER-309). A compute
    // pipeline needs it at or under the device's maxComputeSharedMemorySize (32768 on some drivers).
    std::uint32_t lds_bytes = 0;
    bool uses_code_base = false;
    bool code_base_known = false;
    bool wave64 = false;
    // The subgroup size the translator assumed (64 for a wave64 shader, 32 otherwise); 0 when the entry says null,
    // as one translated with --wave64-emulate does, or has none.
    std::uint32_t subgroup_size = 0;
    // "workgroup" for a shader translated with --wave64-emulate: it uses no subgroup operation and is exact on any
    // subgroup size. Empty for a native translation.
    std::string wave64_exchange;
    // Wave64 VS/PS/GS only: the translator's partition-invariance tier (index.json `wave64_graphics`; docs/design.md,
    // "Wave64 graphics"). A and B are exact when the wave is cut into two 32-lane halves, C is not. Empty for other
    // stages or an index written without the analysis.
    std::string wave64_graphics;
    bool emulated() const { return wave64_exchange == "workgroup"; }
    // Mesh shaders: the index's "root_window" is {"sgpr": 16, "dword": 8}, so the module reads its root arguments
    // from the graphics window (push dword 8 = root dword 0, D3D-448 follow-up). False for one translated before it.
    bool graphics_root_window = false;
    std::vector<SignatureElement> input_signature;  // vertex shaders: what the input layout must feed
    std::vector<std::uint32_t> inputs;              // pixel shaders: input locations declared
    std::vector<std::uint32_t> outputs;             // pixel/vertex shaders: output locations written
    // --f16-interp-fallback: param/attr index -> {low_half_location, high_half_location} (SHADER-378).
    std::map<std::uint32_t, std::array<std::uint32_t, 2>> f16_halves;
    // The S#s the shader builds from immediates (the index's literal_samplers): no heap slot names them, so a
    // pipeline that uses the shader registers them in the bindless set itself (docs/design.md, Literal samplers).
    std::vector<std::array<std::uint32_t, 4>> literal_samplers;
    // Mesh shaders: the output array sizes the SPIR-V declares (OutputVertices / OutputPrimitivesEXT), checked
    // against the device's maxMeshOutputVertices / maxMeshOutputPrimitives when a pipeline is created. 0: not given.
    std::uint32_t max_vertices = 0;
    std::uint32_t max_primitives = 0;
    // --mesh-compute (mesh_compute_pair.h): the validated slot layout for a logical-ms GLCompute producer plus its
    // generated Vertex companion (<name>.mesh_vs.spv). Present only for stage "ms" with a well-formed object;
    // absent/null (native mesh) and malformed (fails closed) both read as nullopt. A 128-byte pc-tail layout stays
    // present with uses_pc_tail so the PSO refuses it explicitly instead of misreading it as native.
    std::optional<MeshComputeLayout> mesh_compute;
};

// Which descriptor-table hash finalizer the loaded modules probe with (descriptor_table.h). Each module's index.json
// meta may carry `"descriptor_hash": "fnv1a_fmix32"`; an absent key means the old bare `"fnv1a"`. The runtime has one
// table for all shaders, so it counts the kinds over every loaded module (the root index and each sub-index) and picks
// the majority; a mixed library still works, because the minority's lookups simply miss to index 0.
struct DescriptorHashCensus {
    std::size_t fnv1a = 0;
    std::size_t fnv1a_fmix32 = 0;
    std::size_t total() const { return fnv1a + fnv1a_fmix32; }
};

// The kind most modules declare. A tie (or no modules) is fnv1a, the behaviour before the marker existed.
inline DescriptorHashKind majority_descriptor_hash_kind(const DescriptorHashCensus& census) {
    return census.fnv1a_fmix32 > census.fnv1a ? DescriptorHashKind::fnv1a_fmix32 : DescriptorHashKind::fnv1a;
}

// Which descriptor-table entry layout the loaded modules read (descriptor_table.h, tools/rdna2spirv.py
// --descriptor-table). A module whose meta carries `"descriptor_table": "tagged"` also reads the kind word next to
// each index and uses it to tell which array an index belongs to; an absent key is the old plain layout, which reads
// only the index. The runtime fills the kind word for a tagged table and leaves it alone for a plain one, so a
// library translated before the marker (or with --descriptor-table plain) behaves exactly as it did.
enum class DescriptorTableLayout : std::uint8_t { plain = 0, tagged = 1 };

inline constexpr const char* descriptor_table_layout_name(DescriptorTableLayout layout) {
    return layout == DescriptorTableLayout::tagged ? "tagged" : "plain";
}

struct DescriptorTableCensus {
    std::size_t plain = 0;
    std::size_t tagged = 0;
    std::size_t total() const { return plain + tagged; }
};

// The tagged layout when any module reads it. The kind word is additive: a tagged module reads it and a plain
// module ignores it (it reads the index at the same place), so one tagged module in the corpus needs the tagged
// layout, however many plain ones there are. Counting entries would let a merged or partial index whose plain
// modules outnumber the tagged ones hand every tagged module an all-zero kind, and every image of another
// dimension would then fall back. No tagged module is the old plain layout.
inline DescriptorTableLayout majority_descriptor_table_layout(const DescriptorTableCensus& census) {
    return census.tagged > 0 ? DescriptorTableLayout::tagged : DescriptorTableLayout::plain;
}

// LEONIDA_DESCRIPTOR_TABLE=plain|tagged overrides the modules' vote. False when `value` is null, empty or not one
// of the two layouts (an unrecognised value is ignored rather than failing startup).
inline bool descriptor_table_layout_from_env(const char* value, DescriptorTableLayout* layout) {
    if (!value || !*value) return false;
    if (std::strcmp(value, "tagged") == 0) {
        if (layout) *layout = DescriptorTableLayout::tagged;
        return true;
    }
    if (std::strcmp(value, "plain") == 0) {
        if (layout) *layout = DescriptorTableLayout::plain;
        return true;
    }
    return false;
}

// LEONIDA_DESCRIPTOR_HASH=fnv1a|fnv1a_fmix32 overrides the modules' majority. False when `value` is null, empty or not
// one of the two kinds (an unrecognised value is ignored rather than failing startup).
inline bool descriptor_hash_kind_from_env(const char* value, DescriptorHashKind* kind) {
    if (!value || !*value) return false;
    if (std::strcmp(value, "fnv1a_fmix32") == 0) {
        if (kind) *kind = DescriptorHashKind::fnv1a_fmix32;
        return true;
    }
    if (std::strcmp(value, "fnv1a") == 0) {
        if (kind) *kind = DescriptorHashKind::fnv1a;
        return true;
    }
    return false;
}

class ShaderLibrary {
public:
    // Reads <dir>/index.json, then the indexes of the programs make_game.py translates from the package's DLLs and the
    // game executable, each when it is there:
    //   - <dir>/fspx/index.json, the FidelityFX DLL's FSPX programs, entries named "fspx/<name>" (docs/fspx.md);
    //   - <dir>/fspx_compute/index.json, the compute programs from shaders.rpf's pipeline library;
    //   - <dir>/fspx_gfx/index.json, the graphics programs from the same library;
    //   - <dir>/bvh/index.json, XboxBvhBuilder.dll's FSSM programs, entries named "bvh/<name>" and found by
    //     fssm_sha256 like the game's own shaders (docs/embedded_shaders.md);
    //   - <dir>/exe/index.json, game_scarlett_master.exe's own DXBC containers, entries named "exe/<name>" and found
    //     the same way (docs/embedded_shaders.md, "Retail executable");
    //   - <dir>/library/index.json, the compute programs of the pipeline library in shaders.rpf, which the executable
    //     creates with slot 67; entries named "library/<name>" and found by fspx_code_sha256 and fspx_state_sha256
    //     like the FSPX ones (docs/shader_library.md).
    // fspx_compute/ and library/ are two builds of the same slot-67 programs (make_game.py's library_shaders() writes
    // library/; fspx_compute/ is the set translated from every console-path miss): both go into the same FSPX lookup
    // by kind-6 and kind-0 SHA-256, and fspx_compute/ is read first, so its entry is the one found when both hold a
    // program. Entries with neither fssm_sha256 nor fspx_code_sha256 cannot be found by content and are skipped. The
    // sub-indexes are optional: a missing or unreadable index there leaves the main one and the other folders loaded.
    bool load(const std::filesystem::path& dir, std::string* error = nullptr) {
        const auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        // Nothing here may throw into the D3D12 shim (only allocation can, and that is caught too).
        std::filesystem::path folder;
        try {
            // The modules are read when a pipeline needs them, later than this. A relative folder would follow the
            // current directory, which the title may change (SetCurrentDirectory), so it is fixed here once.
            std::error_code ec;
            folder = std::filesystem::absolute(dir, ec);
            if (ec) folder = dir;
            if (!load_index(folder, "", fail)) return false;
        } catch (...) {
            return fail("index.json could not be read");
        }
        for (const char* sub_folder : {"fspx", "fspx_compute", "fspx_gfx", "bvh", "exe", "library"}) {
            try {
                const auto ignore = [](const std::string&) { return false; };
                const std::filesystem::path sub = folder / sub_folder;
                if (std::ifstream(sub / "index.json", std::ios::binary))
                    load_index(sub, std::string(sub_folder) + "/", ignore);
            } catch (...) {
            }
        }
        return true;
    }

    // Loads only the optional FSPX sub-indexes (<dir>/fspx/index.json and <dir>/fspx_gfx/index.json) without parsing
    // the 26,565-entry main index.json, for fast stage/hash lookups in the Xbox device proxy.
    bool load_fspx_only(const std::filesystem::path& dir) {
        dir_ = dir;
        entries_.clear();
        fspx_entries_.clear();
        count_ = 0;
        fspx_count_ = 0;
        fspx_compute_count_ = 0;
        fspx_gfx_count_ = 0;
        bvh_count_ = 0;
        exe_count_ = 0;
        descriptor_hash_census_ = {};
        descriptor_table_census_ = {};
        for (const char* folder : {"fspx", "fspx_gfx"}) {
            try {
                const auto ignore = [](const std::string&) { return false; };
                const std::filesystem::path sub = dir / folder;
                if (std::ifstream(sub / "index.json", std::ios::binary))
                    load_index(sub, std::string(folder) + "/", ignore);
            } catch (...) {
            }
        }
        return (fspx_count_ + fspx_gfx_count_) > 0;
    }

    bool loaded() const { return !dir_.empty(); }
    // The folder load() read, made absolute; empty before.
    const std::filesystem::path& directory() const { return dir_; }
    std::size_t size() const { return count_; }
    // The descriptor-hash kinds the loaded modules declare, and the kind the majority of them probe with (fnv1a when
    // they tie or none declares one). The device takes this once, before its descriptor table's first insert, unless
    // LEONIDA_DESCRIPTOR_HASH names a kind.
    const DescriptorHashCensus& descriptor_hash_census() const { return descriptor_hash_census_; }
    DescriptorHashKind descriptor_hash_kind() const { return majority_descriptor_hash_kind(descriptor_hash_census_); }
    // The entry layouts the loaded modules read, and the layout to use: tagged as soon as one module reads the kind
    // word (it is additive, see majority_descriptor_table_layout), plain when none does. The device takes this once,
    // before its descriptor table's first insert, unless LEONIDA_DESCRIPTOR_TABLE names a layout.
    const DescriptorTableCensus& descriptor_table_census() const { return descriptor_table_census_; }
    DescriptorTableLayout descriptor_table_layout() const {
        return majority_descriptor_table_layout(descriptor_table_census_);
    }
    // Entries loaded from <dir>/fspx/index.json and <dir>/library/index.json. A corpus of RPF containers never
    // reaches these: their programs come from FidelityFX DLLs and the pipeline library, so callers comparing reached
    // entries with size() must leave them out.
    std::size_t fspx_size() const { return fspx_count_; }
    std::size_t fspx_compute_size() const { return fspx_compute_count_; }
    // Entries loaded from <dir>/fspx_gfx/index.json (graphics FSPX programs from shaders.rpf's pipeline library).
    std::size_t fspx_gfx_size() const { return fspx_gfx_count_; }
    // Entries loaded from <dir>/bvh/index.json. No RPF container reaches these either: XboxBvhBuilder.dll passes
    // their containers itself.
    std::size_t bvh_size() const { return bvh_count_; }
    // Entries loaded from <dir>/exe/index.json: game_scarlett_master.exe's inline containers, which no shader
    // archive holds.
    std::size_t exe_size() const { return exe_count_; }

    // The first entry for this program. Two containers can hold the same FSSM program with different input
    // signatures; `candidates` returns all of them.
    const ShaderEntry* find(std::string_view stage, std::string_view fssm_sha256) const {
        const auto all = candidates(stage, fssm_sha256);
        return all.empty() ? nullptr : all.front();
    }

    std::vector<const ShaderEntry*> candidates(std::string_view stage, std::string_view fssm_sha256) const {
        return lookup(entries_, stage, fssm_sha256);
    }

    // The entries of an FSPX program, by the SHA-256 of its kind-6 bytes (fspx_code_sha256).
    std::vector<const ShaderEntry*> fspx_candidates(std::string_view stage, std::string_view fspx_code_sha256) const {
        return lookup(fspx_entries_, stage, fspx_code_sha256);
    }

    // Finds an FSPX program by its kind-6 SHA-256 across all stages so the stage comes from the index entry itself.
    const ShaderEntry* find_fspx_by_code(std::string_view fspx_code_sha256) const {
        for (std::string_view st : {"vs", "ps", "ms", "gs", "cs"}) {
            const auto all = fspx_candidates(st, fspx_code_sha256);
            if (!all.empty()) return all.front();
        }
        return nullptr;
    }

    // The entries for a program compute_program() found, by the key of its kind (docs/fspx.md, "Build and run-time
    // lookup"):
    //   - FSSM: by fssm_sha256, as before;
    //   - FSPX with its kind-0 entry (a container or a bare payload): by fspx_code_sha256, then only the entries
    //     whose fspx_state_sha256 is the SHA-256 of that kind-0 entry;
    //   - a bare kind-6 program, which comes without kind 0: by fspx_code_sha256 when all those entries have the
    //     same module and runtime metadata; otherwise the program is ambiguous and the result is empty.
    std::vector<const ShaderEntry*> program_candidates(std::string_view stage, const ComputeProgram& program,
                                                       bool* ambiguous = nullptr) const {
        if (ambiguous) *ambiguous = false;
        if (program.kind == ComputeProgram::Kind::none) return {};
        const std::string hash = sha256_hex(program.bytes);
        if (program.kind == ComputeProgram::Kind::fssm) return candidates(stage, hash);
        auto all = fspx_candidates(stage, hash);
        if (!program.state.empty()) {
            const std::string state = sha256_hex(program.state);
            std::erase_if(all, [&](const ShaderEntry* e) { return e->fspx_state_sha256 != state; });
            return all;
        }
        if (all.size() > 1) {
            const auto first = spirv(*all.front());
            const auto runtime_metadata = [](const ShaderEntry& e) {
                return std::tie(e.binding_count, e.push_constant_bytes, e.local_size, e.lds_bytes, e.uses_code_base,
                                e.wave64, e.subgroup_size, e.wave64_exchange, e.literal_samplers);
            };
            for (std::size_t i = 1; i < all.size(); ++i) {
                if (first.empty() || runtime_metadata(*all[i]) != runtime_metadata(*all.front()) ||
                    spirv(*all[i]) != first) {
                    if (ambiguous) *ambiguous = true;
                    return {};
                }
            }
        }
        return all;
    }

    // The module's file: <dir>/<name>.spv, or the index's spv file name when that one is missing.
    std::filesystem::path spirv_file(const ShaderEntry& entry) const {
        std::error_code ec;
        auto path = dir_;
        path += "/" + entry.name + ".spv";
        if (!std::filesystem::exists(path, ec) && !entry.spv_file.empty()) {
            auto alt = dir_;
            alt += "/" + entry.spv_file;
            return alt;
        }
        return path;
    }

    // The size of the module's file in bytes, or 0 when it is missing: what spirv() would read, without reading it.
    std::size_t spirv_bytes(const ShaderEntry& entry) const {
        std::error_code ec;
        auto path = dir_;
        path += "/" + entry.name + ".spv";
        auto size = std::filesystem::file_size(path, ec);
        if (ec && !entry.spv_file.empty()) {
            auto alt = dir_;
            alt += "/" + entry.spv_file;
            size = std::filesystem::file_size(alt, ec);
        }
        return ec ? 0 : static_cast<std::size_t>(size);
    }

    // Every entry of `stage` once, in name order, from every index (an entry reachable both by its FSSM and its FSPX
    // hash is one entry). gta6rt --precompile-pipelines builds them all.
    std::vector<const ShaderEntry*> stage_entries(std::string_view stage) const {
        std::map<std::pair<std::string_view, std::string_view>, const ShaderEntry*> unique;
        for (const Entries* map : {&entries_, &fspx_entries_})
            for (const auto& [key, list] : *map)
                for (const ShaderEntry& e : list)
                    if (e.stage == stage) unique.emplace(std::pair<std::string_view, std::string_view>{e.name, e.spv_file}, &e);
        std::vector<const ShaderEntry*> out;
        out.reserve(unique.size());
        for (const auto& [key, e] : unique) out.push_back(e);
        return out;
    }

    // The module's words, or empty when <dir>/<name>.spv is missing or not SPIR-V.
    std::vector<std::uint32_t> spirv(const ShaderEntry& entry) const {
        auto path = dir_;
        path += "/" + entry.name + ".spv";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file && !entry.spv_file.empty()) {
            auto alt = dir_;
            alt += "/" + entry.spv_file;
            file.open(alt, std::ios::binary | std::ios::ate);
        }
        return read_spirv_words(file, file.tellg());
    }
    // The mesh-compute companion's file: <dir>/<name>.mesh_vs.spv, adjacent to the producer (mesh_compute_pair.h).
    // The generator names it from the index key, never from spv_file, so a hash-named producer still finds its
    // companion under its own entry name; sub-index entries (name "fspx/...") resolve inside their own folder.
    // Unchecked bytes: callers must use load_mesh_compute_pair / validate_mesh_compute_modules, never this alone
    // for a declared mesh-compute representation.
    std::filesystem::path mesh_companion_file(const ShaderEntry& entry) const {
        auto path = dir_;
        path += "/" + entry.name + kMeshComputeCompanionSuffix + ".spv";
        return path;
    }

    // The companion's words, or empty when <dir>/<name>.mesh_vs.spv is missing or not SPIR-V. Unchecked (see above).
    std::vector<std::uint32_t> mesh_companion_spirv(const ShaderEntry& entry) const {
        auto path = dir_;
        path += "/" + entry.name + kMeshComputeCompanionSuffix + ".spv";
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file) return {};
        return read_spirv_words(file, file.tellg());
    }

    // The versioned seal binding the validated layout to both module bytes:
    // <dir>/<name>.mesh_compute.seal.json (prefix-aware like the companion). Written by rdna2spirv.py at
    // generation (partial: layout+producer) and completed by mesh_compute_vs.py (full: +companion hashes).
    std::filesystem::path mesh_compute_seal_file(const ShaderEntry& entry) const {
        auto path = dir_;
        path += "/" + entry.name + kMeshComputeSealSuffix;
        return path;
    }

    // Loads and validates BOTH byte vectors for PSO creation, bound to the sealed layout. On true, *producer_words
    // and *companion_words hold the exact validated bytes (caller owns, retain immutably, use SAME vectors for
    // vkCreateShaderModule/Pipelines with no re-reads). On false both are cleared and *error explains: missing or
    // malformed seal on a declared ms (never silent native), layout/producer/companion hash mismatch (swapped,
    // tampered, or stale), wrong execution models, LocalSize disagreement with metadata, or 128B pc-tail (explicit).
    bool load_mesh_compute_pair(const ShaderEntry& entry, std::vector<std::uint32_t>* producer_words,
                                std::vector<std::uint32_t>* companion_words, std::string* error = nullptr) const {
        const auto fail = [&](const std::string& why) {
            if (producer_words) producer_words->clear();
            if (companion_words) companion_words->clear();
            if (error) *error = why;
            return false;
        };
        if (!entry.mesh_compute) return fail("mesh_compute: no layout (native mesh or malformed, fails closed)");
        if (!entry.mesh_compute->is_supported_push()) {
            if (entry.mesh_compute->is_pc_tail())
                return fail("mesh_compute: 128-byte pc-tail UBO layout (address at tail dwords 4..5) has no verified "
                            "companion in this wave; refusing (need the 152-byte push variant)");
            return fail("mesh_compute: unsupported push contract");
        }
        // Single read each (no TOCTOU/duplicate I/O): the exact vectors below are hashed and returned on success.
        std::vector<std::uint32_t> producer = spirv(entry);
        if (producer.empty()) {
            std::error_code ec;
            const bool exists = std::filesystem::exists(spirv_file(entry), ec) && !ec;
            return fail(exists ? "mesh_compute: malformed producer SPIR-V for " + entry.name
                               : "mesh_compute: missing producer SPIR-V for " + entry.name);
        }
        std::vector<std::uint32_t> companion = mesh_companion_spirv(entry);
        if (companion.empty()) {
            std::error_code ec;
            const bool exists = std::filesystem::exists(mesh_companion_file(entry), ec) && !ec;
            return fail(exists ? "mesh_compute: malformed companion SPIR-V for " + entry.name
                               : "mesh_compute: missing companion " + entry.name +
                                     std::string(kMeshComputeCompanionSuffix) + ".spv");
        }
        if (!validate_mesh_compute_modules(entry, producer, companion, error))
            return fail(error ? *error : "mesh_compute: pair validation failed");
        if (producer_words) *producer_words = std::move(producer);
        if (companion_words) *companion_words = std::move(companion);
        return true;
    }

    // Revalidates retained vectors against a FRESH seal + current entry (lazy reload, no .spv disk reads, only the
    // seal file is reread). Hashes the given spans' bytes (size*4 LE, must equal file-byte hashes) and re-runs all
    // checks. False (with *error) on any mismatch; never vkCreate* on false.
    bool validate_mesh_compute_modules(const ShaderEntry& entry, std::span<const std::uint32_t> producer_words,
                                       std::span<const std::uint32_t> companion_words,
                                       std::string* error = nullptr) const {
        const auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        if (!entry.mesh_compute) return fail("mesh_compute: no layout (native mesh or malformed, fails closed)");
        const MeshComputeLayout& layout = *entry.mesh_compute;
        if (!layout.is_supported_push()) {
            if (layout.is_pc_tail())
                return fail("mesh_compute: 128-byte pc-tail UBO layout has no verified companion; refusing");
            return fail("mesh_compute: unsupported push contract");
        }
        MeshComputeSeal seal;
        if (!read_mesh_compute_seal(entry, &seal, error)) return fail(error ? *error : "mesh_compute: seal error");
        if (!seal.has_companion())
            return fail("mesh_compute: seal for " + entry.name + " lacks companion hashes (not built)");
        const std::string layout_json = canonical_mesh_compute_json(layout);
        const std::span<const std::uint8_t> layout_span(reinterpret_cast<const std::uint8_t*>(layout_json.data()),
                                                        layout_json.size());
        if (sha256_hex(layout_span) != seal.layout_sha256)
            return fail("mesh_compute: index layout does not match the seal for " + entry.name +
                        " (tampered index while modules unchanged)");
        const auto hash_words = [](std::span<const std::uint32_t> words) {
            const std::uint8_t* bytes = reinterpret_cast<const std::uint8_t*>(words.data());
            return sha256_hex(std::span<const std::uint8_t>(bytes, words.size() * 4));
        };
        if (producer_words.empty() || companion_words.empty()) return fail("mesh_compute: empty module words");
        if (hash_words(producer_words) != seal.producer_sha256)
            return fail("mesh_compute: producer bytes do not match the seal for " + entry.name +
                        " (swapped or stale module)");
        if (hash_words(companion_words) != *seal.companion_spv_sha256)
            return fail("mesh_compute: companion bytes do not match the seal for " + entry.name +
                        " (replaced cached binary)");
        if (!mesh_producer_is_compute(producer_words, error)) return false;
        std::array<std::uint32_t, 3> actual_size{0, 0, 0};
        if (!mesh_producer_local_size(producer_words, actual_size, error)) return false;
        if (actual_size != entry.local_size)
            return fail("mesh_compute: producer LocalSize does not match index metadata for " + entry.name);
        if (!mesh_consumer_is_vertex(companion_words, error)) return false;
        return mesh_companion_matches_layout(layout, companion_words, error);
    }

    // Whether this entry is usable for direct replay (seal-bound). Implemented via load_mesh_compute_pair and
    // discarding the vectors, so no stale blessing: same seal/hash/exec/LocalSize checks, same explicit 128B and
    // missing-stamp errors. Prefer load_mesh_compute_pair when the bytes are needed (single validated load).
    bool validate_mesh_compute_pair(const ShaderEntry& entry, std::string* error = nullptr) const {
        std::vector<std::uint32_t> producer, companion;
        return load_mesh_compute_pair(entry, &producer, &companion, error);
    }

private:
    // A non-negative integer field, or 0 when it is missing, not a number or out of range.
    static std::uint32_t u32(const Json* value) {
        if (!value || value->kind != Json::Kind::number) return 0;
        const double n = value->number;
        return n >= 0 && n <= 4294967295.0 ? std::uint32_t(n) : 0;
    }

    // Strict JSON integers for mesh_compute validation (fail closed, unlike lenient u32() above).
    static bool json_u32(const Json* value, std::uint32_t* out) {
        if (!value || value->kind != Json::Kind::number) return false;
        const double n = value->number;
        if (n < 0.0 || n > 4294967295.0 || double(std::uint32_t(n)) != n) return false;
        if (out) *out = std::uint32_t(n);
        return true;
    }
    static bool json_u64(const Json* value, std::uint64_t* out) {
        if (!value || value->kind != Json::Kind::number) return false;
        const double n = value->number;
        // JSON numbers are doubles: integers only up to 2^53-1, ample for slot byte counts.
        if (n < 0.0 || n > 9007199254740991.0 || double(std::uint64_t(n)) != n) return false;
        if (out) *out = std::uint64_t(n);
        return true;
    }
    static bool json_key_u32(const std::string& key, std::uint32_t* out) {
        if (key.empty() || key.size() > 10) return false;
        std::uint64_t v = 0;
        for (const char c : key) {
            if (c < '0' || c > '9') return false;
            v = v * 10u + std::uint64_t(c - '0');
            if (v > UINT32_MAX) return false;
        }
        if (out) *out = std::uint32_t(v);
        return true;
    }
    static bool is_hex64(const std::string& s) {
        if (s.size() != 64) return false;
        for (const char c : s)
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        return true;
    }
    // Reads and validates <dir>/<name>.mesh_compute.seal.json v1 (hashes only; the seal's "layout" copy is ignored,
    // the canonical hash is recomputed from entry.mesh_compute). False for missing/malformed/unknown-version seals
    // on a declared representation (never silent native).
    bool read_mesh_compute_seal(const ShaderEntry& entry, MeshComputeSeal* seal,
                                std::string* error = nullptr) const {
        const auto fail = [&](const std::string& why) {
            if (error) *error = why;
            return false;
        };
        auto path = dir_;
        path += "/" + entry.name + kMeshComputeSealSuffix;
        std::ifstream file(path, std::ios::binary);
        if (!file) return fail("mesh_compute: missing seal for declared representation " + entry.name);
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        Json root;
        if (!JsonReader(text).parse(root) || root.kind != Json::Kind::object)
            return fail("mesh_compute: malformed seal JSON for " + entry.name);
        std::uint32_t version = 0;
        if (!json_u32(root.find("version"), &version) || version != std::uint32_t(kMeshComputeSealVersion))
            return fail("mesh_compute: unsupported seal version for " + entry.name);
        const Json* layout_hash = root.find("layout_sha256");
        const Json* producer_hash = root.find("producer_sha256");
        if (!layout_hash || layout_hash->kind != Json::Kind::string || !is_hex64(layout_hash->string) ||
            !producer_hash || producer_hash->kind != Json::Kind::string || !is_hex64(producer_hash->string)) {
            return fail("mesh_compute: malformed seal hashes for " + entry.name);
        }
        MeshComputeSeal out;
        out.version = int(version);
        out.layout_sha256 = layout_hash->string;
        out.producer_sha256 = producer_hash->string;
        const Json* asm_hash = root.find("companion_asm_sha256");
        const Json* spv_hash = root.find("companion_spv_sha256");
        const bool asm_null = !asm_hash || asm_hash->kind == Json::Kind::null;
        const bool spv_null = !spv_hash || spv_hash->kind == Json::Kind::null;
        if (asm_null != spv_null) return fail("mesh_compute: partial companion seal for " + entry.name);
        if (!asm_null) {
            if (asm_hash->kind != Json::Kind::string || !is_hex64(asm_hash->string) ||
                spv_hash->kind != Json::Kind::string || !is_hex64(spv_hash->string)) {
                return fail("mesh_compute: malformed companion seal hashes for " + entry.name);
            }
            out.companion_asm_sha256 = asm_hash->string;
            out.companion_spv_sha256 = spv_hash->string;
        }
        if (seal) *seal = std::move(out);
        return true;
    }

    // Validates one index.json "mesh_compute" object for a logical-ms entry whose top-level push/max/f16 fields
    // are already in `entry`. Returns nullopt for any malformed layout (fails closed); the caller only calls this
    // for stage "ms" with an object value, so absent/null (native) and wrong-stage entries never reach here.
    static std::optional<MeshComputeLayout> parse_mesh_compute_layout(const Json& mc, const ShaderEntry& entry) {
        MeshComputeLayout layout;
        std::uint32_t vertex_stride = 0, max_vertices = 0, max_primitives = 0, clip_count = 0, clip_offset = 0;
        std::uint32_t cull_count = 0, cull_offset = 0, vertices = 0, primitive_stride = 0, push_bytes = 0;
        std::uint64_t slot_bytes = 0, primitives = 0;
        if (!json_u64(mc.find("slot_bytes"), &slot_bytes) || !json_u32(mc.find("vertices"), &vertices) ||
            !json_u32(mc.find("vertex_stride"), &vertex_stride) || !json_u64(mc.find("primitives"), &primitives) ||
            !json_u32(mc.find("primitive_stride"), &primitive_stride) ||
            !json_u32(mc.find("max_vertices"), &max_vertices) ||
            !json_u32(mc.find("max_primitives"), &max_primitives) ||
            !json_u32(mc.find("clip_count"), &clip_count) || !json_u32(mc.find("clip_offset"), &clip_offset) ||
            !json_u32(mc.find("cull_count"), &cull_count) || !json_u32(mc.find("cull_offset"), &cull_offset) ||
            !json_u32(mc.find("push_constant_bytes"), &push_bytes)) {
            return std::nullopt;
        }
        if (vertices != kMeshComputeHeaderBytes || primitive_stride != kMeshComputePrimitiveStride ||
            clip_offset != kMeshComputeVerticesOffset) {
            return std::nullopt;
        }
        if (max_vertices == 0 || max_vertices > kMeshComputeMaxVertices || max_primitives == 0 ||
            max_primitives > kMeshComputeMaxPrimitives || clip_count > kMeshComputeMaxClipCull ||
            cull_count > kMeshComputeMaxClipCull || vertex_stride == 0 || vertex_stride % 16 != 0 ||
            vertex_stride > kMeshComputeMaxStride) {
            return std::nullopt;
        }
        if (push_bytes != kMeshComputePush152 && push_bytes != kMeshComputePush128) return std::nullopt;
        const Json* params = mc.find("params");
        if (!params || params->kind != Json::Kind::object || params->object.size() > kMeshComputeMaxParams)
            return std::nullopt;
        std::map<std::uint32_t, std::uint32_t> param_offsets;
        for (const auto& [key, val] : params->object) {
            std::uint32_t loc = 0, off = 0;
            if (!json_key_u32(key, &loc) || !json_u32(&val, &off) || loc > kMeshComputeMaxLocation ||
                !param_offsets.emplace(loc, off).second) {
                return std::nullopt;  // bad key/offset, over-limit location, or duplicate ("0" vs "00")
            }
        }
        const std::uint32_t clip_vec4 = (clip_count + 3) / 4;
        const std::uint32_t cull_vec4 = (cull_count + 3) / 4;
        if (cull_offset != 16u * (1u + clip_vec4)) return std::nullopt;
        if (vertex_stride != 16u * (1u + clip_vec4 + cull_vec4 + std::uint32_t(param_offsets.size())))
            return std::nullopt;
        std::uint32_t k = 0;
        for (const auto& [loc, off] : param_offsets) {
            (void)loc;
            if (off != 16u * (1u + clip_vec4 + cull_vec4 + k)) return std::nullopt;
            ++k;
        }
        // All products in 64 bits before any narrowing: u32*u32 always fits u64.
        const std::uint64_t want_primitives =
            std::uint64_t(kMeshComputeVerticesOffset) + std::uint64_t(max_vertices) * vertex_stride;
        if (primitives != want_primitives) return std::nullopt;
        const std::uint64_t want_slot = want_primitives + std::uint64_t(max_primitives) * 16u;
        if (slot_bytes != want_slot || slot_bytes % 16 != 0 || slot_bytes > kMeshComputeMaxSlotBytes)
            return std::nullopt;
        layout.slot_bytes = slot_bytes;
        layout.vertices = vertices;
        layout.vertex_stride = vertex_stride;
        layout.primitives = primitives;
        layout.primitive_stride = primitive_stride;
        layout.max_vertices = max_vertices;
        layout.max_primitives = max_primitives;
        layout.clip_count = clip_count;
        layout.clip_offset = clip_offset;
        layout.cull_count = cull_count;
        layout.cull_offset = cull_offset;
        layout.params = std::move(param_offsets);
        layout.push_constant_bytes = push_bytes;
        const Json* addr = mc.find("address_dwords");
        const Json* tail_addr = mc.find("pc_tail_address_dwords");
        if (push_bytes == kMeshComputePush152) {
            std::uint32_t d0 = 0, d1 = 0;
            if (!addr || addr->kind != Json::Kind::array || addr->array.size() != 2 ||
                !json_u32(&addr->array[0], &d0) || !json_u32(&addr->array[1], &d1) || d0 != 36 || d1 != 37 ||
                (tail_addr && tail_addr->kind != Json::Kind::null)) {
                return std::nullopt;
            }
            layout.has_address_dwords = true;
            layout.address_dwords = {d0, d1};
        } else {
            std::uint32_t t0 = 0, t1 = 0;
            if (!addr || addr->kind != Json::Kind::null || !tail_addr ||
                tail_addr->kind != Json::Kind::array || tail_addr->array.size() != 2 ||
                !json_u32(&tail_addr->array[0], &t0) || !json_u32(&tail_addr->array[1], &t1) || t0 != 4 || t1 != 5) {
                return std::nullopt;
            }
            layout.uses_pc_tail = true;
            layout.pc_tail_address_dwords = {t0, t1};
        }
        if (const Json* fh = mc.find("f16_halves"); fh && fh->kind != Json::Kind::null) {
            if (fh->kind != Json::Kind::object) return std::nullopt;
            std::map<std::uint32_t, std::array<std::uint32_t, 2>> halves;
            std::set<std::uint32_t> used = layout.raw_locations();
            for (const auto& [key, val] : fh->object) {
                std::uint32_t param = 0, lo = 0, hi = 0;
                if (!json_key_u32(key, &param) || val.kind != Json::Kind::array || val.array.size() != 2 ||
                    !json_u32(&val.array[0], &lo) || !json_u32(&val.array[1], &hi) || lo == hi ||
                    lo > kMeshComputeMaxLocation || hi > kMeshComputeMaxLocation ||
                    !layout.params.count(param) || used.count(lo) || used.count(hi) ||
                    !halves.emplace(param, std::array<std::uint32_t, 2>{lo, hi}).second) {
                    return std::nullopt;
                }
                used.insert(lo);
                used.insert(hi);
            }
            layout.f16_halves = std::move(halves);
        }
        // The entry's top-level fields must agree with the layout, or the pair is not the translator's.
        if (entry.push_constant_bytes != layout.push_constant_bytes ||
            entry.max_vertices != layout.max_vertices || entry.max_primitives != layout.max_primitives ||
            entry.f16_halves != layout.f16_halves) {
            return std::nullopt;
        }
        if (entry.local_size[0] == 0 || entry.local_size[1] == 0 || entry.local_size[2] == 0 ||
            entry.local_size[0] > 1024 || entry.local_size[1] > 1024 || entry.local_size[2] > 1024 ||
            std::uint64_t(entry.local_size[0]) * entry.local_size[1] * entry.local_size[2] > 1024 ||
            entry.lds_bytes > (128u << 10)) {
            return std::nullopt;
        }
        return layout;
    }

    using LookupKey = std::pair<std::string_view, std::string_view>;
    struct KeyLess {
        using is_transparent = void;
        bool operator()(LookupKey a, LookupKey b) const { return a < b; }
    };
    using Entries = std::map<std::pair<std::string, std::string>, std::vector<ShaderEntry>, KeyLess>;

    static std::vector<const ShaderEntry*> lookup(const Entries& entries, std::string_view stage, std::string_view key) {
        std::vector<const ShaderEntry*> out;
        const auto it = entries.find(LookupKey{stage, key});
        if (it != entries.end())
            for (const ShaderEntry& e : it->second) out.push_back(&e);
        return out;
    }

    // Reads <dir>/index.json. With an empty prefix it replaces what was loaded (the main index); with a prefix it
    // adds its entries under "<prefix><name>", the path of their modules relative to the main folder.
    template<typename Fail>
    bool load_index(const std::filesystem::path& dir, const std::string& prefix, Fail&& fail) {
        const auto index = dir / "index.json";
        std::ifstream file(index, std::ios::binary);
        if (!file) {
            const auto path = index.u8string();
            return fail("cannot open " + std::string(path.begin(), path.end()));
        }
        const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        constexpr std::string_view utf8_bom = "\xef\xbb\xbf";
        std::string_view json(text);
        if (json.starts_with(utf8_bom)) json.remove_prefix(utf8_bom.size());
        Json root;
        if (!JsonReader(json).parse(root) || root.kind != Json::Kind::object) return fail("index.json is not a JSON object");
        if (prefix.empty()) {
            dir_ = dir;
            entries_.clear();
            fspx_entries_.clear();
            count_ = 0;
            fspx_count_ = 0;
            fspx_compute_count_ = 0;
            fspx_gfx_count_ = 0;
            bvh_count_ = 0;
            exe_count_ = 0;
            descriptor_hash_census_ = {};
            descriptor_table_census_ = {};
        }
        for (const auto& [name, meta] : root.object) {
            const Json* hash = meta.find("fssm_sha256");
            const Json* code_hash = meta.find("fspx_code_sha256");
            const Json* stage = meta.find("stage");
            const bool by_fssm = hash && hash->kind == Json::Kind::string;
            const bool by_fspx = code_hash && code_hash->kind == Json::Kind::string;
            if ((!by_fssm && !by_fspx) || !stage || stage->kind != Json::Kind::string) continue;
            ShaderEntry e;
            e.name = prefix + name;
            if (const Json* spv = meta.find("spv"); spv && spv->kind == Json::Kind::string && !spv->string.empty())
                e.spv_file = prefix + spv->string;
            e.stage = stage->string;
            if (by_fssm) e.fssm_sha256 = hash->string;
            if (by_fspx) e.fspx_code_sha256 = code_hash->string;
            if (const Json* s = meta.find("fspx_state_sha256"); s && s->kind == Json::Kind::string)
                e.fspx_state_sha256 = s->string;
            if (const Json* b = meta.find("bindings"); b && b->kind == Json::Kind::array) e.binding_count = b->array.size();
            e.push_constant_bytes = u32(meta.find("push_constant_bytes"));
            e.lds_bytes = u32(meta.find("lds_bytes"));
            if (const Json* l = meta.find("local_size"); l && l->kind == Json::Kind::array)
                for (std::size_t i = 0; i < l->array.size() && i < 3; ++i) e.local_size[i] = u32(&l->array[i]);
            // The paired FSPX translator emits uses_code_base only when true. Older
            // key-only indexes have no such contract; retain their program bytes.
            e.code_base_known = !e.fspx_state_sha256.empty();
            if (const Json* c = meta.find("uses_code_base"); c && c->kind == Json::Kind::boolean) {
                e.uses_code_base = c->boolean;
                e.code_base_known = true;
            }
            if (const Json* w = meta.find("wave64"); w && w->kind == Json::Kind::boolean) e.wave64 = w->boolean;
            if (const Json* g = meta.find("subgroup_size"); g && g->kind == Json::Kind::number)
                e.subgroup_size = u32(g);
            if (const Json* x = meta.find("wave64_exchange"); x && x->kind == Json::Kind::string)
                e.wave64_exchange = x->string;
            if (const Json* r = meta.find("root_window"); r && r->kind == Json::Kind::object) {
                const Json* sgpr = r->find("sgpr");
                const Json* dword = r->find("dword");
                e.graphics_root_window = sgpr && dword && sgpr->kind == Json::Kind::number &&
                                         dword->kind == Json::Kind::number && u32(sgpr) == 16 && u32(dword) == 8;
            }
            if (const Json* wg = meta.find("wave64_graphics"); wg && wg->kind == Json::Kind::string)
                e.wave64_graphics = wg->string;
            if (const Json* s = meta.find("input_signature"); s && s->kind == Json::Kind::array) {
                for (const Json& item : s->array) {
                    SignatureElement element;
                    if (const Json* v = item.find("semantic"); v && v->kind == Json::Kind::string) element.semantic = v->string;
                    element.semantic_index = u32(item.find("semantic_index"));
                    element.register_index = u32(item.find("register"));
                    element.system_value = u32(item.find("system_value"));
                    if (const Json* v = item.find("component_type"); v && v->kind == Json::Kind::string)
                        element.component_type = v->string;
                    e.input_signature.push_back(std::move(element));
                }
            }
            if (const Json* in = meta.find("inputs"); in && in->kind == Json::Kind::array)
                for (const Json& item : in->array) e.inputs.push_back(u32(&item));
            if (const Json* o = meta.find("outputs"); o && o->kind == Json::Kind::array)
                for (const Json& item : o->array) e.outputs.push_back(u32(&item));
            if (const Json* fh = meta.find("f16_halves"); fh && fh->kind == Json::Kind::object) {
                for (const auto& [key, val] : fh->object) {
                    if (key.empty() || val.kind != Json::Kind::array || val.array.size() != 2) continue;
                    std::uint64_t param = 0;
                    bool ok = true;
                    for (const char c : key) {
                        if (c < '0' || c > '9') {
                            ok = false;
                            break;
                        }
                        param = param * 10u + std::uint64_t(c - '0');
                        if (param > UINT32_MAX) {
                            ok = false;
                            break;
                        }
                    }
                    if (!ok) continue;
                    e.f16_halves[std::uint32_t(param)] = {u32(&val.array[0]), u32(&val.array[1])};
                }
            }
            if (const Json* l = meta.find("literal_samplers"); l && l->kind == Json::Kind::array) {
                for (const Json& item : l->array) {
                    if (item.kind != Json::Kind::array || item.array.size() != 4) continue;
                    std::array<std::uint32_t, 4> words{};
                    for (std::size_t i = 0; i < 4; ++i) words[i] = u32(&item.array[i]);
                    e.literal_samplers.push_back(words);
                }
            }
            e.max_vertices = u32(meta.find("max_vertices"));
            e.max_primitives = u32(meta.find("max_primitives"));
            // --mesh-compute: typed slot layout on logical "ms" only. Absent/null is a native mesh shader;
            // malformed fails closed (nullopt, never a half-validated layout the PSO could misread).
            if (e.stage == "ms") {
                if (const Json* mc = meta.find("mesh_compute"); mc && mc->kind == Json::Kind::object) {
                    e.mesh_compute = parse_mesh_compute_layout(*mc, e);
                }
            }
            // Which finalizer this module's shader probes the descriptor table with (descriptor_table.h). Absent, or not
            // a kind this runtime knows, is the old bare FNV-1a.
            if (const Json* dh = meta.find("descriptor_hash");
                dh && dh->kind == Json::Kind::string && dh->string == "fnv1a_fmix32")
                ++descriptor_hash_census_.fnv1a_fmix32;
            else
                ++descriptor_hash_census_.fnv1a;
            // Which entry layout this module reads (descriptor_table.h). Absent, or not a layout this runtime knows,
            // is the old plain one.
            if (const Json* dt = meta.find("descriptor_table");
                dt && dt->kind == Json::Kind::string && dt->string == "tagged")
                ++descriptor_table_census_.tagged;
            else
                ++descriptor_table_census_.plain;
            if (by_fspx) fspx_entries_[{e.stage, e.fspx_code_sha256}].push_back(e);
            if (by_fssm) entries_[{e.stage, e.fssm_sha256}].push_back(std::move(e));
            if (by_fspx && (prefix == "fspx/" || prefix == "library/")) ++fspx_count_;
            if (by_fspx && prefix == "fspx_compute/") ++fspx_compute_count_;
            if (by_fspx && prefix == "fspx_gfx/") ++fspx_gfx_count_;
            if (prefix == "bvh/") ++bvh_count_;
            if (prefix == "exe/") ++exe_count_;
            ++count_;
        }
        return true;
    }

    std::filesystem::path dir_;
    std::size_t count_ = 0;
    std::size_t fspx_count_ = 0;
    std::size_t fspx_compute_count_ = 0;
    std::size_t fspx_gfx_count_ = 0;
    std::size_t bvh_count_ = 0;
    std::size_t exe_count_ = 0;
    // Entries by (stage, fssm_sha256) and by (stage, fspx_code_sha256), in index order.
    Entries entries_;
    Entries fspx_entries_;
    DescriptorHashCensus descriptor_hash_census_;
    DescriptorTableCensus descriptor_table_census_;
};

} // namespace leonida
