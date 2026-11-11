#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <unknwn.h>
#include <d3d12.h>       // Resolves to sdk/agility/extracted/build/native/include/d3d12.h
#include <dxgi1_6.h>
#include <wrl/client.h>

#ifndef __in
#define __in
#define __out
#define __inout
#define __in_opt
#define __out_opt
#define __inout_opt
#define __in_ecount(x)
#define __out_ecount(x)
#define __out_ecount_opt(x)
#define __inout_ecount(x)
#define __inout_ecount_part_opt(x,y)
#define __in_bcount(x)
#define __out_bcount(x)
#endif

#include "nvapi.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace amd_mg {

using Microsoft::WRL::ComPtr;

// ============================================================================
// Logging
// ============================================================================
void LogInit();
void Log(const char* fmt, ...);

// ============================================================================
// GPU Indirect Struct Layouts (Host-side reference for validation & testing)
// ============================================================================
#pragma pack(push, 1)

struct NvTrianglesToClasArgs {
    uint32_t clusterId;
    uint32_t clusterFlags;
    uint32_t packedCounts; // tri:9, vtx:9, trunc:6, idxFmt:4, ommFmt:4
    uint32_t baseGeometryIndexAndFlags;
    uint16_t indexBufferStride;
    uint16_t vertexBufferStride;
    uint16_t geometryIndexAndFlagsBufferStride;
    uint16_t opacityMicromapIndexBufferStride;
    uint64_t indexBuffer;
    uint64_t vertexBuffer;
    uint64_t geometryIndexAndFlagsBuffer;
    uint64_t opacityMicromapArray;
    uint64_t opacityMicromapIndexBuffer;
};
static_assert(sizeof(NvTrianglesToClasArgs) == 64, "NvTrianglesToClasArgs must be 64 bytes");

struct NvTrianglesToTemplateArgs : NvTrianglesToClasArgs {
    uint64_t instantiationBoundingBoxLimit;
};
static_assert(sizeof(NvTrianglesToTemplateArgs) == 72, "NvTrianglesToTemplateArgs must be 72 bytes");

struct NvInstantiateTemplateArgs {
    uint32_t clusterIdOffset;
    uint32_t geometryIndexOffset;
    uint64_t clusterTemplate;
    uint64_t vertexBufferStartAddress;
    uint64_t vertexBufferStrideInBytes;
};
static_assert(sizeof(NvInstantiateTemplateArgs) == 32, "NvInstantiateTemplateArgs must be 32 bytes");

struct NvClasToBlasArgs {
    uint32_t clasCount;
    uint32_t reserved;
    uint64_t clasAddresses;
};
static_assert(sizeof(NvClasToBlasArgs) == 16, "NvClasToBlasArgs must be 16 bytes");

struct NvPtlasWriteInstanceArgs {
    float    transform[3][4];
    float    explicitAABB[6];
    uint32_t instanceID;
    uint32_t instanceMask;
    uint32_t instanceContributionToHitGroupIndex;
    uint32_t instanceFlags;
    uint32_t instanceIndex;
    uint32_t partitionIndex;
    uint64_t accelerationStructure;
};
static_assert(sizeof(NvPtlasWriteInstanceArgs) == 104, "NvPtlasWriteInstanceArgs must be 104 bytes");

#pragma pack(pop)

static_assert(sizeof(D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS) == 80, "DXR CLAS_FROM_TRIANGLES_ARGS must be 80 bytes");
static_assert(sizeof(D3D12_RTAS_OPERATION_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES_ARGS) == 96, "DXR BUILD_CLUSTER_TEMPLATES_ARGS must be 96 bytes");
static_assert(sizeof(D3D12_RTAS_OPERATION_INSTANTIATE_CLUSTER_TEMPLATES_ARGS) == 32, "DXR INSTANTIATE_CLUSTER_TEMPLATES_ARGS must be 32 bytes");
static_assert(sizeof(D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS) == 16, "DXR BUILD_BLAS_FROM_CLAS_ARGS must be 16 bytes");
static_assert(sizeof(D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS) == 104, "DXR PTLAS_WRITE_INSTANCE_ARGS must be 104 bytes");

// ============================================================================
// CPU-side Reference Converter (also used in tests & fallback)
// ============================================================================
void ConvertClasFromTrianglesCPU(const NvTrianglesToClasArgs& src, D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS& dst);
void ConvertTemplatesFromTrianglesCPU(const NvTrianglesToTemplateArgs& src, D3D12_RTAS_OPERATION_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES_ARGS& dst);
void ConvertInstantiateTemplatesCPU(const NvInstantiateTemplateArgs& src, D3D12_RTAS_OPERATION_INSTANTIATE_CLUSTER_TEMPLATES_ARGS& dst);
void ConvertClasToBlasCPU(const NvClasToBlasArgs& src, D3D12_RTAS_OPERATION_BUILD_BLAS_FROM_CLAS_ARGS& dst);
void ConvertPtlasWriteInstanceCPU(const NvPtlasWriteInstanceArgs& src, D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS& dst);

// ============================================================================
// Host Struct Translation: NvAPI -> D3D12 1.722
// ============================================================================
struct TranslatedClusterInputs {
    D3D12_RTAS_OPERATION_INPUTS                       inputs{};
    D3D12_RTAS_CLUSTER_TRIANGLES_INPUTS_DESC          trianglesDesc{};
    D3D12_RTAS_CLUSTER_TEMPLATE_TRIANGLES_INPUTS_DESC templatesDesc{};
    D3D12_RTAS_INSTANTIATE_CLUSTER_TEMPLATE_INPUTS_DESC instantiateDesc{};
    D3D12_RTAS_CLAS_INPUTS_DESC                       clasDesc{};
    D3D12_RTAS_CLUSTER_MOVES_DESC                     movesDesc{};
};

bool TranslateClusterInputs(
    const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS& nvIn,
    TranslatedClusterInputs& out);

struct TranslatedPtlasInputs {
    D3D12_RTAS_OPERATION_INPUTS              inputs{};
    D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC  ptlasDesc{};
};

bool TranslatePtlasPrebuildInputs(
    const NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS& nvParams,
    TranslatedPtlasInputs& out);

bool TranslatePtlasBuildInputs(
    const NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS& nvParams,
    TranslatedPtlasInputs& out);

// ============================================================================
// D3D12 Hook & Preview Enabler (src/d3d12_hook.cpp)
// ============================================================================
using PFN_nvapi_QueryInterface = void* (__cdecl*)(uint32_t id);
void SetChainedQueryInterface(PFN_nvapi_QueryInterface fn);
PFN_nvapi_QueryInterface GetChainedQueryInterface();
void InstallProcessIatHooks(HMODULE selfModule);
void TryEnableD3D12PreviewFeatures();

// AMD_MG_DISABLE=1: report Cluster Operations / Partitioned TLAS (NvAPI caps, cluster shader
// opcodes and DXR OPTIONS_RAYTRACING2.ClustersAndPTLASSupported) as unsupported, so the game
// takes its non-Mega-Geometry path. Everything else (Agility SDK 1.722, NvAPI chaining to
// fakenvapi, Streamline/AMD-NR) stays active. Read on every query (cheap getenv) so an A/B
// needs only a restart with the variable changed.
bool IsMegaGeometryDisabled();
void HookDeviceForStateObjects(ID3D12Device* pDevice);
void SetDeviceShaderExtnSlot(ID3D12Device* pDevice, uint32_t uavSlot, uint32_t uavSpace, bool isThreadLocal);
bool GetActiveShaderExtnSlot(ID3D12Device* pDevice, uint32_t& outSlot, uint32_t& outSpace);

// ============================================================================
// DXIL Shader Patcher (src/dxil_patcher.cpp)
// ============================================================================
bool InitializeDxilPatcher();
void ShutdownDxilPatcher();

bool PatchDxilClusterShader(
    const void* pBytecode,
    size_t bytecodeLength,
    uint32_t uavSlot,
    uint32_t uavSpace,
    std::vector<uint8_t>& outPatchedBytecode);

bool TransformDxilIrText(
    const std::string& irText,
    uint32_t uavSlot,
    uint32_t uavSpace,
    std::string& outIrText,
    uint32_t& outPatchCount);

// ============================================================================
// GPU Indirect Argument Converter (src/gpu_arg_converter.cpp)
// ============================================================================
struct ConvertedGpuDispatch {
    D3D12_GPU_VIRTUAL_ADDRESS indirectArgArrayAddr = 0;
    uint32_t                  indirectArgStride    = 0;
    D3D12_GPU_VIRTUAL_ADDRESS resultSizeArrayAddr  = 0;
    uint32_t                  resultSizeStride     = 0;
    bool                      needsResultDowncast  = false;
    D3D12_GPU_VIRTUAL_ADDRESS origResultSizeAddr   = 0;
    uint32_t                  origResultSizeStride = 0;
    uint32_t                  maxOperations        = 0;
    D3D12_GPU_VIRTUAL_ADDRESS argCountAddr         = 0;  // NvAPI indirectArgCount (GPU count), 0 = maxArgCount
    int                       argSlot              = -1; // scratch slot holding converted args (-1 = none)
    int                       sizeSlot             = -1; // scratch slot holding 64-bit DXR result sizes (-1 = none)
    D3D12_GPU_VIRTUAL_ADDRESS resultAddressArrayAddr = 0;  // destination array passed to DXR
    uint32_t                  resultAddressStride    = 0;
    int                       dstSlot              = -1; // scratch slot holding the sanitized destination array
    // AMD_MG_SPLIT_OPS: execute as numChunks operations of chunkSize arguments each.
    uint32_t                  chunkSize            = 0;  // 0 = single operation
    uint32_t                  numChunks            = 0;
    D3D12_GPU_VIRTUAL_ADDRESS chunkCountsAddr      = 0;  // per-chunk GPU counts (GPU-count operations only)
    int                       chunkSlot            = -1;
    uint32_t                  convOp               = 0;  // converter operation sequence (16 bits)
};

// Layout analysis: BLAS-from-CLAS result size reported to the game for a single BLAS of
// clasCount CLAS (GetClusterOperationRequirementsInfo with maxArgCount = 1).
void NoteReportedBlasSize(uint32_t clasCount, uint64_t resultBytes);

// A/B diagnostics for the BLAS_FROM_CLAS device removal (AW2), from env / amd_megageometry.ini:
// AMD_MG_BLAS_PRIVATE_SCRATCH=1: BLAS operations use a wrapper-owned scratch buffer.
// AMD_MG_BLAS_FAKE_CLAS=1: every BLAS is built from one wrapper-built one-triangle CLAS.
bool BlasPrivateScratchEnabled();
bool BlasFakeClasEnabled();
// AMD_MG_FORCE_GEOM0=1: CLAS / template arguments get geometry index 0 (flags kept).
bool ForceGeom0Enabled();
// AMD_MG_TEMPLATE_NO_HINT=1: template arguments are passed without their (optional) vertex buffer.
bool TemplateNoHintEnabled();
// Persistent wrapper scratch of at least `bytes` (grow-only, never freed while in use); 0 on failure.
D3D12_GPU_VIRTUAL_ADDRESS AcquireBlasPrivateScratch(ID3D12Device* dev, uint64_t bytes);

// AMD_MG_SPLIT_OPS progress markers: written by the GPU before (completed=false) and after each chunk.
void RecordChunkProgress(ID3D12GraphicsCommandList* pCmdList, const ConvertedGpuDispatch& dispatch,
                         uint32_t chunk, bool completed);

// Records the NvAPI -> DXR argument conversion on pCmdList. Scratch slots taken here stay
// reserved until FinalizeGpuClusterResultSizes has recorded their GPU completion marker, so it
// MUST be called after ExecuteIndirectRTASOperations whenever this returns true.
bool PrepareGpuClusterArguments(
    ID3D12GraphicsCommandList* pCmdList,
    const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC& nvDesc,
    ConvertedGpuDispatch& outDispatch);

// Downcasts the 64-bit DXR result sizes into the game's 32-bit array (when requested), returns
// the scratch slots to their canonical state and records their GPU completion markers.
void FinalizeGpuClusterResultSizes(
    ID3D12GraphicsCommandList* pCmdList,
    const ConvertedGpuDispatch& dispatch);

// processTerminating: DLL_PROCESS_DETACH during ExitProcess, D3D12 objects are leaked.
void ShutdownGpuConverter(bool processTerminating = false);

// AMD_MG_DUMP_ARGS=1: print every pending argument dump now (called from the DRED dump).
void FlushArgDumpsForDiagnostics();

// DRED page fault: log the wrapper's own GPU buffers closest to va (a fault just past the end of
// one of them points at a DXR read/write overflowing the converter's arrays).
void DescribeConverterAllocationsNear(uint64_t va);

// ============================================================================
// Command-list compute-state shadowing (src/cmdlist_state.cpp)
// ============================================================================
// Hooks the compute-state setters of the runtime's command-list vtables (probe lists of type
// DIRECT and COMPUTE are created on the device) so the wrapper can put back the game's compute
// root signature, root arguments and pipeline after recording its own conversion dispatches.
void HookDeviceCommandListsForStateShadow(ID3D12Device* pDevice);

// DRED (src/dred.cpp): breadcrumbs + page-fault data written to the log on device removal.
// Default on; AMD_MG_DRED=0 disables. Must be enabled before the device is created.
void EnableDredBeforeDeviceCreation();
void WatchDeviceRemoval(ID3D12Device* pDevice);
bool HookCommandListForStateShadow(ID3D12GraphicsCommandList* pList);

struct CmdListComputeState;

// RAII: snapshot the game's compute state on construction, suppress shadow updates while the
// wrapper records its own commands, replay the snapshot on destruction.
class ComputeStateScope {
public:
    explicit ComputeStateScope(ID3D12GraphicsCommandList* pList);
    ~ComputeStateScope();
    ComputeStateScope(const ComputeStateScope&) = delete;
    ComputeStateScope& operator=(const ComputeStateScope&) = delete;

private:
    ID3D12GraphicsCommandList*           m_list = nullptr;
    const void*                          m_key  = nullptr;
    std::unique_ptr<CmdListComputeState> m_snapshot;
};

// Test hook: scratch buffer behind a slot index from ConvertedGpuDispatch (nullptr if unknown).
ID3D12Resource* GetConverterScratchForTest(ID3D12Device* pDevice, int slot);

} // namespace amd_mg

extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(uint32_t id);
