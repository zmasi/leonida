#pragma once

// Direct mesh-as-compute PSO contract (CPU-visible side).
//
// A logically-mesh D3D12 PSO whose index entry carries a validated `mesh_compute`
// layout runs as a compute producer plus a vertex+pixel consumer. The producer is
// a translated GLCompute module; the consumer's vertex stage is the offline
// `<base>.mesh_vs.spv` companion (Vertex) next to the producer. Both phases use a
// 152-byte push block: the 144-byte ShaderUserData plus the output buffer device
// address in dwords 36-37. Logical `GraphicsPipeline::mesh` stays true; only the
// Vulkan stages differ (no mesh extension for the emulated path).
//
// This header is the CPU inspect surface for tests. It needs no Vulkan headers.

#include <cstdint>
#include <string>
#include <vector>

struct ID3D12PipelineState;

namespace rt::d3d12 {

// Push bytes for both mesh-compute phases (144-byte user data + 8-byte output BDA).
inline constexpr std::uint32_t kMeshComputePushBytes = 152;
// Output address dwords inside that block (s0..s31, heap, code, then output).
inline constexpr std::uint32_t kMeshComputeOutputDword0 = 36;
inline constexpr std::uint32_t kMeshComputeOutputDword1 = 37;

// True for an emulated mesh-compute PSO; false for ordinary, native mesh, and non-PSOs.
bool pipeline_is_mesh_compute(ID3D12PipelineState* pso);
// Slot contract + producer code base for an emulated PSO. Any out-param may be null.
// slot_bytes/max_primitives come from the validated index layout; producer_code_base
// is the console VA (arena offset) of the producer FSSM blob, 0 when unused.
// Returns false when `pso` is not an emulated mesh-compute PSO.
bool pipeline_mesh_compute_info(ID3D12PipelineState* pso, std::uint64_t* slot_bytes,
                                std::uint32_t* max_primitives, std::uint64_t* producer_code_base);
// The producer's SPIR-V words (GLCompute), reloaded from the library. False with `why`
// when not emulated or when the module went missing since creation.
bool pipeline_mesh_compute_producer_words(ID3D12PipelineState* pso, std::vector<std::uint32_t>& words,
                                          std::string& why);
// True once the producer's Vulkan pipeline exists: at creation for an eager PSO (LEONIDA_LAZY_PIPELINES=0), after the
// first DispatchMesh that executes it for a lazy one. False for a non-emulated PSO.
bool pipeline_mesh_compute_producer_built(ID3D12PipelineState* pso);
// Producer driver builds this process has started (one per PSO at most, a failed one included): the unused PSOs of a
// lazy device add none.
std::uint64_t mesh_compute_producer_builds();

}  // namespace rt::d3d12
