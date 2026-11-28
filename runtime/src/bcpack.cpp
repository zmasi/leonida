// C++ port of tools/bcpack.py. The build derives the model and BCn format
// constants from that same reference.
#include "bcpack.h"
#include "bcpack_stream.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>

namespace leonida::bcpack {
namespace {
using namespace detail;

void color(uint8_t *dst, const int *e, int a, int b, int c, int d) {
  for (int i = 0; i < 2; ++i, e += 3) {
    const int rgb = (e[0] << 11) | (e[1] << 5) | e[2];
    dst[i * 2] = uint8_t(rgb);
    dst[i * 2 + 1] = uint8_t(rgb >> 8);
  }
  dst[4] = uint8_t((a & 15) | (b << 4));
  dst[5] = uint8_t((a >> 4) | (b & 240));
  dst[6] = uint8_t((c & 15) | (d << 4));
  dst[7] = uint8_t((c >> 4) | (d & 240));
}

struct Endpoints {
  int v[8]{};
};

bool rebuild_classic(unsigned codec, size_t i, const Symbols &a,
                     const Symbols &b, array<Endpoints, 256> &past,
                     uint8_t *dst, bool allow_fitted, Quality &quality) {
  const bool single = codec == 1 || codec == 4;
  if (a[1] || b[1])
    return false;
  const unsigned distance = unzig(single ? a[0] : interleave(a[0], b[0])) & 255;
  Endpoints e;
  const int count = codec == 1 ? 6 : codec == 3 ? 8 : codec == 4 ? 2 : 4;
  if (!distance) {
    if (codec != 1)
      fill_n(e.v, codec == 5 ? 4 : 2, 128);
  } else if (distance <= i) {
    e = past[(i - distance) & 255];
  } else {
    const unsigned slot = (i - distance) & 255;
    const Seed &seed = codec == 1   ? history1[slot]
                       : codec == 3 ? history3[slot]
                       : codec == 4 ? history4[slot]
                                    : history5[slot];
    if (seed.values[0] < 0 || (seed.fitted && !allow_fitted))
      return false;
    if (seed.fitted)
      quality = Quality::fitted;
    copy_n(seed.values, count, e.v);
  }
  const int alpha = codec == 1 ? 0 : codec == 5 ? 4 : 2;
  for (int k = 0; k < count; ++k) {
    const int coord = k - alpha;
    const int mask = coord < 0 ? 255 : coord % 3 == 1 ? 63 : 31;
    const int symbol = single      ? a[2 + k]
                       : coord < 0 ? (k % 2 ? b : a)[2 + k / 2]
                                   : (coord < 3 ? a : b)[7 + coord % 3];
    e.v[k] = (e.v[k] + unzig(symbol)) & mask;
  }
  past[i & 255] = e;
  for (int ch = 0; ch < alpha / 2; ++ch) {
    const auto *pairs = codec == 3 ? pair3 : codec == 4 ? pair4 : pair5;
    uint64_t bits = 0;
    for (int row = 0; row < 4; ++row) {
      const int lane =
          codec == 4 ? 4 + 2 * row : (codec == 3 ? 3 : 4 + 4 * ch) + row;
      const int right = single ? a[lane + 1] : b[lane];
      bits |= uint64_t(pairs[a[lane]] | (pairs[right] << 6)) << (12 * row);
    }
    *dst++ = uint8_t(e.v[ch * 2]);
    *dst++ = uint8_t(e.v[ch * 2 + 1]);
    for (int j = 0; j < 6; ++j)
      *dst++ = uint8_t(bits >> (8 * j));
  }
  if (codec == 1)
    color(dst, e.v, quad1[a[8]], quad1[a[9]], quad1[a[10]], quad1[a[11]]);
  if (codec == 3)
    color(dst, e.v + 2, quad3[a[10]], quad3[b[10]], quad3[a[11]], quad3[b[11]]);
  return true;
}

struct BitWriter {
  uint8_t *dst;
  int pos{}, violations{};
  void put(int value, int bits) {
    while (bits > 0) {
      const int n = min(bits, 8 - pos % 8);
      dst[pos / 8] |= uint8_t((value & ((1 << n) - 1)) << (pos % 8));
      value >>= n;
      bits -= n;
      pos += n;
    }
  }
};

// Endpoints and indices have independent predictors, with distances limited to
// eight bits.
struct BptcState {
  array<int, 24> ep{};
  array<int, 32> ix{};
  int ep_root = -1, ix_root = -1;
};
int mod_bits(int v, int n) { return v & ((1 << n) - 1); }

void index_row(int a, int b, int row, int bits, const int *ref, int *value,
               int *history) {
  for (int x = 0; x < 4; ++x) {
    const int s = (x < 2 ? a : b) >> ((x % 2) ^ (row < 3));
    const int delta =
        (s & 1) | ((s >> 1) & 2) | ((s >> 2) & 4) | ((s >> 3) & 8);
    const int p = 4 * row + x;
    value[p] = mod_bits((ref[p] >> (4 - bits)) + unzig(delta), bits);
    history[p] = value[p] << (4 - bits);
  }
}

bool put_indices(BitWriter &writer, const int *values, int bits,
                 int anchor1 = -1, int anchor2 = -1) {
  for (int p = 0; p < 16; ++p) {
    const int n = bits - (p == 0 || p == anchor1 || p == anchor2);
    if (values[p] >> n)
      ++writer.violations;
    writer.put(values[p], n);
  }
  return true;
}

bool bc6(const Symbols &a, const Symbols &b, const BptcState &ep,
         const BptcState &ix, BptcState &next, BitWriter &writer) {
  if (a[15] >= 14)
    return false;
  const auto &m = bc6Modes[a[15]];
  const int partition = min<int>(b[15], 31), base_shift = 16 - m.base;
  int fields[3][4]{};
  for (int c = 0; c < 3; ++c) {
    const int n = m.delta[c], shift = max(0, 6 - n);
    const auto *ref = &ep.ep[c * 4];
    const int w = mod_bits((ref[0] >> base_shift) +
                               unzig(interleave(a[2 + c], b[2 + c], 8)),
                           m.base);
    const int x = mod_bits(
        (ref[1] >> (10 - n)) + unzig(interleave(a[5 + c], b[5 + c], 5)), n);
    const int y = mod_bits((ref[2] >> shift) + unzig(a[8 + c]), n);
    const int z = mod_bits((ref[3] >> shift) + unzig(b[8 + c]), n);
    auto *state = &next.ep[c * 4];
    state[0] = w << base_shift;
    state[1] = x << (10 - n);
    if (m.regions == 2) {
      state[2] = y << shift;
      state[3] = z << shift;
    } else {
      const int v = mod_bits(w, n) ^ (m.transformed << (n - 1));
      state[2] = n >= 6 ? v >> (n - 6) : v << shift;
      state[3] = state[1] >> 4;
    }
    fields[c][0] = w;
    if (a[15] >= 12) {
      fields[c][0] &= 1023;
      for (int bit = 10; bit < m.base; ++bit)
        fields[c][0] |= ((w >> bit) & 1) << (m.base + 9 - bit);
    }
    fields[c][1] = x;
    fields[c][2] = y;
    fields[c][3] = z;
  }
  writer.put(m.code, m.code < 2 ? 2 : 5);
  for (int j = 0; j < m.count; ++j) {
    const auto &f = bc6Plan[m.first + j];
    writer.put((f[0] < 0 ? partition : fields[f[0]][f[1]]) >> f[2], 1);
  }
  int values[16];
  const int bits = m.regions == 2 ? 3 : 4;
  for (int y = 0; y < 4; ++y)
    index_row(a[11 + y], b[11 + y], y, bits, ix.ix.data(), values,
              next.ix.data());
  return put_indices(writer, values, bits,
                     m.regions == 2 ? anchor2[partition] : -1);
}

bool bc7(const Symbols &a, const Symbols &b, const BptcState &ep,
         const BptcState &ix, BptcState &next, BitWriter &writer) {
  if (a[15] >= 32 || b[15] >= 16)
    return false;
  const auto &cfg = bc7Config[a[15] * 16 + b[15]];
  const auto [tag, part, rot, isel] = cfg;
  const int mode = tag & 127;
  if (mode > 7 || part >= 64)
    return false;
  const auto &f = bc7_formats[mode];
  int widths[4] = {f.color, f.color, f.color, f.alpha};
  if (rot)
    swap(widths[rot - 1], widths[3]);
  constexpr int lane[3][4] = {{2, 3, 4, 5}, {6, 7, 8, 9}, {10, 5, 9, -1}};
  int v[3][2][4]{};
  for (int sub = 0; sub < f.subsets; ++sub) {
    for (int end = 0; end < 2; ++end) {
      for (int c = 0; c < 4; ++c) {
        if (!widths[c] || lane[sub][c] < 0)
          continue;
        const int n = widths[c] + (f.pbits && c == 0);
        const int k = 8 * sub + 4 * end + c;
        v[sub][end][c] = mod_bits(
            (ep.ep[k] >> (8 - n)) + unzig((end ? b : a)[lane[sub][c]]), n);
        next.ep[k] = v[sub][end][c] << (8 - n);
      }
    }
    if (f.pbits == 2 && ((v[sub][0][0] ^ v[sub][1][0]) & 1))
      ++writer.violations;
  }
  for (int sub = f.subsets; sub < 3; ++sub)
    copy_n(next.ep.begin(), 8, next.ep.begin() + 8 * sub);
  int primary[16], secondary[16];
  constexpr int second_rows[] = {6, 7, 8, 10};
  const bool two = mode == 4 || mode == 5;
  for (int y = 0; y < 4; ++y) {
    index_row(a[11 + y], b[11 + y], y, f.index, ix.ix.data(), primary,
              next.ix.data());
    if (two)
      index_row(a[second_rows[y]], b[second_rows[y]], y, 2, ix.ix.data() + 16,
                secondary, next.ix.data() + 16);
  }
  if (mode == 0 && part == 0 &&
      ranges::all_of(next.ep, [](int v) { return !v; }) &&
      ranges::all_of(primary, [](int v) { return !v; }))
    return true;

  writer.put(1 << mode, mode + 1);
  writer.put(part, f.partition);
  if (two)
    writer.put(rot, 2);
  if (mode == 4)
    writer.put(isel, 1);
  for (int c = 0; c < 4; ++c) {
    const int src = rot && c == rot - 1 ? 3 : rot && c == 3 ? rot - 1 : c;
    for (int sub = 0; sub < f.subsets; ++sub)
      for (int end = 0; end < 2; ++end)
        writer.put(v[sub][end][src] >> (f.pbits && src == 0),
                   c == 3 ? f.alpha : f.color);
  }
  if (f.pbits) {
    for (int sub = 0; sub < f.subsets; ++sub)
      for (int end = 0; end < (f.pbits == 1 ? 2 : 1); ++end)
        writer.put(v[sub][end][0], 1);
  }
  const int anc1 = f.subsets == 2   ? anchor2[part]
                   : f.subsets == 3 ? anchor3a[part]
                                    : -1;
  const int anc2 = f.subsets == 3 ? anchor3b[part] : -1;
  if (mode == 4)
    return put_indices(writer, secondary, 2) &&
           put_indices(writer, primary, f.index);
  return put_indices(writer, primary, f.index, anc1, anc2) &&
         (!two || put_indices(writer, secondary, 2));
}

// A predictor can influence later blocks through either independent reference
// chain. A bit-depth gap discovered during the first 256 blocks also affects
// earlier uses of the slot.
struct Coverage {
  array<bool, 256> missing_ep{}, missing_ix{};
  size_t lost{}, inconsistent{}, limit{}, lost_limit{};
  bool lost_endpoints{}, lost_indices{};
  void note_missing(const BptcState &block) {
    const bool ep = block.ep_root >= 0 && missing_ep[block.ep_root];
    const bool ix = block.ix_root >= 0 && missing_ix[block.ix_root];
    lost += ep || ix;
    lost_endpoints |= ep;
    lost_indices |= ix;
  }
};

bool rebuild_bptc(unsigned codec, size_t i, size_t blocks, const Symbols &a,
                  const Symbols &b, array<BptcState, 256> &past, uint8_t *dest,
                  const Calibration *history, bool allow_fitted,
                  Quality &quality, Coverage &coverage, const char*& reason) {
  const unsigned di = interleave(a[1], b[1]);
  const unsigned de = (di + unzig(interleave(a[0], b[0]))) & 255;
  if ((di > i || de > i) && !history) {
    reason = "missing-profile";
    return false;
  }
  const int islot = di > i ? int(256 + i - di) : -1;
  const int eslot = de > i ? int(256 + i - de) : -1;
  bool fitted = false;
  BptcState zero{};
  if (codec == 6) {
    for (int c = 0; c < 3; ++c) {
      zero.ep[4 * c] = 32768;
      zero.ep[4 * c + 1] = 512;
      zero.ep[4 * c + 2] = zero.ep[4 * c + 3] = 32;
    }
    zero.ix.fill(8);
  }
  if (codec == 6) {
    if (a[15] >= 14)
      return false;
    const auto &m = bc6Modes[a[15]];
    if (eslot >= 0) {
      const auto &row = history->bc6[eslot];
      for (int c = 0; c < 3; ++c) {
        const int needs[] = {m.base, m.delta[c],
                             m.regions == 2 ? m.delta[c] : 0,
                             m.regions == 2 ? m.delta[c] : 0};
        for (int k = 0; k < 4; ++k) {
          const int j = 4 * c + k;
          zero.ep[j] = row.ep[2 * j] | (row.ep[2 * j + 1] << 8);
          fitted |= row.ep_confirmed[j] < needs[k];
        }
      }
      fitted |= row.ep_class != 3;
      coverage.missing_ep[eslot] |= !row.ep_class;
    }
    if (islot >= 0) {
      const auto &row = history->bc6[islot];
      copy(row.ix.begin(), row.ix.end(), zero.ix.begin());
      fitted |=
          row.ix_class != 3 || ranges::any_of(row.ix_confirmed, [&](int n) {
            return n < (m.regions == 2 ? 3 : 4);
          });
      coverage.missing_ix[islot] |= !row.ix_class;
    }
  } else {
    if (a[15] >= 32 || b[15] >= 16)
      return false;
    const auto &cfg = bc7Config[a[15] * 16 + b[15]];
    const unsigned mode = cfg[0] & 127;
    if (mode >= 8 || cfg[1] >= 64)
      return false;
    if (cfg[0] & 128)
      fitted = true;
    const auto &format = bc7_formats[mode];
    if (eslot >= 0) {
      const auto &row = history->bc7[eslot];
      copy(row.ep.begin(), row.ep.end(), zero.ep.begin());
      int width[] = {format.color, format.color, format.color, format.alpha};
      if (cfg[2])
        swap(width[3], width[cfg[2] - 1]);
      width[0] += format.pbits != 0;
      for (int j = 0; j < 8 * format.subsets; ++j) {
        const int need = width[j % 4];
        fitted |= row.ep_confirmed[j] < need || row.ep_bits[j] < need;
        coverage.missing_ep[eslot] |= row.ep_bits[j] < min(need, 4);
      }
    }
    if (islot >= 0) {
      const auto &row = history->bc7[islot];
      copy(row.ix.begin(), row.ix.end(), zero.ix.begin());
      for (int j = 0; j < (mode == 4 || mode == 5 ? 32 : 16); ++j) {
        int need = j < 16 ? format.index : 2;
        fitted |= row.ix_confirmed[j] < need || row.ix_bits[j] < need;
        coverage.missing_ix[islot] |= row.ix_bits[j] < need;
      }
    }
  }
  if (fitted) {
    if (!allow_fitted) {
      reason = "unconfirmed-predictor";
      return false;
    }
    quality = Quality::fitted;
  }
  const auto &ep = de && eslot < 0 ? past[(i - de) & 255] : zero;
  const auto &ix = di && islot < 0 ? past[(i - di) & 255] : zero;
  auto &next = past[i & 255];
  next = {};
  next.ep_root = eslot >= 0 ? eslot : ep.ep_root;
  next.ix_root = islot >= 0 ? islot : ix.ix_root;
  fill_n(dest, 16, 0);
  BitWriter writer{dest};
  const bool valid = codec == 6 ? bc6(a, b, ep, ix, next, writer)
                                : bc7(a, b, ep, ix, next, writer);
  coverage.inconsistent +=
      codec == 6 ? writer.violations : int(writer.violations != 0);
  if (i + 1 == min<size_t>(256, blocks)) {
    for (size_t k = 0; k <= i; ++k)
      coverage.note_missing(past[k]);
  } else if (i >= 256)
    coverage.note_missing(next);
  if (!valid) return false;
  if (coverage.lost > coverage.lost_limit) {
    reason = coverage.lost_endpoints && coverage.lost_indices ? "missing-endpoint-and-index" :
             coverage.lost_endpoints ? "missing-endpoint" : "missing-index";
    return false;
  }
  if (coverage.inconsistent > coverage.limit) { reason = "anchor-or-pbit"; return false; }
  return true;
}
} // namespace

Quality decode(unsigned codec, std::span<const uint8_t> source,
               std::span<uint8_t> output, bool allow_fitted,
               const Calibration *history, const char** failure, bool fast_entropy, size_t lost_divisor,
               Report *report) {
  const char* reason = "bcpack-stream";
  if (failure) *failure = reason;
  Quality quality = Quality::confirmed;
  if (!codec || codec == 2 || codec > 7)
    return Quality::failed;
  const bool single = codec == 1 || codec == 4;
  const unsigned block_size = single ? 8 : 16;
  if (output.size() % block_size)
    return Quality::failed;
  array<Endpoints, 256> classic{};
  array<BptcState, 256> bptc{};
  Coverage coverage{};
  coverage.limit = allow_fitted && history
                       ? max<size_t>(2, output.size() / block_size / 1024)
                       : 0;
  coverage.lost_limit = allow_fitted && history
                            ? max<size_t>(2, output.size() / block_size / max<size_t>(1, lost_divisor))
                            : 0;
  if (report) *report = {output.size() / block_size, 0};
  const bool valid = visit_blocks(codec, source, output.size(), [&](size_t i, const Symbols &first, const Symbols &tuple) {
    auto *dst = output.data() + i * block_size;
    if (codec >= 6)
      return rebuild_bptc(codec, i, output.size() / block_size, first, tuple, bptc, dst, history, allow_fitted, quality, coverage, reason);
    return rebuild_classic(codec, i, first, tuple, classic, dst, allow_fitted, quality);
  }, fast_entropy);
  if (report) report->lost = coverage.lost;
  if (valid && (!coverage.inconsistent || quality == Quality::fitted)) return quality;
  if (failure) *failure = coverage.inconsistent && valid ? "anchor-or-pbit" : reason;
  return Quality::failed;
}
} // namespace leonida::bcpack
