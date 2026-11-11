#include "amd_mg.h"
#include "dxcapi.h"
#include "nvShaderExtnEnums.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

int g_passed = 0;
int g_failed = 0;

void Check(bool cond, const char* name) {
    if (cond) {
        std::printf("  [PASS] %s\n", name);
        g_passed++;
    } else {
        std::printf("  [FAIL] %s\n", name);
        g_failed++;
    }
}

ComPtr<ID3D12Resource> CreateUploadBuffer(ID3D12Device* dev, const void* data, size_t bytes) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = bytes;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;

    void* mapped = nullptr;
    res->Map(0, nullptr, &mapped);
    std::memcpy(mapped, data, bytes);
    res->Unmap(0, nullptr);
    return res;
}

} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    _putenv_s("AMD_MG_LOG", "1");
    _putenv_s("AMD_MG_FORCE_CAPS", "1");

    std::printf("================================================================\n");
    std::printf(" amd-megageometry Test Suite (RDNA 4 / RX 9070 XT)\n");
    std::printf("================================================================\n\n");

    // ------------------------------------------------------------------------
    // 1. Load nvapi64.dll and test nvapi_QueryInterface
    // ------------------------------------------------------------------------
    std::printf("[1/4] Testing nvapi64.dll & nvapi_QueryInterface...\n");
    HMODULE hNvapi = LoadLibraryA("build\\nvapi64.dll");
    if (!hNvapi) {
        hNvapi = LoadLibraryA("nvapi64.dll");
    }
    Check(hNvapi != nullptr, "LoadLibraryA(nvapi64.dll)");
    if (!hNvapi) {
        return 1;
    }

    using PFN_QueryInterface = void* (__cdecl*)(uint32_t);
    auto pfnQI = reinterpret_cast<PFN_QueryInterface>(GetProcAddress(hNvapi, "nvapi_QueryInterface"));
    Check(pfnQI != nullptr, "GetProcAddress(nvapi_QueryInterface)");

    using PFN_Initialize = NvAPI_Status(__cdecl*)();
    using PFN_GetVer     = NvAPI_Status(__cdecl*)(NvAPI_ShortString);
    using PFN_GetCaps    = NvAPI_Status(__cdecl*)(ID3D12Device*, NVAPI_D3D12_RAYTRACING_CAPS_TYPE, void*, size_t);
    using PFN_IsOpSupp   = NvAPI_Status(__cdecl*)(ID3D12Device*, NvU32, bool*);
    using PFN_SetSlot    = NvAPI_Status(__cdecl*)(IUnknown*, NvU32, NvU32);
    using PFN_GetReqInfo = NvAPI_Status(__cdecl*)(ID3D12Device5*, const NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS*);
    using PFN_ExecClust  = NvAPI_Status(__cdecl*)(ID3D12GraphicsCommandList4*, const NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS*);
    using PFN_PtlasPre   = NvAPI_Status(__cdecl*)(ID3D12Device5*, const NVAPI_GET_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PREBUILD_INFO_PARAMS*);
    using PFN_PtlasBuild = NvAPI_Status(__cdecl*)(ID3D12GraphicsCommandList4*, const NVAPI_BUILD_RAYTRACING_PARTITIONED_TLAS_INDIRECT_PARAMS*);

    auto fnInit       = reinterpret_cast<PFN_Initialize>(pfnQI(0x0150e828));
    auto fnGetVer     = reinterpret_cast<PFN_GetVer>(pfnQI(0x01053fa5));
    auto fnGetCaps    = reinterpret_cast<PFN_GetCaps>(pfnQI(0x85a6c2a0));
    auto fnIsOpSupp   = reinterpret_cast<PFN_IsOpSupp>(pfnQI(0x3dfacec8));
    auto fnSetSlot    = reinterpret_cast<PFN_SetSlot>(pfnQI(0xac2dfeb5));
    auto fnGetReqInfo = reinterpret_cast<PFN_GetReqInfo>(pfnQI(0x5c9163f4));
    auto fnExecClust  = reinterpret_cast<PFN_ExecClust>(pfnQI(0x67c798af));
    auto fnPtlasPre   = reinterpret_cast<PFN_PtlasPre>(pfnQI(0xcdfdc5f2));
    auto fnPtlasBuild = reinterpret_cast<PFN_PtlasBuild>(pfnQI(0x7cfc6fc3));

    Check(fnInit && fnGetVer && fnGetCaps && fnIsOpSupp && fnSetSlot && fnGetReqInfo && fnExecClust && fnPtlasPre && fnPtlasBuild,
          "All 9 Mega Geometry NvAPI function IDs resolved");

    Check(fnInit() == NVAPI_OK, "NvAPI_Initialize() == NVAPI_OK");

    NvAPI_ShortString verStr{};
    Check(fnGetVer(verStr) == NVAPI_OK, "NvAPI_GetInterfaceVersionString()");
    std::printf("       Version string: \"%s\"\n", verStr);

    // ------------------------------------------------------------------------
    // 2. Initialize D3D12 on RX 9070 XT & test NvAPI D3D12 Caps / Prebuild
    // ------------------------------------------------------------------------
    std::printf("\n[2/4] Testing D3D12 Device on AMD Radeon RX 9070 XT...\n");
    ComPtr<IDXGIFactory6> factory;
    CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> adapter;
    factory->EnumAdapters1(0, &adapter);
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    std::printf("       GPU: %ls (Vendor=0x%04X, Device=0x%04X)\n", desc.Description, desc.VendorId, desc.DeviceId);

    ComPtr<ID3D12Device5> device;
    amd_mg::EnableDredBeforeDeviceCreation();
    HRESULT hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device));
    amd_mg::WatchDeviceRemoval(device.Get());
    Check(SUCCEEDED(hr) && device != nullptr, "D3D12CreateDevice on primary GPU");

    ComPtr<ID3D12DeviceRaytracing2> rt2Dev;
    HRESULT hrRt2 = device->QueryInterface(IID_PPV_ARGS(&rt2Dev));
    std::printf("       ID3D12DeviceRaytracing2 QueryInterface: 0x%08X (%s)\n",
                static_cast<unsigned>(hrRt2),
                SUCCEEDED(hrRt2) ? "Preview active!" : "Preview inactive - enable Windows Developer Mode for live RTAS2");

    NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS clusterCaps = NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_NONE;
    Check(fnGetCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS, &clusterCaps, sizeof(clusterCaps)) == NVAPI_OK &&
          clusterCaps == NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_STANDARD,
          "NvAPI_D3D12_GetRaytracingCaps(CLUSTER_OPERATIONS) == STANDARD");

    NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAPS ptlasCaps = NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_NONE;
    Check(fnGetCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_PARTITIONED_TLAS, &ptlasCaps, sizeof(ptlasCaps)) == NVAPI_OK &&
          ptlasCaps == NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_STANDARD,
          "NvAPI_D3D12_GetRaytracingCaps(PARTITIONED_TLAS) == STANDARD");

    bool op86 = false, op93 = false;
    fnIsOpSupp(device.Get(), NV_EXTN_OP_RT_TRIANGLE_OBJECT_POSITIONS, &op86);
    fnIsOpSupp(device.Get(), NV_EXTN_OP_RT_GET_CLUSTER_ID, &op93);
    Check(op86 && op93, "NvAPI_D3D12_IsNvShaderExtnOpCodeSupported(86 & 93) == true");

    // AMD_MG_DISABLE=1: the A/B switch must hide Mega Geometry from NvAPI and DXR queries alike.
    {
        SetEnvironmentVariableA("AMD_MG_DISABLE", "1");
        NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS offCluster = NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_STANDARD;
        NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAPS   offPtlas   = NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_STANDARD;
        fnGetCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS, &offCluster, sizeof(offCluster));
        fnGetCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_PARTITIONED_TLAS, &offPtlas, sizeof(offPtlas));
        bool off93 = true;
        fnIsOpSupp(device.Get(), NV_EXTN_OP_RT_GET_CLUSTER_ID, &off93);
        D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2 rt2Opts{};
        device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2, &rt2Opts, sizeof(rt2Opts)); // hooked via the NvAPI calls above
        SetEnvironmentVariableA("AMD_MG_DISABLE", nullptr);
        Check(offCluster == NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_NONE &&
              offPtlas == NVAPI_D3D12_RAYTRACING_PARTITIONED_TLAS_CAP_NONE && !off93 && !rt2Opts.ClustersAndPTLASSupported,
              "AMD_MG_DISABLE=1 hides Cluster Ops / PTLAS (NvAPI caps, opcode 93, OPTIONS_RAYTRACING2)");

        NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAPS onCluster = NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_NONE;
        fnGetCaps(device.Get(), NVAPI_D3D12_RAYTRACING_CAPS_TYPE_CLUSTER_OPERATIONS, &onCluster, sizeof(onCluster));
        Check(onCluster == NVAPI_D3D12_RAYTRACING_CLUSTER_OPERATIONS_CAP_STANDARD,
              "Clearing AMD_MG_DISABLE restores CLUSTER_OPERATIONS == STANDARD");
    }

    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS clusterInputs{};
    clusterInputs.maxArgCount                           = 128;
    clusterInputs.flags                                 = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_FAST_TRACE;
    clusterInputs.type                                  = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLAS_FROM_TRIANGLES;
    clusterInputs.mode                                  = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_IMPLICIT_DESTINATIONS;
    clusterInputs.trianglesDesc.vertexFormat            = DXGI_FORMAT_R32G32B32_FLOAT;
    clusterInputs.trianglesDesc.maxTriangleCountPerArg  = 128;
    clusterInputs.trianglesDesc.maxVertexCountPerArg    = 128;
    clusterInputs.trianglesDesc.maxTotalTriangleCount   = 16384;
    clusterInputs.trianglesDesc.maxTotalVertexCount     = 16384;

    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO reqOut{};
    NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS reqParams{};
    reqParams.version = NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_VER;
    reqParams.pInput  = &clusterInputs;
    reqParams.pInfo   = &reqOut;

    Check(fnGetReqInfo(device.Get(), &reqParams) == NVAPI_OK && reqOut.resultDataMaxSizeInBytes > 0 && reqOut.scratchDataSizeInBytes > 0,
          "NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo()");
    std::printf("       CLAS Prebuild Info (128 clusters): Result=%llu B, Scratch=%llu B\n",
                reqOut.resultDataMaxSizeInBytes, reqOut.scratchDataSizeInBytes);

    // ------------------------------------------------------------------------
    // 3. Test GPU Compute Indirect Argument Converter on RX 9070 XT
    // ------------------------------------------------------------------------
    std::printf("\n[3/4] Testing GPU Compute Shader Argument Conversion (64B NvAPI -> 80B DXR)...\n");

    D3D12_COMMAND_QUEUE_DESC qDesc{};
    qDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    device->CreateCommandQueue(&qDesc, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> cmdAlloc;
    device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc.Get(), nullptr, IID_PPV_ARGS(&cmdList));

    amd_mg::NvTrianglesToClasArgs nvClasArg{};
    nvClasArg.clusterId                         = 0xCAFE1234u;
    nvClasArg.clusterFlags                      = 0x3u;
    nvClasArg.packedCounts                      = (64u) | (96u << 9) | (12u << 18) | (2u << 24); // 16-bit indices
    nvClasArg.baseGeometryIndexAndFlags         = 0x01000042u;
    nvClasArg.indexBufferStride                 = 0; // "index size" -> must become an explicit 2
    nvClasArg.vertexBufferStride                = 12;
    nvClasArg.geometryIndexAndFlagsBufferStride = 4;
    nvClasArg.opacityMicromapIndexBufferStride  = 2;
    nvClasArg.indexBuffer                       = 0x0000000111110000ull;
    nvClasArg.vertexBuffer                      = 0x0000000222220000ull;
    nvClasArg.geometryIndexAndFlagsBuffer       = 0x0000000333330000ull;
    nvClasArg.opacityMicromapArray              = 0x0000000444440000ull;
    nvClasArg.opacityMicromapIndexBuffer        = 0x0000000555550000ull;

    D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS expectedDxrClas{};
    amd_mg::ConvertClasFromTrianglesCPU(nvClasArg, expectedDxrClas);

    auto uploadBuf = CreateUploadBuffer(device.Get(), &nvClasArg, sizeof(nvClasArg));
    const uint32_t gpuArgCount = 1;
    auto countBuf  = CreateUploadBuffer(device.Get(), &gpuArgCount, sizeof(gpuArgCount));

    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC opDesc{};
    opDesc.inputs                         = clusterInputs;
    opDesc.inputs.maxArgCount             = 2; // GPU count (1) must win: element 1 does not exist
    opDesc.indirectArgArray.StartAddress  = uploadBuf->GetGPUVirtualAddress();
    opDesc.indirectArgArray.StrideInBytes = sizeof(nvClasArg);
    opDesc.indirectArgCount               = countBuf->GetGPUVirtualAddress();

    amd_mg::ConvertedGpuDispatch gpuDispatch{};
    bool prepOk = amd_mg::PrepareGpuClusterArguments(cmdList.Get(), opDesc, gpuDispatch);
    Check(prepOk && gpuDispatch.indirectArgStride == 80 && gpuDispatch.indirectArgArrayAddr != 0 && gpuDispatch.argSlot >= 0,
          "PrepareGpuClusterArguments dispatched CS_ConvertArgs (64B -> 80B) on RX 9070 XT");

    // Read back what the GPU converter wrote (slot is in NON_PIXEL_SHADER_RESOURCE between
    // Prepare and Finalize, i.e. where ExecuteIndirectRTASOperations would consume it).
    ComPtr<ID3D12Resource> readback;
    {
        D3D12_HEAP_PROPERTIES rb{};
        rb.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 2 * 80;
        d.Height = 1; d.DepthOrArraySize = 1; d.MipLevels = 1; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
    }
    ID3D12Resource* slotRes = amd_mg::GetConverterScratchForTest(device.Get(), gpuDispatch.argSlot);
    if (slotRes && readback) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = slotRes;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cmdList->ResourceBarrier(1, &b);
        cmdList->CopyBufferRegion(readback.Get(), 0, slotRes, 0, 80);
        std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
        cmdList->ResourceBarrier(1, &b);
    }
    amd_mg::FinalizeGpuClusterResultSizes(cmdList.Get(), gpuDispatch);

    cmdList->Close();
    ID3D12CommandList* lists[] = { cmdList.Get() };
    queue->ExecuteCommandLists(1, lists);

    ComPtr<ID3D12Fence> fence;
    device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    queue->Signal(fence.Get(), 1);
    HANDLE hEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, hEv);
    WaitForSingleObject(hEv, 5000);
    CloseHandle(hEv);
    Check(SUCCEEDED(device->GetDeviceRemovedReason()), "Converter command list executed without device removal");

    D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS gpuDxrClas{};
    if (readback) {
        void* p = nullptr;
        D3D12_RANGE r{ 0, 80 };
        if (SUCCEEDED(readback->Map(0, &r, &p))) {
            std::memcpy(&gpuDxrClas, p, sizeof(gpuDxrClas));
            readback->Unmap(0, nullptr);
        }
    }
    Check(std::memcmp(&gpuDxrClas, &expectedDxrClas, sizeof(gpuDxrClas)) == 0,
          "GPU CS_ConvertArgs output matches CPU reference converter byte-for-byte");

    // The slot must be recycled once the GPU completion marker has landed.
    cmdAlloc->Reset();
    cmdList->Reset(cmdAlloc.Get(), nullptr);
    amd_mg::ConvertedGpuDispatch gpuDispatch2{};
    bool prepOk2 = amd_mg::PrepareGpuClusterArguments(cmdList.Get(), opDesc, gpuDispatch2);
    Check(prepOk2 && gpuDispatch2.argSlot == gpuDispatch.argSlot,
          "Converter scratch slot recycled after GPU completion marker (WriteBufferImmediate)");
    amd_mg::FinalizeGpuClusterResultSizes(cmdList.Get(), gpuDispatch2);
    cmdList->Close();
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 2);
    hEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(2, hEv);
    WaitForSingleObject(hEv, 5000);
    CloseHandle(hEv);

    Check(expectedDxrClas.ClusterID == 0xCAFE1234u &&
          expectedDxrClas.TriangleCount == 64 &&
          expectedDxrClas.UniqueVertexIndexCount == 96 &&
          expectedDxrClas.VertexBufferStride == 12 &&
          expectedDxrClas.IndexBufferStride == 2 &&
          expectedDxrClas.PositionTruncateBitCount == 12 &&
          expectedDxrClas.VertexBuffer == 0x0000000222220000ull &&
          expectedDxrClas.IndexBuffer == 0x0000000111110000ull,
          "80-byte D3D12_RTAS_OPERATION_BUILD_CLAS_FROM_TRIANGLES_ARGS field mapping verified (16-bit idx, stride 0 -> 2)");

    // ------------------------------------------------------------------------
    // 3b. Compute state restored after the converter dispatch (Northlight-style engines keep
    //     using the root signature / PSO they bound before the NvAPI call).
    // ------------------------------------------------------------------------
    {
        amd_mg::HookDeviceCommandListsForStateShadow(device.Get());

        using PFN_D3DCompile = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        HMODULE hc = LoadLibraryW(L"d3dcompiler_47.dll");
        auto pCompile = hc ? reinterpret_cast<PFN_D3DCompile>(GetProcAddress(hc, "D3DCompile")) : nullptr;
        const char* src =
            "cbuffer C : register(b0) { uint v; };\n"
            "RWByteAddressBuffer o : register(u0);\n"
            "[numthreads(1,1,1)] void main() { o.Store(0, v); }\n";
        ComPtr<ID3DBlob> cs, err;
        bool compiled = pCompile && SUCCEEDED(pCompile(src, std::strlen(src), "state_test", nullptr, nullptr,
                                                        "main", "cs_5_0", 0, 0, &cs, &err));

        D3D12_ROOT_PARAMETER rp[2]{};
        rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp[0].Constants.Num32BitValues = 1;
        rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_ROOT_SIGNATURE_DESC rsd{ 2, rp, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE };
        ComPtr<ID3DBlob> rsBlob;
        ComPtr<ID3D12RootSignature> gameRs;
        ComPtr<ID3D12PipelineState> gamePso;
        if (compiled && SUCCEEDED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1_0, &rsBlob, &err))) {
            device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&gameRs));
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = gameRs.Get();
            pd.CS = { cs->GetBufferPointer(), cs->GetBufferSize() };
            device->CreateComputePipelineState(&pd, IID_PPV_ARGS(&gamePso));
        }

        D3D12_HEAP_PROPERTIES dh{}; dh.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = 256; bd.Height = 1; bd.DepthOrArraySize = 1;
        bd.MipLevels = 1; bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> outBuf;
        device->CreateCommittedResource(&dh, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&outBuf));

        uint32_t gameValue = 0;
        if (gameRs && gamePso && outBuf && readback) {
            cmdAlloc->Reset();
            cmdList->Reset(cmdAlloc.Get(), nullptr);
            // "Game" binds its compute state, then calls the NvAPI cluster op, then dispatches.
            cmdList->SetComputeRootSignature(gameRs.Get());
            cmdList->SetPipelineState(gamePso.Get());
            cmdList->SetComputeRoot32BitConstant(0, 0x1234ABCDu, 0);
            cmdList->SetComputeRootUnorderedAccessView(1, outBuf->GetGPUVirtualAddress());
            amd_mg::ConvertedGpuDispatch d3{};
            {
                amd_mg::ComputeStateScope scope(cmdList.Get());
                amd_mg::PrepareGpuClusterArguments(cmdList.Get(), opDesc, d3);
                amd_mg::FinalizeGpuClusterResultSizes(cmdList.Get(), d3);
            }
            cmdList->Dispatch(1, 1, 1);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = outBuf.Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            cmdList->ResourceBarrier(1, &b);
            cmdList->CopyBufferRegion(readback.Get(), 0, outBuf.Get(), 0, 4);
            cmdList->Close();
            queue->ExecuteCommandLists(1, lists);
            queue->Signal(fence.Get(), 3);
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            fence->SetEventOnCompletion(3, ev);
            WaitForSingleObject(ev, 5000);
            CloseHandle(ev);
            void* p = nullptr;
            D3D12_RANGE r{ 0, 4 };
            if (SUCCEEDED(readback->Map(0, &r, &p))) {
                std::memcpy(&gameValue, p, 4);
                readback->Unmap(0, nullptr);
            }
        }
        std::printf("  state test: compiled=%d rs=%d pso=%d value=0x%08X\n", compiled ? 1 : 0, gameRs ? 1 : 0, gamePso ? 1 : 0, gameValue);
        Check(compiled && gameValue == 0x1234ABCDu,
              "Game compute root signature, root args and PSO restored after converter dispatch");
        Check(SUCCEEDED(device->GetDeviceRemovedReason()), "State-restore command list executed without device removal");
    }

    // ------------------------------------------------------------------------
    // 4. Test DXIL Shader Patcher (NV_EXTN_OP 93 & 86 -> SM 6.10 DXIL)
    // ------------------------------------------------------------------------
    std::printf("\n[4/4] Testing DXIL Bytecode Patcher (nvHLSLExtns -> SM 6.10)...\n");

    std::ifstream ifs("build\\test_nv_shader.dxil", std::ios::binary | std::ios::ate);
    Check(ifs.is_open(), "Loaded compiled NvAPI cluster shader (build\\test_nv_shader.dxil)");
    if (ifs.is_open()) {
        size_t sz = static_cast<size_t>(ifs.tellg());
        ifs.seekg(0);
        std::vector<uint8_t> origDxil(sz);
        ifs.read(reinterpret_cast<char*>(origDxil.data()), sz);

        std::vector<uint8_t> patchedDxil;
        bool patchedOk = amd_mg::PatchDxilClusterShader(origDxil.data(), origDxil.size(), 63, 0, patchedDxil);
        Check(patchedOk && !patchedDxil.empty(), "PatchDxilClusterShader() disassembled, transformed & re-assembled container");

        if (patchedOk) {
            std::ofstream ofs("build\\patched_test.dxil", std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(patchedDxil.data()), patchedDxil.size());
            ofs.close();

            HMODULE hDxc = GetModuleHandleW(L"dxcompiler.dll");
            auto pfnCreate = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(hDxc, "DxcCreateInstance"));
            ComPtr<IDxcCompiler3> comp;
            pfnCreate(CLSID_DxcCompiler, IID_PPV_ARGS(&comp));
            DxcBuffer buf{ patchedDxil.data(), patchedDxil.size(), 0 };
            ComPtr<IDxcResult> res;
            comp->Disassemble(&buf, IID_PPV_ARGS(&res));
            ComPtr<IDxcBlobEncoding> disBlob;
            res->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&disBlob), nullptr);
            std::string disText(static_cast<const char*>(disBlob->GetBufferPointer()), disBlob->GetBufferSize());

            Check(disText.find("@dx.op.clusterID(i32 -2147483645)") != std::string::npos,
                  "Patched DXIL contains native @dx.op.clusterID(i32 -2147483645)");
            Check(disText.find("@dx.op.triangleObjectPosition.f32(i32 -2147483641)") != std::string::npos,
                  "Patched DXIL contains native @dx.op.triangleObjectPosition.f32(i32 -2147483641)");
            Check(disText.find("!{!\"lib\", i32 6, i32 10}") != std::string::npos,
                  "Patched DXIL upgraded shaderModel metadata to lib_6_10");
            Check(disText.find("call i32 @dx.op.bufferUpdateCounter") == std::string::npos,
                  "Patched DXIL removed all NvAPI fake-UAV bufferUpdateCounter calls");

            // Test live ID3D12Device5::CreateStateObject hook + amdxc64.dll compilation on RX 9070 XT!
            fnSetSlot(device.Get(), 63, 0);

            D3D12_DXIL_LIBRARY_DESC libDesc{};
            libDesc.DXILLibrary.pShaderBytecode = origDxil.data();
            libDesc.DXILLibrary.BytecodeLength  = origDxil.size();

            D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{};
            shaderCfg.MaxPayloadSizeInBytes   = 16;
            shaderCfg.MaxAttributeSizeInBytes = 8;

            D3D12_RAYTRACING_PIPELINE_CONFIG pipeCfg{};
            pipeCfg.MaxTraceRecursionDepth = 1;

            D3D12_ROOT_PARAMETER rootParam{};
            rootParam.ParameterType             = D3D12_ROOT_PARAMETER_TYPE_UAV;
            rootParam.Descriptor.ShaderRegister = 63;
            rootParam.Descriptor.RegisterSpace  = 0;
            rootParam.ShaderVisibility          = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC rsDesc{};
            rsDesc.NumParameters = 1;
            rsDesc.pParameters   = &rootParam;
            ComPtr<ID3DBlob> rsBlob;
            D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, nullptr);
            ComPtr<ID3D12RootSignature> globalRs;
            device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&globalRs));
            D3D12_GLOBAL_ROOT_SIGNATURE globalRsSub{ globalRs.Get() };

            D3D12_STATE_SUBOBJECT subobjects[4]{};
            subobjects[0].Type  = D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY;
            subobjects[0].pDesc = &libDesc;
            subobjects[1].Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG;
            subobjects[1].pDesc = &shaderCfg;
            subobjects[2].Type  = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG;
            subobjects[2].pDesc = &pipeCfg;
            subobjects[3].Type  = D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE;
            subobjects[3].pDesc = &globalRsSub;

            D3D12_STATE_OBJECT_DESC soDesc{};
            soDesc.Type          = D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE;
            soDesc.NumSubobjects = 4;
            soDesc.pSubobjects   = subobjects;

            ComPtr<ID3D12StateObject> rtpso;
            HRESULT hrSo = device->CreateStateObject(&soDesc, IID_PPV_ARGS(&rtpso));
            std::printf("       ID3D12Device5::CreateStateObject (patched SM 6.10 on RX 9070 XT): 0x%08X\n", static_cast<unsigned>(hrSo));
            Check(SUCCEEDED(hrSo) && rtpso != nullptr,
                  "ID3D12Device5::CreateStateObject compiled patched NvAPI->SM 6.10 RTPSO on RX 9070 XT");
        }
    }

    std::ifstream ifsNative("build\\native_nv.dxil", std::ios::binary | std::ios::ate);
    if (ifsNative.is_open()) {
        size_t sz = static_cast<size_t>(ifsNative.tellg());
        ifsNative.seekg(0);
        std::vector<uint8_t> origNative(sz);
        ifsNative.read(reinterpret_cast<char*>(origNative.data()), sz);
        std::vector<uint8_t> patchedNative;
        bool okNative = amd_mg::PatchDxilClusterShader(origNative.data(), origNative.size(), 1000, 0, patchedNative);
        Check(okNative && !patchedNative.empty(), "PatchDxilClusterShader(build\\native_nv.dxil) with ILDB strip succeeded");
        if (okNative) {
            std::ofstream ofsN("build\\patched_native_nv.dxil", std::ios::binary);
            ofsN.write(reinterpret_cast<const char*>(patchedNative.data()), patchedNative.size());
        }
    }

    // DRED watch: removing the device on purpose must produce the breadcrumb dump in the log.
    {
        ComPtr<ID3D12Device5> dev5;
        if (device && SUCCEEDED(device.As(&dev5))) {
            dev5->RemoveDevice();
            bool dumped = false;
            for (int i = 0; i < 40 && !dumped; ++i) {
                Sleep(50);
                std::ifstream lf("build\\amd_megageometry.log");
                std::string line;
                while (std::getline(lf, line)) {
                    if (line.find("[DRED] ===== Device removed") != std::string::npos) {
                        dumped = true;
                    }
                }
            }
            Check(dumped, "DRED dump written to the log on device removal");
        }
    }

    std::printf("\n================================================================\n");
    std::printf(" Summary: %d PASSED, %d FAILED\n", g_passed, g_failed);
    std::printf("================================================================\n");

    return (g_failed == 0) ? 0 : 1;
}
