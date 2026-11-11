#include "amd_mg.h"

#include <cstdlib>

#include <atomic>
#include "nvShaderExtnEnums.h"

namespace amd_mg {

// ============================================================================
// Host Struct Translation Helpers
// ============================================================================
namespace {

D3D12_RTAS_OPERATION_FLAGS TranslateClusterOpFlags(
    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAGS nvFlags)
{
    D3D12_RTAS_OPERATION_FLAGS dxrFlags = D3D12_RTAS_OPERATION_FLAG_NONE;
    if (nvFlags & NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_FAST_TRACE)
        dxrFlags |= D3D12_RTAS_OPERATION_FLAG_FAST_TRACE;
    if (nvFlags & NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_FAST_BUILD)
        dxrFlags |= D3D12_RTAS_OPERATION_FLAG_FAST_OPERATION;
    if (nvFlags & NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_ALLOW_OMM)
        dxrFlags |= D3D12_RTAS_OPERATION_FLAG_ALLOW_OMM;
    return dxrFlags;
}

D3D12_VERTEX_FORMAT TranslateVertexFormat(uint32_t dxgiFmt) {
    switch (dxgiFmt) {
    case DXGI_FORMAT_R32G32_FLOAT:       return D3D12_VERTEX_FORMAT_FLOAT32_2;
    case DXGI_FORMAT_R32G32B32_FLOAT:    return D3D12_VERTEX_FORMAT_FLOAT32_3;
    case DXGI_FORMAT_R16G16_FLOAT:       return D3D12_VERTEX_FORMAT_FLOAT16_2;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return D3D12_VERTEX_FORMAT_FLOAT16_3;
    case DXGI_FORMAT_R16G16_UNORM:       return D3D12_VERTEX_FORMAT_UNORM16_2;
    case DXGI_FORMAT_R16G16B16A16_UNORM: return D3D12_VERTEX_FORMAT_UNORM16_3;
    case DXGI_FORMAT_R16G16_SNORM:       return D3D12_VERTEX_FORMAT_SNORM16_2;
    case DXGI_FORMAT_R16G16B16A16_SNORM: return D3D12_VERTEX_FORMAT_SNORM16_3;
    case DXGI_FORMAT_R8G8_UNORM:         return D3D12_VERTEX_FORMAT_UNORM8_2;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     return D3D12_VERTEX_FORMAT_UNORM8_3;
    case DXGI_FORMAT_R8G8_SNORM:         return D3D12_VERTEX_FORMAT_SNORM8_2;
    case DXGI_FORMAT_R8G8B8A8_SNORM:     return D3D12_VERTEX_FORMAT_SNORM8_3;
    case DXGI_FORMAT_R10G10B10A2_UNORM:  return D3D12_VERTEX_FORMAT_10_10_10_2_UNORM;
    default:                             return D3D12_VERTEX_FORMAT_FLOAT32_3;
    }
}

// Index format declared to DXR for triangle/template builds. NvAPI declares it per argument,
// DXR per operation. UINT8 (default) reads the low byte of each index with the real element
// stride: correct only while every index value is < 256. AMD_MG_INDEX_FORMAT=16|32 declares a
// wider format instead (the argument converter keeps the real per-argument stride).
D3D12_INDEX_FORMAT DeclaredIndexFormat() {
    static const D3D12_INDEX_FORMAT fmt = [] {
        char value[8] = {};
        const DWORD len = GetEnvironmentVariableA("AMD_MG_INDEX_FORMAT", value, sizeof(value));
        D3D12_INDEX_FORMAT f = D3D12_INDEX_FORMAT_UINT8;
        if (len > 0 && len < sizeof(value)) {
            const int bits = std::atoi(value);
            if (bits == 16) f = D3D12_INDEX_FORMAT_UINT16;
            else if (bits == 32) f = D3D12_INDEX_FORMAT_UINT32;
        }
        Log("[NvAPI] Cluster index format declared to DXR: %s (AMD_MG_INDEX_FORMAT)",
            f == D3D12_INDEX_FORMAT_UINT16 ? "UINT16" : (f == D3D12_INDEX_FORMAT_UINT32 ? "UINT32" : "UINT8"));
        return f;
    }();
    return fmt;
}

void PopulateClusterLimits(
    const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS& nvIn,
    D3D12_RTAS_CLUSTER_LIMITS& limits)
{
    limits.MaxArgCount                                   = nvIn.maxArgCount;
    limits.MaxGeometryIndexValue                         = nvIn.trianglesDesc.maxGeometryIndexValue;
    limits.MaxUniqueGeometryIndexAndFlagsCountPerCluster = std::max<UINT>(1u, nvIn.trianglesDesc.maxUniqueGeometryCountPerArg);
    limits.MaxTriangleCountPerCluster                    = nvIn.trianglesDesc.maxTriangleCountPerArg;
    limits.MaxUniqueVertexIndexCountPerCluster           = nvIn.trianglesDesc.maxVertexCountPerArg;
    limits.MaxTotalTriangleCount                         = nvIn.trianglesDesc.maxTotalTriangleCount;
    limits.MaxSumOfUniqueVertexIndexCounts               = nvIn.trianglesDesc.maxTotalVertexCount;
    limits.MaxOpacityMicromapIndicesPerCluster           = 0;
}

} // namespace

bool TranslateClusterInputs(
    const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS& nvIn,
    TranslatedClusterInputs& out)
{
    out = {};
    auto dxrFlags = TranslateClusterOpFlags(nvIn.flags);
    auto dxrMode  = static_cast<D3D12_RTAS_OPERATION_MODE>(nvIn.mode);

    switch (nvIn.type) {
    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES:
        out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
        PopulateClusterLimits(nvIn, out.trianglesDesc.ClusterLimits);
        out.trianglesDesc.Flags                             = dxrFlags | D3D12_RTAS_OPERATION_FLAG_ALLOW_DATA_ACCESS;
        out.trianglesDesc.Mode                              = dxrMode;
        out.trianglesDesc.VertexFormat                      = TranslateVertexFormat(nvIn.trianglesDesc.vertexFormat);
        out.trianglesDesc.IndexFormat                       = DeclaredIndexFormat();
        out.trianglesDesc.GeometryIndexAndFlagsIndexFormat  = D3D12_INDEX_FORMAT_NONE;
        out.trianglesDesc.OpacityMicromapIndexFormat        = D3D12_INDEX_FORMAT_NONE;
        out.trianglesDesc.MinPositionTruncateBitCount       = nvIn.trianglesDesc.minPositionTruncateBitCount;
        out.inputs.pClusterTrianglesDesc                    = &out.trianglesDesc;
        return true;

    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES:
        out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES;
        PopulateClusterLimits(nvIn, out.templatesDesc.ClusterLimits);
        out.templatesDesc.Flags                             = dxrFlags | D3D12_RTAS_OPERATION_FLAG_ALLOW_DATA_ACCESS;
        out.templatesDesc.Mode                              = dxrMode;
        out.templatesDesc.VertexHintFormat                  = TranslateVertexFormat(nvIn.trianglesDesc.vertexFormat);
        out.templatesDesc.VertexInstantiationFormat         = TranslateVertexFormat(nvIn.trianglesDesc.vertexFormat);
        out.templatesDesc.IndexFormat                       = DeclaredIndexFormat();
        out.templatesDesc.GeometryIndexAndFlagsIndexFormat  = D3D12_INDEX_FORMAT_NONE;
        out.templatesDesc.OpacityMicromapIndexFormat        = D3D12_INDEX_FORMAT_NONE;
        out.inputs.pClusterTemplateTrianglesDesc            = &out.templatesDesc;
        return true;

    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_INSTANTIATE_CLUSTER_TEMPLATES:
        out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_INSTANTIATE_CLUSTER_TEMPLATES;
        PopulateClusterLimits(nvIn, out.instantiateDesc.ClusterLimits);
        out.instantiateDesc.Flags                           = dxrFlags | D3D12_RTAS_OPERATION_FLAG_ALLOW_DATA_ACCESS;
        out.instantiateDesc.Mode                            = dxrMode;
        out.instantiateDesc.VertexSourceFormat              = TranslateVertexFormat(nvIn.trianglesDesc.vertexFormat);
        out.inputs.pInstantiateClusterTemplateDesc          = &out.instantiateDesc;
        return true;

    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS:
        out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        out.clasDesc.Flags             = dxrFlags;
        out.clasDesc.MaxArgCount       = nvIn.maxArgCount;
        out.clasDesc.Mode              = dxrMode;
        out.clasDesc.MaxTotalClasCount = nvIn.clasDesc.maxTotalClasCount;
        out.clasDesc.MaxClasCountPerArg = nvIn.clasDesc.maxClasCountPerArg;
        out.inputs.pClasDesc           = &out.clasDesc;
        return true;

    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_MOVE_CLUSTER_OBJECT:
        out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_MOVE_CLUSTER_OBJECTS;
        out.movesDesc.Flags         = (nvIn.flags & NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_NO_OVERLAP)
            ? D3D12_RTAS_CLUSTER_MOVE_OPERATION_FLAG_NO_OVERLAP
            : D3D12_RTAS_CLUSTER_MOVE_OPERATION_FLAG_NONE;
        out.movesDesc.MaxArgCount   = nvIn.maxArgCount;
        out.movesDesc.Mode          = dxrMode;
        out.movesDesc.Type          = static_cast<D3D12_RTAS_MOVE_OPERATION_TYPE>(nvIn.movesDesc.type);
        out.movesDesc.MaxBytesMoved = nvIn.movesDesc.maxBytesMoved;
        out.inputs.pClusterMovesDesc = &out.movesDesc;
        return true;

    default:
        return false;
    }
}

namespace {

void PopulatePtlasDesc(
    const NVAPI_D3D12_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_INPUTS& src,
    D3D12_RTAS_PARTITIONED_TLAS_INPUTS_DESC& outDesc)
{
    outDesc.Flags                             = static_cast<D3D12_RTAS_PARTITIONED_TLAS_FLAGS>(src.flags);
    outDesc.InstanceCount                     = src.instanceCount;
    outDesc.MaxInstancePerPartitionCount      = src.maxInstancePerPartitionCount;
    outDesc.PartitionCount                    = src.partitionCount;
    outDesc.MaxInstanceInGlobalPartitionCount = src.maxInstanceInGlobalPartitionCount;
}

} // namespace

bool TranslatePtlasPrebuildInputs(
    const NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS& nvParams,
    TranslatedPtlasInputs& out)
{
    if (!nvParams.pInput) {
        return false;
    }
    out = {};
    out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS;
    PopulatePtlasDesc(*nvParams.pInput, out.ptlasDesc);
    out.inputs.pPartitionedTLASInputsDesc = &out.ptlasDesc;
    return true;
}

bool TranslatePtlasBuildInputs(
    const NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS& nvParams,
    TranslatedPtlasInputs& out)
{
    if (!nvParams.pDesc) {
        return false;
    }
    out = {};
    out.inputs.Type = D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS;
    PopulatePtlasDesc(nvParams.pDesc->inputs, out.ptlasDesc);
    out.inputs.pPartitionedTLASInputsDesc = &out.ptlasDesc;
    return true;
}

// ============================================================================
// NvAPI Implemented Entrypoints
// ============================================================================
namespace {

NvAPI_Status __cdecl Impl_NvAPI_Initialize() {
    LogInit();
    Log("[NvAPI] NvAPI_Initialize()");
    TryEnableD3D12PreviewFeatures();
    InitializeDxilPatcher();
    if (auto chain = GetChainedQueryInterface()) {
        using PFN_Init = NvAPI_Status(__cdecl*)();
        if (auto origInit = reinterpret_cast<PFN_Init>(chain(0x0150e828))) {
            origInit();
        }
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_Unload() {
    Log("[NvAPI] NvAPI_Unload()");
    ShutdownGpuConverter();
    ShutdownDxilPatcher();
    if (auto chain = GetChainedQueryInterface()) {
        using PFN_Unload = NvAPI_Status(__cdecl*)();
        if (auto origUnload = reinterpret_cast<PFN_Unload>(chain(0xd22bdd7e))) {
            origUnload();
        }
    }
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_GetErrorMessage(NvAPI_Status nr, NvAPI_ShortString szDesc) {
    if (!szDesc) {
        return NVAPI_INVALID_ARGUMENT;
    }
    std::snprintf(szDesc, sizeof(NvAPI_ShortString), "amd-megageometry status %d", static_cast<int>(nr));
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_GetInterfaceVersionString(NvAPI_ShortString szDesc) {
    if (!szDesc) {
        return NVAPI_INVALID_ARGUMENT;
    }
    std::snprintf(szDesc, sizeof(NvAPI_ShortString), "NVidia Complete Version 570.00 (amd-megageometry)");
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_GetRaytracingCaps(
    ID3D12Device* pDevice,
    NVAPI_D3D12_RAYTRACING_CAPS_TYPE type,
    void* pData,
    size_t dataSize)
{
    if (!pDevice || !pData) {
        return NVAPI_INVALID_ARGUMENT;
    }

    bool forceCaps = false;
    if (const char* envForce = std::getenv("AMD_MG_FORCE_CAPS")) {
        forceCaps = (std::atoi(envForce) != 0);
    }

    ComPtr<ID3D12DeviceRaytracing2> pRt2;
    bool hasRt2 = SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pRt2))) && (pRt2 != nullptr);

    HookDeviceForStateObjects(pDevice);

    switch (type) {
    case NVAPI_D3D12_RAYTRACING_CAPS_TYPE_THREAD_REORDERING:
        if (dataSize != sizeof(NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS))
            return NVAPI_INVALID_ARGUMENT;
        *static_cast<NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAPS*>(pData) =
            (hasRt2 || forceCaps)
                ? NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_STANDARD
                : NVAPI_D3D12_RAYTRACING_THREAD_REORDERING_CAP_NONE;
        return NVAPI_OK;

    case NVAPI_D3D12_RAYTRACING_CAPS_TYPE_OPACITY_MICROMAP:
        if (dataSize != sizeof(NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS))
            return NVAPI_INVALID_ARGUMENT;
        *static_cast<NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAPS*>(pData) =
            (hasRt2 || forceCaps)
                ? NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_STANDARD
                : NVAPI_D3D12_RAYTRACING_OPACITY_MICROMAP_CAP_NONE;
        return NVAPI_OK;

    case NVAPI_D3D12_RAYTRACING_CAPS_TYPE_DISPLACEMENT_MICROMAP:
        if (dataSize != sizeof(NVAPI_D3D12_RAYTRACING_DISPLACEMENT_MICROMAP_CAPS))
            return NVAPI_INVALID_ARGUMENT;
        *static_cast<NVAPI_D3D12_RAYTRACING_DISPLACEMENT_MICROMAP_CAPS*>(pData) =
            NVAPI_D3D12_RAYTRACING_DISPLACEMENT_MICROMAP_CAP_NONE;
        return NVAPI_OK;

    case NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS: {
        if (dataSize != sizeof(NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS))
            return NVAPI_INVALID_ARGUMENT;
        const bool disabled = IsMegaGeometryDisabled();
        const bool supported = !disabled && (hasRt2 || forceCaps);
        *static_cast<NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS*>(pData) =
            supported ? NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_STANDARD
                      : NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_NONE;
        Log("[NvAPI] GetRaytracingCaps(CLUSTER_OPERATIONS) -> %s (hasRt2=%d, force=%d, disabled=%d)",
            supported ? "STANDARD" : "NONE", hasRt2 ? 1 : 0, forceCaps ? 1 : 0, disabled ? 1 : 0);
        return NVAPI_OK;
    }

    case NVAPI_D3D12_RAYTRACING_CAPS_TYPE_PARTITIONED_TLAS: {
        if (dataSize != sizeof(NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAPS))
            return NVAPI_INVALID_ARGUMENT;
        const bool disabled = IsMegaGeometryDisabled();
        const bool supported = !disabled && (hasRt2 || forceCaps);
        *static_cast<NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAPS*>(pData) =
            supported ? NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_STANDARD
                      : NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_NONE;
        Log("[NvAPI] GetRaytracingCaps(PARTITIONED_TLAS) -> %s (hasRt2=%d, force=%d, disabled=%d)",
            supported ? "STANDARD" : "NONE", hasRt2 ? 1 : 0, forceCaps ? 1 : 0, disabled ? 1 : 0);
        return NVAPI_OK;
    }

    default:
        return NVAPI_NOT_SUPPORTED;
    }
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(
    ID3D12Device* pDevice,
    NvU32 opCode,
    bool* pSupported)
{
    if (!pDevice || !pSupported) {
        return NVAPI_INVALID_ARGUMENT;
    }
    HookDeviceForStateObjects(pDevice);
    if (opCode != NV_EXTN_OP_SHFL && IsMegaGeometryDisabled()) {
        *pSupported = false;
        Log("[NvAPI] IsNvShaderExtnOpCodeSupported(op=%u) -> false (AMD_MG_DISABLE)", opCode);
        return NVAPI_OK;
    }
    switch (opCode) {
    case NV_EXTN_OP_SHFL:                                     // 1 (used by NVRHI to detect HLSL extension UAV support)
    case NV_EXTN_OP_RT_TRIANGLE_OBJECT_POSITIONS:             // 86
    case NV_EXTN_OP_RT_GET_CLUSTER_ID:                        // 93
    case NV_EXTN_OP_RT_GET_CANDIDATE_CLUSTER_ID:              // 94
    case NV_EXTN_OP_RT_GET_COMMITTED_CLUSTER_ID:              // 95
    case NV_EXTN_OP_HIT_OBJECT_GET_CLUSTER_ID:                // 96
    case NV_EXTN_OP_RT_CANDIDATE_TRIANGLE_OBJECT_POSITIONS:   // 97
    case NV_EXTN_OP_RT_COMMITTED_TRIANGLE_OBJECT_POSITIONS:   // 98
    case NV_EXTN_OP_HIT_OBJECT_GET_TRIANGLE_OBJECT_POSITIONS: // 99
        *pSupported = true;
        Log("[NvAPI] IsNvShaderExtnOpCodeSupported(op=%u) -> true", opCode);
        return NVAPI_OK;
    default:
        *pSupported = false;
        return NVAPI_OK;
    }
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_QuerySinglePassStereoSupport(
    ID3D12Device* pDevice,
    NV_QUERY_SINGLE_PASS_STEREO_SUPPORT_PARAMS* pParams)
{
    if (!pDevice || !pParams) {
        return NVAPI_INVALID_ARGUMENT;
    }
    pParams->bSinglePassStereoSupported = 0;
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_SetCreatePipelineStateOptions(
    ID3D12Device* pDevice,
    const NVAPI_D3D12_SET_CREATE_PIPELINE_STATE_OPTIONS_PARAMS* pState)
{
    if (!pDevice || !pState) {
        return NVAPI_INVALID_ARGUMENT;
    }
    HookDeviceForStateObjects(pDevice);
    Log("[NvAPI] SetCreatePipelineStateOptions(dev=%p)", pDevice);
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_SetNvShaderExtnSlotSpace(
    IUnknown* pDev,
    NvU32 uavSlot,
    NvU32 uavSpace)
{
    if (!pDev) {
        return NVAPI_INVALID_ARGUMENT;
    }
    ComPtr<ID3D12Device> pDevice;
    pDev->QueryInterface(IID_PPV_ARGS(&pDevice));
    SetDeviceShaderExtnSlot(pDevice.Get(), uavSlot, uavSpace, false);
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread(
    IUnknown* pDev,
    NvU32 uavSlot,
    NvU32 uavSpace)
{
    if (!pDev) {
        return NVAPI_INVALID_ARGUMENT;
    }
    ComPtr<ID3D12Device> pDevice;
    pDev->QueryInterface(IID_PPV_ARGS(&pDevice));
    SetDeviceShaderExtnSlot(pDevice.Get(), uavSlot, uavSpace, true);
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo(
    ID3D12Device5* pDevice,
    const NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS* pParams)
{
    if (!pDevice || !pParams || !pParams->pInput || !pParams->pInfo) {
        return NVAPI_INVALID_POINTER;
    }

    const auto& nvIn = *pParams->pInput;
    const uint64_t count = std::max<uint64_t>(1ull, nvIn.maxArgCount);
    auto report = [&](uint64_t result, uint64_t scratch, const char* how) {
        pParams->pInfo->resultDataMaxSizeInBytes = result;
        pParams->pInfo->scratchDataSizeInBytes   = scratch;
        if (nvIn.type == NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS &&
            nvIn.maxArgCount == 1) {
            NoteReportedBlasSize(nvIn.clasDesc.maxClasCountPerArg, result);
        }
        Log("[NvAPI] GetClusterOperationRequirementsInfo(type=%d, mode=%d, maxOps=%u) -> result=%llu, scratch=%llu (%s)"
            " [flags=0x%x tri=%u vtx=%u totTri=%u totVtx=%u trunc=%u | clas=%u/%u]",
            nvIn.type, nvIn.mode, nvIn.maxArgCount, result, scratch, how, nvIn.flags,
            nvIn.trianglesDesc.maxTriangleCountPerArg, nvIn.trianglesDesc.maxVertexCountPerArg,
            nvIn.trianglesDesc.maxTotalTriangleCount, nvIn.trianglesDesc.maxTotalVertexCount,
            nvIn.trianglesDesc.minPositionTruncateBitCount,
            nvIn.type == NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS ? nvIn.clasDesc.maxTotalClasCount : 0u,
            nvIn.type == NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS ? nvIn.clasDesc.maxClasCountPerArg : 0u);
        return NVAPI_OK;
    };

    ComPtr<ID3D12DeviceRaytracing2> pRt2;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pRt2))) && pRt2) {
        TranslatedClusterInputs translated{};
        if (!TranslateClusterInputs(nvIn, translated)) {
            return NVAPI_INVALID_ARGUMENT;
        }
        const bool implicitMode = nvIn.mode == NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_IMPLICIT_DESTINATIONS;
        auto query = [&](TranslatedClusterInputs& t) {
            D3D12_RTAS_OPERATION_PREBUILD_INFO info{};
            pRt2->GetRTASOperationPrebuildInfo(&t.inputs, &info);
            return info;
        };
        // In GET_SIZES / EXPLICIT_DESTINATIONS a zero result size is legitimate (nothing is
        // written to batchResultData), so only an all-zero answer is treated as a rejection.
        auto acceptable = [&](const D3D12_RTAS_OPERATION_PREBUILD_INFO& i, bool needResult) {
            return needResult ? i.ResultDataMaxSizeInBytes > 0
                              : (i.ResultDataMaxSizeInBytes > 0 || i.ScratchDataSizeInBytes > 0);
        };

        D3D12_RTAS_OPERATION_PREBUILD_INFO info = query(translated);
        if (acceptable(info, implicitMode)) {
            return report(info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes, "via DXR2");
        }
        Log("[NvAPI] GetRTASOperationPrebuildInfo rejected type=%d mode=%d maxArgs=%u (geomMax=%u uniqueGeom=%u tri=%u vtx=%u totalTri=%u totalVtx=%u trunc=%u clasTotal=%u clasPerArg=%u moveBytes=%u) -> result=%llu scratch=%llu",
            nvIn.type, nvIn.mode, nvIn.maxArgCount,
            nvIn.trianglesDesc.maxGeometryIndexValue, nvIn.trianglesDesc.maxUniqueGeometryCountPerArg,
            nvIn.trianglesDesc.maxTriangleCountPerArg, nvIn.trianglesDesc.maxVertexCountPerArg,
            nvIn.trianglesDesc.maxTotalTriangleCount, nvIn.trianglesDesc.maxTotalVertexCount,
            nvIn.trianglesDesc.minPositionTruncateBitCount,
            nvIn.clasDesc.maxTotalClasCount, nvIn.clasDesc.maxClasCountPerArg, nvIn.movesDesc.maxBytesMoved,
            info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes);

        // Retry with progressively more conservative inputs. Over-reporting is safe (the game
        // allocates more), under-reporting makes the GPU write past the game's buffers.
        D3D12_RTAS_CLUSTER_LIMITS* limits = nullptr;
        D3D12_RTAS_OPERATION_MODE* mode   = nullptr;
        switch (translated.inputs.Type) {
        case D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES:
            limits = &translated.trianglesDesc.ClusterLimits;  mode = &translated.trianglesDesc.Mode;  break;
        case D3D12_RTAS_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES:
            limits = &translated.templatesDesc.ClusterLimits;  mode = &translated.templatesDesc.Mode;  break;
        case D3D12_RTAS_OPERATION_TYPE_INSTANTIATE_CLUSTER_TEMPLATES:
            limits = &translated.instantiateDesc.ClusterLimits; mode = &translated.instantiateDesc.Mode; break;
        case D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS:
            mode = &translated.clasDesc.Mode; break;
        case D3D12_RTAS_OPERATION_TYPE_MOVE_CLUSTER_OBJECTS:
            mode = &translated.movesDesc.Mode; break;
        default:
            break;
        }

        // 1) CLAS without ALLOW_DATA_ACCESS (position fetch) in case the driver rejects it.
        if (translated.inputs.Type == D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES) {
            translated.trianglesDesc.Flags &= ~D3D12_RTAS_OPERATION_FLAG_ALLOW_DATA_ACCESS;
            info = query(translated);
            if (acceptable(info, implicitMode)) {
                return report(info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes, "via DXR2, no data access");
            }
        }
        // 2) Totals raised to their per-argument upper bound (0 or inconsistent totals).
        if (limits) {
            limits->MaxTotalTriangleCount = std::max<UINT>(limits->MaxTotalTriangleCount,
                limits->MaxArgCount * std::max<UINT>(1u, limits->MaxTriangleCountPerCluster));
            limits->MaxSumOfUniqueVertexIndexCounts = std::max<UINT>(limits->MaxSumOfUniqueVertexIndexCounts,
                limits->MaxArgCount * std::max<UINT>(1u, limits->MaxUniqueVertexIndexCountPerCluster));
            info = query(translated);
            if (acceptable(info, implicitMode)) {
                return report(info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes, "via DXR2, raised totals");
            }
        }
        // 3) IMPLICIT_DESTINATIONS is an upper bound for every mode.
        if (mode && *mode != D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS) {
            *mode = D3D12_RTAS_OPERATION_MODE_IMPLICIT_DESTINATIONS;
            info = query(translated);
            if (acceptable(info, true)) {
                return report(info.ResultDataMaxSizeInBytes, info.ScratchDataSizeInBytes, "via DXR2, implicit upper bound");
            }
        }
        // 4) Single-argument query scaled by maxArgCount.
        if (limits && limits->MaxArgCount > 1) {
            limits->MaxArgCount                     = 1;
            limits->MaxTotalTriangleCount           = std::max<UINT>(1u, limits->MaxTriangleCountPerCluster);
            limits->MaxSumOfUniqueVertexIndexCounts = std::max<UINT>(1u, limits->MaxUniqueVertexIndexCountPerCluster);
            info = query(translated);
            if (acceptable(info, true)) {
                return report(info.ResultDataMaxSizeInBytes * count, info.ScratchDataSizeInBytes * count,
                              "via DXR2, single-arg x maxArgCount");
            }
        }
    }

    // Heuristic upper bounds (driver gave nothing usable). A 128-triangle CLAS is ~12.5 KB on
    // RDNA 4 (prebuild for maxArgCount=1), so size generously per argument.
    switch (nvIn.type) {
    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_MOVE_CLUSTER_OBJECT:
        return report(std::max<uint64_t>(nvIn.movesDesc.maxBytesMoved, 64ull * 1024ull),
                      64ull * 1024ull + count * 256ull, "heuristic");
    case NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS:
        return report(64ull * 1024ull + count * 4096ull + uint64_t(nvIn.clasDesc.maxTotalClasCount) * 256ull,
                      64ull * 1024ull + uint64_t(nvIn.clasDesc.maxTotalClasCount) * 128ull, "heuristic");
    default:
        return report(64ull * 1024ull + count * 32ull * 1024ull, 64ull * 1024ull + count * 8ull * 1024ull, "heuristic");
    }
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation(
    ID3D12GraphicsCommandList4* pCommandList,
    const NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS* pParams)
{
    if (!pCommandList || !pParams || !pParams->pDesc) {
        return NVAPI_INVALID_POINTER;
    }

    const auto& nvDesc = *pParams->pDesc;
    Log("[NvAPI] ExecuteMultiIndirectClusterOperation(type=%d, mode=%d, maxOps=%u)",
        nvDesc.inputs.type, nvDesc.inputs.mode, nvDesc.inputs.maxArgCount);
    {
        // First call of each type: full descriptor, for diagnosing engine-specific usage.
        static std::atomic<uint32_t> loggedTypes{0};
        const uint32_t bit = 1u << (static_cast<uint32_t>(nvDesc.inputs.type) & 31u);
        if ((loggedTypes.fetch_or(bit) & bit) == 0) {
            const auto& t = nvDesc.inputs.trianglesDesc;
            Log("[NvAPI]   detail type=%d: flags=0x%x addrFlags=0x%x argCountVA=0x%llx args(VA=0x%llx stride=%llu) "
                "dst(VA=0x%llx stride=%llu) sizes(VA=0x%llx stride=%llu) batchResult=0x%llx batchScratch=0x%llx",
                nvDesc.inputs.type, nvDesc.inputs.flags, nvDesc.addressResolutionFlags,
                static_cast<unsigned long long>(nvDesc.indirectArgCount),
                static_cast<unsigned long long>(nvDesc.indirectArgArray.StartAddress),
                static_cast<unsigned long long>(nvDesc.indirectArgArray.StrideInBytes),
                static_cast<unsigned long long>(nvDesc.destinationAddressArray.StartAddress),
                static_cast<unsigned long long>(nvDesc.destinationAddressArray.StrideInBytes),
                static_cast<unsigned long long>(nvDesc.resultSizeArray.StartAddress),
                static_cast<unsigned long long>(nvDesc.resultSizeArray.StrideInBytes),
                static_cast<unsigned long long>(nvDesc.batchResultData),
                static_cast<unsigned long long>(nvDesc.batchScratchData));
            if (nvDesc.inputs.type == NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS) {
                Log("[NvAPI]   detail clas: maxTotalClas=%u maxClasPerArg=%u",
                    nvDesc.inputs.clasDesc.maxTotalClasCount, nvDesc.inputs.clasDesc.maxClasCountPerArg);
            } else if (nvDesc.inputs.type != NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_MOVE_CLUSTER_OBJECT) {
                Log("[NvAPI]   detail tris: vtxFmt=%d maxGeomIdx=%u maxGeomPerArg=%u maxTriPerArg=%u maxVtxPerArg=%u "
                    "maxTotalTri=%u maxTotalVtx=%u minTrunc=%u",
                    t.vertexFormat, t.maxGeometryIndexValue, t.maxUniqueGeometryCountPerArg, t.maxTriangleCountPerArg,
                    t.maxVertexCountPerArg, t.maxTotalTriangleCount, t.maxTotalVertexCount, t.minPositionTruncateBitCount);
            }
        }
    }
    TranslatedClusterInputs translated{};
    if (!TranslateClusterInputs(nvDesc.inputs, translated)) {
        return NVAPI_INVALID_ARGUMENT;
    }

    ComPtr<ID3D12CommandListRaytracing2> pCmdRt2;
    if (FAILED(pCommandList->QueryInterface(IID_PPV_ARGS(&pCmdRt2))) || !pCmdRt2) {
        Log("[NvAPI] ExecuteMultiIndirectClusterOperation: ID3D12CommandListRaytracing2 not available");
        return NVAPI_NOT_SUPPORTED;
    }

    ComPtr<ID3D12GraphicsCommandList> pGfxCmdList;
    pCommandList->QueryInterface(IID_PPV_ARGS(&pGfxCmdList));

    // The converter binds its own compute root signature/PSO: give the game its state back
    // when this call returns (NvAPI on NVIDIA leaves bound state untouched).
    ComputeStateScope stateScope(pGfxCmdList.Get());

    ConvertedGpuDispatch gpuDispatch{};
    if (!PrepareGpuClusterArguments(pGfxCmdList.Get(), nvDesc, gpuDispatch)) {
        return NVAPI_ERROR;
    }

    D3D12_RTAS_BATCHED_OPERATION_DATA batchedData{};
    batchedData.AddressResolutionFlags              = static_cast<D3D12_RTAS_OPERATION_ADDRESS_RESOLUTION_FLAGS>(nvDesc.addressResolutionFlags);
    batchedData.BatchResultData                     = nvDesc.batchResultData;
    batchedData.BatchScratchData                    = nvDesc.batchScratchData;
    if (translated.inputs.Type == D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS && BlasPrivateScratchEnabled()) {
        ComPtr<ID3D12Device> dev;
        ComPtr<ID3D12DeviceRaytracing2> rt2;
        if (SUCCEEDED(pCommandList->GetDevice(IID_PPV_ARGS(&dev))) && dev &&
            SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&rt2))) && rt2) {
            D3D12_RTAS_OPERATION_PREBUILD_INFO info{};
            rt2->GetRTASOperationPrebuildInfo(&translated.inputs, &info);
            const uint64_t need = std::max<uint64_t>(info.ScratchDataSizeInBytes, 65536);
            if (const D3D12_GPU_VIRTUAL_ADDRESS va = AcquireBlasPrivateScratch(dev.Get(), need)) {
                batchedData.BatchScratchData = va;
            }
        }
    }
    batchedData.ResultAddressArray.StartAddress     = gpuDispatch.resultAddressArrayAddr;
    batchedData.ResultAddressArray.StrideInBytes    = gpuDispatch.resultAddressStride;
    batchedData.ResultSizeArray.StartAddress        = gpuDispatch.resultSizeArrayAddr;
    batchedData.ResultSizeArray.StrideInBytes       = gpuDispatch.resultSizeStride;
    batchedData.IndirectArgumentArray.StartAddress  = gpuDispatch.indirectArgArrayAddr;
    batchedData.IndirectArgumentArray.StrideInBytes = gpuDispatch.indirectArgStride;
    batchedData.IndirectArgumentArraySize           = nvDesc.indirectArgCount;
    batchedData.ToolsInfo                           = 0;

    D3D12_RTAS_OPERATION_DESC opDesc{};
    opDesc.Inputs                = translated.inputs;
    opDesc.pBatchedOperationData = &batchedData;

    // DRED breadcrumb context: names the cluster operation that is in flight when the GPU faults.
    static std::atomic<uint32_t> s_opSeq{0};
    const uint32_t opSeq = ++s_opSeq;
    wchar_t marker[96];
    if (pGfxCmdList) {
        const int n = swprintf_s(marker, L"amd_mg op#%u type=%d max=%u", opSeq,
                                 static_cast<int>(nvDesc.inputs.type), nvDesc.inputs.maxArgCount);
        pGfxCmdList->SetMarker(0, marker, static_cast<UINT>((n + 1) * sizeof(wchar_t)));
    }

    UINT* splitMaxArgs =
        translated.inputs.Type == D3D12_RTAS_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES ? &translated.trianglesDesc.ClusterLimits.MaxArgCount
      : translated.inputs.Type == D3D12_RTAS_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES ? &translated.templatesDesc.ClusterLimits.MaxArgCount
      : translated.inputs.Type == D3D12_RTAS_OPERATION_TYPE_INSTANTIATE_CLUSTER_TEMPLATES ? &translated.instantiateDesc.ClusterLimits.MaxArgCount
      : translated.inputs.Type == D3D12_RTAS_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS ? &translated.clasDesc.MaxArgCount
      : nullptr;
    if (gpuDispatch.chunkSize && splitMaxArgs && pGfxCmdList) {
        // AMD_MG_SPLIT_OPS: one operation per chunk; the shared batch scratch is reused, so each
        // chunk waits for the previous one (UAV barrier). GPU progress markers name the chunk in
        // flight at a device removal.
        const uint32_t total = nvDesc.inputs.maxArgCount;
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        for (uint32_t c = 0; c < gpuDispatch.numChunks; ++c) {
            const uint32_t first = c * gpuDispatch.chunkSize;
            *splitMaxArgs = std::min(gpuDispatch.chunkSize, total - first);
            D3D12_RTAS_BATCHED_OPERATION_DATA chunkData = batchedData;
            chunkData.IndirectArgumentArray.StartAddress =
                gpuDispatch.indirectArgArrayAddr + static_cast<uint64_t>(first) * gpuDispatch.indirectArgStride;
            if (chunkData.ResultAddressArray.StartAddress) {
                const uint64_t s = gpuDispatch.resultAddressStride ? gpuDispatch.resultAddressStride : 8u;
                chunkData.ResultAddressArray.StartAddress += static_cast<uint64_t>(first) * s;
            }
            if (chunkData.ResultSizeArray.StartAddress) {
                const uint64_t s = gpuDispatch.resultSizeStride ? gpuDispatch.resultSizeStride : 8u;
                chunkData.ResultSizeArray.StartAddress += static_cast<uint64_t>(first) * s;
            }
            chunkData.IndirectArgumentArraySize = gpuDispatch.chunkCountsAddr ? gpuDispatch.chunkCountsAddr + c * 4ull : 0;
            D3D12_RTAS_OPERATION_DESC chunkDesc{};
            chunkDesc.Inputs                = translated.inputs;
            chunkDesc.pBatchedOperationData = &chunkData;
            RecordChunkProgress(pGfxCmdList.Get(), gpuDispatch, c, false);
            pCmdRt2->ExecuteIndirectRTASOperations(1, &chunkDesc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
            RecordChunkProgress(pGfxCmdList.Get(), gpuDispatch, c, true);
            pGfxCmdList->ResourceBarrier(1, &uav);
        }
        *splitMaxArgs = total;
    } else {
        pCmdRt2->ExecuteIndirectRTASOperations(1, &opDesc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    }

    if (pGfxCmdList) {
        const int n = swprintf_s(marker, L"amd_mg op#%u done", opSeq);
        pGfxCmdList->SetMarker(0, marker, static_cast<UINT>((n + 1) * sizeof(wchar_t)));
    }

    FinalizeGpuClusterResultSizes(pGfxCmdList.Get(), gpuDispatch);
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo(
    ID3D12Device5* pDevice,
    const NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS* pParams)
{
    if (!pDevice || !pParams || !pParams->pInput || !pParams->pInfo) {
        return NVAPI_INVALID_POINTER;
    }

    TranslatedPtlasInputs translated{};
    TranslatePtlasPrebuildInputs(*pParams, translated);

    ComPtr<ID3D12DeviceRaytracing2> pRt2;
    if (SUCCEEDED(pDevice->QueryInterface(IID_PPV_ARGS(&pRt2))) && pRt2) {
        D3D12_RTAS_OPERATION_PREBUILD_INFO prebuild{};
        pRt2->GetRTASOperationPrebuildInfo(&translated.inputs, &prebuild);
        pParams->pInfo->resultDataMaxSizeInBytes = prebuild.ResultDataMaxSizeInBytes;
        pParams->pInfo->scratchDataSizeInBytes   = prebuild.ScratchDataSizeInBytes;
        return NVAPI_OK;
    }

    uint64_t count = std::max<uint64_t>(1ull, pParams->pInput->instanceCount);
    pParams->pInfo->resultDataMaxSizeInBytes = count * 256ull + 65536ull;
    pParams->pInfo->scratchDataSizeInBytes   = count * 128ull + 65536ull;
    return NVAPI_OK;
}

NvAPI_Status __cdecl Impl_NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect(
    ID3D12GraphicsCommandList4* pCommandList,
    const NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS* pParams)
{
    if (!pCommandList || !pParams || !pParams->pDesc) {
        return NVAPI_INVALID_POINTER;
    }

    const auto& nvDesc = *pParams->pDesc;
    TranslatedPtlasInputs translated{};
    TranslatePtlasBuildInputs(*pParams, translated);

    ComPtr<ID3D12CommandListRaytracing2> pCmdRt2;
    if (FAILED(pCommandList->QueryInterface(IID_PPV_ARGS(&pCmdRt2))) || !pCmdRt2) {
        return NVAPI_NOT_SUPPORTED;
    }

    D3D12_RTAS_PARTITIONED_TLAS_OPERATION_DATA ptlasData{};
    ptlasData.SourceAccelerationStructureData  = nvDesc.srcAccelerationStructureData;
    ptlasData.DestAccelerationStructureData    = nvDesc.destAccelerationStructureData;
    ptlasData.ScratchAccelerationStructureData = nvDesc.scratchAccelerationStructureData;
    ptlasData.IndirectPartitionedTlasOpCount   = nvDesc.indirectOpCount;
    ptlasData.IndirectPartitionedTlasOps       = nvDesc.indirectOps;

    D3D12_RTAS_OPERATION_DESC opDesc{};
    opDesc.Inputs                        = translated.inputs;
    opDesc.pPartitionedTlasOperationData = &ptlasData;

    pCmdRt2->ExecuteIndirectRTASOperations(1, &opDesc, D3D12_EXECUTE_INDIRECT_RTAS_OPERATIONS_FLAG_NONE);
    return NVAPI_OK;
}

} // namespace

} // namespace amd_mg

// ============================================================================
// Exported nvapi_QueryInterface Dispatcher
// ============================================================================
extern "C" __declspec(dllexport) void* __cdecl nvapi_QueryInterface(uint32_t id) {
    using namespace amd_mg;

    switch (id) {
    case 0x0150e828: return reinterpret_cast<void*>(&Impl_NvAPI_Initialize);
    case 0xd22bdd7e: return reinterpret_cast<void*>(&Impl_NvAPI_Unload);
    case 0x6c2d048c: return reinterpret_cast<void*>(&Impl_NvAPI_GetErrorMessage);
    case 0x01053fa5: return reinterpret_cast<void*>(&Impl_NvAPI_GetInterfaceVersionString);
    case 0x85a6c2a0: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_GetRaytracingCaps);
    case 0x3dfacec8: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_IsNvShaderExtnOpCodeSupported);
    case 0x3b03791b: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_QuerySinglePassStereoSupport);
    case 0x5c607a27: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_SetCreatePipelineStateOptions);
    case 0xac2dfeb5: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_SetNvShaderExtnSlotSpace);
    case 0x43d867c0: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread);
    case 0x5c9163f4: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo);
    case 0x67c798af: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation);
    case 0xcdfdc5f2: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo);
    case 0x7cfc6fc3: return reinterpret_cast<void*>(&Impl_NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect);
    default:
        if (auto chain = GetChainedQueryInterface()) {
            if (void* fn = chain(id)) {
                return fn;
            }
        }
        Log("[NvAPI] Unhandled nvapi_QueryInterface(0x%08x)", id);
        return nullptr;
    }
}
