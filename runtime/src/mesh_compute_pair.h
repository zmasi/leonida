#pragma once

// Mesh-as-compute slot layout and producer/companion pair checks (MESH_COMPUTE_D3D12_PLAN.md step 1).
//
// On a device without VK_EXT_mesh_shader, rdna2spirv.py --mesh-compute turns each mesh shader into a GLCompute
// module with the same workgroup size. Each workgroup writes its counts, vertices and primitives into a slot of
// an output buffer; tools/mesh_compute_vs.py writes the Vertex shader that draws those slots. The slot byte
// layout is the index's "mesh_compute" object; this header carries its validated form plus the SPIR-V execution
// and interface checks a PSO needs before creating either half. Pure C++20 stdlib, no Vulkan or Windows headers.

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace leonida {

// Byte layout constants matching tools/rdna2spirv.py mesh_compute_layout().
inline constexpr std::uint32_t kMeshComputeHeaderBytes = 16;
inline constexpr std::uint32_t kMeshComputeVerticesOffset = 16;
inline constexpr std::uint32_t kMeshComputePrimitiveStride = 16;
inline constexpr std::uint32_t kMeshComputePush152 = 152;
inline constexpr std::uint32_t kMeshComputePush128 = 128;
inline constexpr std::uint32_t kMeshComputeAddressDword0 = 36;
inline constexpr std::uint32_t kMeshComputeAddressDword1 = 37;
inline constexpr std::uint32_t kMeshComputePcTailDword0 = 4;
inline constexpr std::uint32_t kMeshComputePcTailDword1 = 5;
// Companion module next to the producer: <name>.mesh_vs.spv (tools/mesh_compute_vs.py SUFFIX).
inline constexpr const char* kMeshComputeCompanionSuffix = ".mesh_vs";
// Versioned seal binding the validated layout to both module bytes (<name>.mesh_compute.seal.json, written by
// rdna2spirv.py at generation and completed by mesh_compute_vs.py; see canonical_mesh_compute_json). Version 1
// only; any other version fails closed. Must match tools/mesh_compute_vs.py MESH_COMPUTE_SEAL_VERSION byte for
// byte (canonical JSON + lowercase hex SHA-256).
inline constexpr int kMeshComputeSealVersion = 1;
inline constexpr const char* kMeshComputeSealSuffix = ".mesh_compute.seal.json";

// Absolute sanity caps for on-disk validation (fail closed on absurd; the PSO applies the device's own limits).
inline constexpr std::uint64_t kMeshComputeMaxSlotBytes = std::uint64_t(1) << 24;  // 16 MiB per workgroup slot
inline constexpr std::uint32_t kMeshComputeMaxVertices = 65535;
inline constexpr std::uint32_t kMeshComputeMaxPrimitives = 65535;
inline constexpr std::uint32_t kMeshComputeMaxClipCull = 8;
inline constexpr std::uint32_t kMeshComputeMaxStride = 4096;
inline constexpr std::uint32_t kMeshComputeMaxLocation = 63;
inline constexpr std::uint32_t kMeshComputeMaxParams = 32;

// GPU-driven indirect tiles (tools/rdna2spirv.py MESH_INDIRECT_TILE_MARKER). A regenerated 152-byte producer that
// supports them carries exactly one OpModuleProcessed with kMeshComputeIndirectTileMarker in its SHA-sealed words;
// an older sealed producer does not and stays direct-only. The Tile/TileMeta layout, tagging and alignment rules are
// the canonical ABI in vulkan_mesh_indirect_tiles.h; this header only recognizes the marker.
inline constexpr const char* kMeshComputeIndirectTileMarker = "leonida.mesh_indirect_tile.v1";

// One validated --mesh-compute slot layout. Present only for logical stage "ms" with a well-formed object;
// absent/null (native mesh) and malformed (fails closed) both read as std::nullopt in ShaderEntry.
struct MeshComputeLayout {
    std::uint64_t slot_bytes = 0;
    std::uint32_t vertices = 0;  // always 16: the header is 16 bytes
    std::uint32_t vertex_stride = 0;
    std::uint64_t primitives = 0;  // byte offset of the primitive array
    std::uint32_t primitive_stride = 0;  // always 16
    std::uint32_t max_vertices = 0;
    std::uint32_t max_primitives = 0;
    std::uint32_t clip_count = 0;
    std::uint32_t clip_offset = 0;  // always 16
    std::uint32_t cull_count = 0;
    std::uint32_t cull_offset = 0;
    // Param location -> byte offset within one vertex (ascending locations, consecutive 16-byte slots).
    std::map<std::uint32_t, std::uint32_t> params;
    std::uint32_t push_constant_bytes = 0;  // 152 (supported) or 128 (pc-tail, explicitly refused this wave)
    bool uses_pc_tail = false;
    std::array<std::uint32_t, 2> address_dwords{0, 0};  // [36,37] when 152-byte
    bool has_address_dwords = false;
    std::array<std::uint32_t, 2> pc_tail_address_dwords{0, 0};  // [4,5] when 128-byte
    // Static --f16-interp-fallback halves (param -> [low location, high location]), as the companion was built.
    std::map<std::uint32_t, std::array<std::uint32_t, 2>> f16_halves;

    // This wave's only supported producer/companion contract: output address in push dwords 36..37 (152 bytes).
    bool is_supported_push() const {
        return !uses_pc_tail && push_constant_bytes == kMeshComputePush152 && has_address_dwords &&
               address_dwords[0] == kMeshComputeAddressDword0 && address_dwords[1] == kMeshComputeAddressDword1;
    }
    // The 128-byte set-1 UBO variant (address at tail dwords 4..5): well-formed on disk but with no verified
    // companion in this baseline, so the PSO must refuse it explicitly rather than render it wrong.
    bool is_pc_tail() const {
        return uses_pc_tail && push_constant_bytes == kMeshComputePush128 && !has_address_dwords &&
               pc_tail_address_dwords[0] == kMeshComputePcTailDword0 &&
               pc_tail_address_dwords[1] == kMeshComputePcTailDword1;
    }
    // Raw param locations the companion exports (before f16 linking); for link_f16_halves + the bary GS helper.
    std::set<std::uint32_t> raw_locations() const {
        std::set<std::uint32_t> out;
        for (const auto& [loc, off] : params) {
            (void)off;
            out.insert(loc);
        }
        return out;
    }
    std::set<std::uint32_t> half_locations() const {
        std::set<std::uint32_t> out;
        for (const auto& [param, pair] : f16_halves) {
            (void)param;
            out.insert(pair[0]);
            out.insert(pair[1]);
        }
        return out;
    }
};

// Parsed v1 seal (<name>.mesh_compute.seal.json): hashes only. The seal file also carries a human-readable
// "layout" object, but verification recomputes the canonical hash from the validated MeshComputeLayout and never
// trusts the seal's copy. Companion hashes are nullopt when absent (translator partial seal, or 128B pc-tail which
// never gains a companion). All hashes are lowercase hex SHA-256; malformed/unknown versions fail closed.
struct MeshComputeSeal {
    int version = 0;
    std::string layout_sha256;
    std::string producer_sha256;
    std::optional<std::string> companion_asm_sha256;
    std::optional<std::string> companion_spv_sha256;
    bool has_companion() const { return companion_asm_sha256 && companion_spv_sha256; }
};

// Deterministic canonical JSON of a validated layout, byte-identical to Python
// `json.dumps(normalized, sort_keys=True, separators=(",", ":"))` in tools/mesh_compute_vs.py (same sort_keys compact).
// Top-level keys alphabetical, params/halves keys
// as strings sorted lexicographically (so "10" < "2"), no spaces, null for absent address lists, {} for absent
// halves. SHA-256 of its UTF-8 bytes is the sealed layout identity.
inline std::string canonical_mesh_compute_json(const MeshComputeLayout& layout) {
    std::string out;
    out.reserve(256 + layout.params.size() * 16 + layout.f16_halves.size() * 24);
    out += "{\"address_dwords\":";
    if (layout.has_address_dwords) {
        out += "[";
        out += std::to_string(layout.address_dwords[0]);
        out += ",";
        out += std::to_string(layout.address_dwords[1]);
        out += "]";
    } else {
        out += "null";
    }
    out += ",\"clip_count\":";
    out += std::to_string(layout.clip_count);
    out += ",\"clip_offset\":";
    out += std::to_string(layout.clip_offset);
    out += ",\"cull_count\":";
    out += std::to_string(layout.cull_count);
    out += ",\"cull_offset\":";
    out += std::to_string(layout.cull_offset);
    out += ",\"f16_halves\":{";
    {
        std::vector<std::string> keys;
        keys.reserve(layout.f16_halves.size());
        for (const auto& [param, pair] : layout.f16_halves) {
            (void)pair;
            keys.push_back(std::to_string(param));
        }
        std::sort(keys.begin(), keys.end());
        bool first = true;
        for (const std::string& key : keys) {
            std::uint32_t param = 0;
            for (const char c : key) param = param * 10u + std::uint32_t(c - '0');
            const auto& pair = layout.f16_halves.find(param)->second;
            if (!first) out += ",";
            first = false;
            out += "\"";
            out += key;
            out += "\":[";
            out += std::to_string(pair[0]);
            out += ",";
            out += std::to_string(pair[1]);
            out += "]";
        }
    }
    out += "},\"max_primitives\":";
    out += std::to_string(layout.max_primitives);
    out += ",\"max_vertices\":";
    out += std::to_string(layout.max_vertices);
    out += ",\"params\":{";
    {
        std::vector<std::string> keys;
        keys.reserve(layout.params.size());
        for (const auto& [loc, off] : layout.params) {
            (void)off;
            keys.push_back(std::to_string(loc));
        }
        std::sort(keys.begin(), keys.end());
        bool first = true;
        for (const std::string& key : keys) {
            std::uint32_t loc = 0;
            for (const char c : key) loc = loc * 10u + std::uint32_t(c - '0');
            if (!first) out += ",";
            first = false;
            out += "\"";
            out += key;
            out += "\":";
            out += std::to_string(layout.params.find(loc)->second);
        }
    }
    out += "},\"pc_tail_address_dwords\":";
    if (layout.uses_pc_tail) {
        out += "[";
        out += std::to_string(layout.pc_tail_address_dwords[0]);
        out += ",";
        out += std::to_string(layout.pc_tail_address_dwords[1]);
        out += "]";
    } else {
        out += "null";
    }
    out += ",\"primitive_stride\":";
    out += std::to_string(layout.primitive_stride);
    out += ",\"primitives\":";
    out += std::to_string(layout.primitives);
    out += ",\"push_constant_bytes\":";
    out += std::to_string(layout.push_constant_bytes);
    out += ",\"slot_bytes\":";
    out += std::to_string(layout.slot_bytes);
    out += ",\"vertex_stride\":";
    out += std::to_string(layout.vertex_stride);
    out += ",\"vertices\":";
    out += std::to_string(layout.vertices);
    out += "}";
    return out;
}

namespace mesh_compute_detail {

// SPIR-V ids used below (spirv/unified1/spirv.h). Kept local so this header stays dependency-free.
inline constexpr std::uint32_t kExecVertex = 0;
inline constexpr std::uint32_t kExecGLCompute = 5;
inline constexpr std::uint32_t kExecMeshNV = 5268;
inline constexpr std::uint32_t kExecMeshEXT = 5365;
inline constexpr std::uint32_t kExecTaskNV = 5267;
inline constexpr std::uint32_t kExecTaskEXT = 5364;
inline constexpr std::uint32_t kCapInt64 = 11;
inline constexpr std::uint32_t kCapClipDistance = 32;
inline constexpr std::uint32_t kCapCullDistance = 33;
inline constexpr std::uint32_t kCapMeshShadingEXT = 5283;
inline constexpr std::uint32_t kCapMeshShadingNV = 5282;
inline constexpr std::uint32_t kCapPhysicalStorageBuffer = 5347;
inline constexpr std::uint16_t kOpEntryPoint = 15;
inline constexpr std::uint16_t kOpExecutionMode = 16;
inline constexpr std::uint16_t kOpCapability = 17;
inline constexpr std::uint16_t kOpVariable = 59;
inline constexpr std::uint16_t kOpDecorate = 71;
inline constexpr std::uint16_t kOpMemberDecorate = 72;
inline constexpr std::uint16_t kOpExecutionModeId = 331;
inline constexpr std::uint16_t kOpModuleProcessed = 330;
inline constexpr std::uint32_t kExecModeLocalSize = 17;
inline constexpr std::uint32_t kExecModeLocalSizeId = 38;
inline constexpr std::uint32_t kStorageOutput = 3;
inline constexpr std::uint32_t kDecLocation = 30;
inline constexpr std::uint32_t kDecOffset = 35;

// True when the OpModuleProcessed at words[pos] (word count wc) is exactly `marker`: its literal bytes, then a NUL,
// then zero padding to the end of the instruction (no extra word). Compared in place, no allocation.
inline bool module_processed_is(const std::uint32_t* words, std::size_t wc, const char* marker) {
    std::size_t n = 0;
    while (marker[n]) ++n;
    if (wc < 2 || (wc - 1) * 4 != ((n + 4) & ~std::size_t(3))) return false;  // n bytes + NUL, whole words, no spare word
    for (std::size_t i = 0; i < (wc - 1) * 4; ++i) {
        const char ch = char((words[1 + i / 4] >> (8 * (i % 4))) & 0xffu);
        if (ch != (i < n ? marker[i] : '\0')) return false;
    }
    return true;
}

struct Scan {
    bool valid = false;
    std::vector<std::uint32_t> exec_models;
    std::vector<std::pair<std::uint32_t, std::uint32_t>> entries;  // (entry id, exec model)
    struct LocalSize {
        std::uint32_t entry = 0, x = 0, y = 0, z = 0;
    };
    std::vector<LocalSize> local_sizes;  // OpExecutionMode LocalSize literals
    bool has_local_size_id = false;      // OpExecutionModeId LocalSizeId (explicitly refused)
    std::set<std::uint32_t> capabilities;
    // Output variable id -> storage class (globals only, before the first function).
    std::map<std::uint32_t, std::uint32_t> output_vars;
    // Location decorations on Output variables: location -> variable id (first wins).
    std::map<std::uint32_t, std::uint32_t> output_locations;
    std::set<std::uint32_t> member_offsets;
    bool has_function = false;
    // Global OpModuleProcessed instructions that are exactly kMeshComputeIndirectTileMarker.
    std::uint32_t indirect_tile_markers = 0;
};

inline Scan scan_spirv(std::span<const std::uint32_t> words) {
    Scan out;
    if (words.size() < 5 || words[0] != 0x07230203u || words[3] == 0) return out;
    std::size_t pos = 5;
    std::map<std::uint32_t, std::uint32_t> var_storage;
    struct Dec {
        std::uint32_t target, dec, lit;
    };
    std::vector<Dec> decorations;
    bool ok = true;
    while (pos < words.size()) {
        const std::uint32_t head = words[pos];
        const std::uint32_t wc = head >> 16;
        const std::uint16_t op = std::uint16_t(head & 0xffffu);
        if (wc == 0 || pos + wc > words.size()) {
            ok = false;
            break;
        }
        const auto at = [&](std::size_t i) { return words[pos + i]; };
        if (op == kOpCapability && wc >= 2) {
            out.capabilities.insert(at(1));
        } else if (op == kOpEntryPoint && wc >= 4) {
            out.exec_models.push_back(at(1));
            out.entries.emplace_back(at(2), at(1));
        } else if (op == kOpExecutionMode && wc >= 3 && at(2) == kExecModeLocalSize && wc >= 6) {
            out.local_sizes.push_back({at(1), at(3), at(4), at(5)});
        } else if (op == kOpExecutionModeId && wc >= 3 && at(2) == kExecModeLocalSizeId) {
            out.has_local_size_id = true;
        } else if (op == kOpVariable && wc >= 4 && !out.has_function) {
            var_storage[at(2)] = at(3);
        } else if (op == kOpDecorate && wc >= 4) {
            decorations.push_back({at(1), at(2), at(3)});
        } else if (op == kOpMemberDecorate && wc >= 5 && at(3) == kDecOffset) {
            out.member_offsets.insert(at(4));
        } else if (op == kOpModuleProcessed && !out.has_function) {
            if (module_processed_is(&words[pos], wc, kMeshComputeIndirectTileMarker)) ++out.indirect_tile_markers;
        } else if (op == 54) {  // OpFunction: globals end here
            out.has_function = true;
        }
        pos += wc;
    }
    if (!ok) return Scan{};
    for (const auto& [id, storage] : var_storage)
        if (storage == kStorageOutput) out.output_vars[id] = storage;
    for (const auto& d : decorations) {
        if (d.dec == kDecLocation && out.output_vars.count(d.target)) out.output_locations.emplace(d.lit, d.target);
    }
    out.valid = true;
    return out;
}

inline bool is_mesh_exec(std::uint32_t model) {
    return model == kExecMeshNV || model == kExecMeshEXT || model == kExecTaskNV || model == kExecTaskEXT;
}

}  // namespace mesh_compute_detail

// The producer must be exactly one GLCompute entry, with 64-bit buffer addressing and no mesh/task capability
// or execution model. False (with *error when given) for malformed words or anything else.
inline bool mesh_producer_scan_is_compute(const mesh_compute_detail::Scan& scan, std::string* error) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!scan.valid) return fail("mesh_compute: malformed producer SPIR-V");
    if (scan.exec_models.size() != 1 || scan.exec_models[0] != mesh_compute_detail::kExecGLCompute)
        return fail("mesh_compute: producer must be exactly one GLCompute entry point");
    for (const std::uint32_t model : scan.exec_models)
        if (mesh_compute_detail::is_mesh_exec(model)) return fail("mesh_compute: producer must not be a mesh/task entry");
    if (scan.capabilities.count(mesh_compute_detail::kCapMeshShadingEXT) ||
        scan.capabilities.count(mesh_compute_detail::kCapMeshShadingNV))
        return fail("mesh_compute: producer must not declare a mesh-shading capability");
    if (!scan.capabilities.count(mesh_compute_detail::kCapInt64) ||
        !scan.capabilities.count(mesh_compute_detail::kCapPhysicalStorageBuffer))
        return fail("mesh_compute: producer must declare Int64 + PhysicalStorageBufferAddresses");
    if (!scan.member_offsets.count(144) || !scan.member_offsets.count(148))
        return fail("mesh_compute: producer must address its output at push bytes 144/148 (dwords 36..37, 152 bytes)");
    return true;
}

inline bool mesh_producer_is_compute(std::span<const std::uint32_t> words, std::string* error = nullptr) {
    return mesh_producer_scan_is_compute(mesh_compute_detail::scan_spirv(words), error);
}

// True when `words` is a producer the 152-byte mesh-compute contract accepts (mesh_producer_is_compute) that
// declares the indirect-tile contract: exactly one global OpModuleProcessed whose string is
// kMeshComputeIndirectTileMarker, parsed from the words themselves (never from a name or path), in a module of
// SPIR-V 1.1 or later (the first version that has the instruction). Callers pass the SHA-sealed words. False with
// *why for an older (direct-only) producer, a malformed or duplicated marker, or anything mesh_producer_is_compute
// refuses.
inline bool mesh_producer_indirect_tiles_v1(std::span<const std::uint32_t> words, std::string* why = nullptr) {
    const auto fail = [&](const std::string& reason) {
        if (why) *why = reason;
        return false;
    };
    const auto scan = mesh_compute_detail::scan_spirv(words);
    if (!mesh_producer_scan_is_compute(scan, why)) return false;
    if (words[1] < 0x00010100u) return fail("mesh_compute: SPIR-V older than 1.1 has no OpModuleProcessed marker");
    if (scan.indirect_tile_markers == 0)
        return fail("mesh_compute: producer does not declare the indirect-tile contract (direct-only)");
    if (scan.indirect_tile_markers > 1) return fail("mesh_compute: producer repeats the indirect-tile marker");
    return true;
}

// Reads the GLCompute workgroup size from OpExecutionMode LocalSize literals. False for malformed words, a
// LocalSizeId form (IDs cannot be evaluated statically, explicitly refused), missing/multiple sizes, or sizes
// outside 1..1024. The caller must require agreement with index metadata so a default {1,1,1} can never pass the
// wrong device limit.
inline bool mesh_producer_local_size(std::span<const std::uint32_t> words, std::array<std::uint32_t, 3>& out,
                                     std::string* error = nullptr) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const auto scan = mesh_compute_detail::scan_spirv(words);
    if (!scan.valid) return fail("mesh_compute: malformed producer SPIR-V");
    if (scan.has_local_size_id)
        return fail("mesh_compute: producer uses LocalSizeId form, which is explicitly unsupported (need literals)");
    if (scan.entries.size() != 1 || scan.entries[0].second != mesh_compute_detail::kExecGLCompute)
        return fail("mesh_compute: producer must be exactly one GLCompute entry point");
    const std::uint32_t entry = scan.entries[0].first;
    const mesh_compute_detail::Scan::LocalSize* found = nullptr;
    for (const auto& ls : scan.local_sizes) {
        if (ls.entry != entry) continue;
        if (found) return fail("mesh_compute: producer declares multiple LocalSize modes");
        found = &ls;
    }
    if (!found) return fail("mesh_compute: producer lacks an OpExecutionMode LocalSize");
    if (found->x == 0 || found->y == 0 || found->z == 0 || found->x > 1024 || found->y > 1024 ||
        found->z > 1024) {
        return fail("mesh_compute: producer LocalSize out of range");
    }
    out = {found->x, found->y, found->z};
    return true;
}

// The companion must be exactly one Vertex entry, with 64-bit buffer addressing and no mesh/task capability.
inline bool mesh_consumer_is_vertex(std::span<const std::uint32_t> words, std::string* error = nullptr) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    const auto scan = mesh_compute_detail::scan_spirv(words);
    if (!scan.valid) return fail("mesh_compute: malformed companion SPIR-V");
    if (scan.exec_models.size() != 1 || scan.exec_models[0] != mesh_compute_detail::kExecVertex)
        return fail("mesh_compute: companion must be exactly one Vertex entry point");
    for (const std::uint32_t model : scan.exec_models)
        if (mesh_compute_detail::is_mesh_exec(model))
            return fail("mesh_compute: companion must not be a mesh/task entry");
    if (scan.capabilities.count(mesh_compute_detail::kCapMeshShadingEXT) ||
        scan.capabilities.count(mesh_compute_detail::kCapMeshShadingNV))
        return fail("mesh_compute: companion must not declare a mesh-shading capability");
    if (!scan.capabilities.count(mesh_compute_detail::kCapInt64) ||
        !scan.capabilities.count(mesh_compute_detail::kCapPhysicalStorageBuffer))
        return fail("mesh_compute: companion must declare Int64 + PhysicalStorageBufferAddresses");
    return true;
}

// The on-disk companion must export exactly the layout's params plus its static halves (no missing/extra
// locations), carry ClipDistance/CullDistance capabilities exactly when the layout needs them, and read the
// output address from push bytes 144/148. A 128-byte pc-tail layout has no supported companion here.
inline bool mesh_companion_matches_layout(const MeshComputeLayout& layout, std::span<const std::uint32_t> words,
                                          std::string* error = nullptr) {
    const auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!layout.is_supported_push()) {
        if (layout.is_pc_tail())
            return fail("mesh_compute: 128-byte pc-tail UBO layout (address at tail dwords 4..5) has no verified "
                        "companion in this wave; refusing (need the 152-byte push variant)");
        return fail("mesh_compute: unsupported push contract for the companion");
    }
    const auto scan = mesh_compute_detail::scan_spirv(words);
    if (!scan.valid) return fail("mesh_compute: malformed companion SPIR-V");
    std::set<std::uint32_t> want;
    for (const auto& [loc, off] : layout.params) {
        (void)off;
        want.insert(loc);
    }
    for (const auto& [param, pair] : layout.f16_halves) {
        (void)param;
        want.insert(pair[0]);
        want.insert(pair[1]);
    }
    std::set<std::uint32_t> got;
    for (const auto& [loc, id] : scan.output_locations) {
        (void)id;
        got.insert(loc);
    }
    if (got != want) {
        std::string missing, extra;
        for (const std::uint32_t loc : want)
            if (!got.count(loc)) missing += (missing.empty() ? "" : ",") + std::to_string(loc);
        for (const std::uint32_t loc : got)
            if (!want.count(loc)) extra += (extra.empty() ? "" : ",") + std::to_string(loc);
        std::string why = "mesh_compute: companion locations do not match the layout";
        if (!missing.empty()) why += "; missing " + missing;
        if (!extra.empty()) why += "; extra " + extra;
        return fail(why);
    }
    const bool want_clip = layout.clip_count > 0;
    const bool want_cull = layout.cull_count > 0;
    const bool has_clip = scan.capabilities.count(mesh_compute_detail::kCapClipDistance) > 0;
    const bool has_cull = scan.capabilities.count(mesh_compute_detail::kCapCullDistance) > 0;
    if (want_clip != has_clip || want_cull != has_cull)
        return fail("mesh_compute: companion ClipDistance/CullDistance capabilities do not match the layout");
    if (!scan.member_offsets.count(144) || !scan.member_offsets.count(148))
        return fail("mesh_compute: companion must address its output at push bytes 144/148 (dwords 36..37)");
    return true;
}

}  // namespace leonida
