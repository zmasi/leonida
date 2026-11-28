// C++ BCPack decoder. Publish the caller's staged output only on success.
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace leonida::bcpack {
enum class Quality { failed, confirmed, fitted };

// Fixed-size little-endian sidecar produced by
// tools/gen_bcpack_runtime_tables.py --profile. All fields are bytes: BC6H's 12
// endpoint quantities are stored as little-endian uint16 pairs.
struct Calibration {
  struct BC6 {
    std::array<std::uint8_t, 24> ep;
    std::array<std::uint8_t, 16> ix;
    std::array<std::uint8_t, 12> ep_confirmed;
    std::array<std::uint8_t, 16> ix_confirmed;
    std::uint8_t ep_class, ix_class;
  };
  struct BC7 {
    std::array<std::uint8_t, 24> ep, ep_bits, ep_confirmed;
    std::array<std::uint8_t, 32> ix, ix_bits, ix_confirmed;
  };
  std::array<BC6, 256> bc6;
  std::array<BC7, 256> bc7;
};
static_assert(sizeof(Calibration) == 256 * (70 + 168));
// What a BC6H/BC7 decode had to approximate: blocks whose endpoint or index predictor chain starts at a history
// entry the profile does not know ("lost"), out of all blocks of the request.
struct Report {
  std::size_t blocks = 0, lost = 0;
};

// lost_divisor: in fitted mode with a profile, a BC6H/BC7 request may have up to max(2, blocks / lost_divisor) lost
// blocks; they are rebuilt from the bits the profile has, so their colours can be wrong. More than that fails as
// missing-endpoint / missing-index. 1024 is tools/bcpack.py's _loose_limit. Inconsistent blocks (anchor or p-bit)
// keep the 1-in-1,024 limit whatever this is.
Quality decode(unsigned codec, std::span<const std::uint8_t> source,
               std::span<std::uint8_t> output, bool allow_fitted = false,
               const Calibration *history = nullptr, const char** failure = nullptr,
               bool fast_entropy = true, std::size_t lost_divisor = 1024, Report *report = nullptr);
} // namespace leonida::bcpack
