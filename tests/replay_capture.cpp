// ============================================================================
// replay_capture: rebuilds, through the wrapper (build\nvapi64.dll), the templates, CLAS and
// BLAS captured from a game with AMD_MG_CAPTURE_REAL=N (amd_megageometry_capture.bin).
//
//   replay_capture.exe <capture.bin> [--only=B] [--nocount]
//
// The capture holds, for every CLAS of the first N BLAS: the game's template and instance
// arguments, the template's index data and the vertex windows (instance vertices and template
// hint vertices) its indices touch. Addresses are rebased onto one upload buffer; everything else
// (formats, counts, flags, operation inputs) is the game's.
//
// File (little endian): "AMGCAP01", u32 version, u32 M (CLAS slots), u32 N (BLAS), u32 64,
// u32 S, template / instantiate / BLAS NvAPI inputs (S bytes each), u32 dataBytes,
// M x { rec[16 u32], rec2[16 u32], instance args[32], template args[72] }, data.
//   rec:  [0] flags (1 valid, 2 template, 4 indices, 8 vertices, 16 hint, 32 clamped, 64 overflow)
//         [1] BLAS | ref << 8  [2] tri  [3] instance  [4] template  [5] ib delta | istride << 8 | isize << 16
//         [6] ib data offset  [7] ib bytes  [8] min index  [9] max index  [10..12] vb off/bytes/delta
//         [13..15] hint off/bytes/delta
//   rec2: [0..1] instance vb  [2] instance vb stride  [4..5] template hint vb  [6] template vb stride
// ============================================================================
#include "amd_mg.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 722;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

using Inputs = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS;

ComPtr<ID3D12Resource> MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE heap, uint64_t size,
                                  D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = heap;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width            = size < 256 ? 256 : size;
    rd.Height           = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels        = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags            = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> res;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&res)))) {
        std::printf("CreateCommittedResource(%llu) failed\n", static_cast<unsigned long long>(size));
        std::exit(2);
    }
    return res;
}

ComPtr<ID3D12Resource> Upload(ID3D12Device* dev, const void* data, size_t bytes) {
    auto res = MakeBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, bytes, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    void* p = nullptr;
    res->Map(0, nullptr, &p);
    std::memcpy(p, data, bytes);
    res->Unmap(0, nullptr);
    return res;
}

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

struct Rec {
    uint32_t r[16];
    uint32_t r2[16];
    uint8_t  inst[32];
    uint8_t  tpl[72];
};

template <typename T> T Get(const uint8_t* p) { T v; std::memcpy(&v, p, sizeof(T)); return v; }
template <typename T> void Put(uint8_t* p, T v) { std::memcpy(p, &v, sizeof(T)); }

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: replay_capture <capture.bin> [--only=B] [--nocount]\n");
        return 2;
    }
    int only = -1;
    bool gpuCount = true;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--only=", 0) == 0) only = std::atoi(a.c_str() + 7);
        else if (a == "--nocount") gpuCount = false;
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }

    // --- Load the capture
    FILE* f = nullptr;
    if (fopen_s(&f, argv[1], "rb") != 0 || !f) { std::printf("cannot open %s\n", argv[1]); return 2; }
    char magic[8];
    uint32_t version = 0, M = 0, N = 0, recBytes = 0, inSize = 0, dataBytes = 0;
    std::fread(magic, 1, 8, f);
    std::fread(&version, 4, 1, f); std::fread(&M, 4, 1, f); std::fread(&N, 4, 1, f);
    std::fread(&recBytes, 4, 1, f); std::fread(&inSize, 4, 1, f);
    if (std::memcmp(magic, "AMGCAP01", 8) != 0 || version != 1 || recBytes != 64 || inSize != sizeof(Inputs)) {
        std::printf("not a version-1 capture (or NvAPI inputs size %u != %zu)\n", inSize, sizeof(Inputs));
        return 2;
    }
    Inputs tIn{}, iIn{}, bIn{};
    std::fread(&tIn, sizeof(Inputs), 1, f); std::fread(&iIn, sizeof(Inputs), 1, f); std::fread(&bIn, sizeof(Inputs), 1, f);
    std::fread(&dataBytes, 4, 1, f);
    std::vector<Rec> recs(M);
    for (auto& r : recs) {
        std::fread(r.r, 64, 1, f); std::fread(r.r2, 64, 1, f); std::fread(r.inst, 32, 1, f); std::fread(r.tpl, 72, 1, f);
    }
    std::vector<uint8_t> data(static_cast<size_t>(dataBytes) + 65536, 0);
    std::fread(data.data(), 1, dataBytes, f);
    std::fclose(f);
    std::printf("capture: %u BLAS, %u CLAS slots, %u data bytes | templates: flags=0x%x vtxFmt=%d maxGeom=%u tri=%u vtx=%u trunc=%u | instantiate flags=0x%x | BLAS flags=0x%x clas=%u/%u\n",
                N, M, dataBytes, tIn.flags, tIn.trianglesDesc.vertexFormat, tIn.trianglesDesc.maxGeometryIndexValue,
                tIn.trianglesDesc.maxTriangleCountPerArg, tIn.trianglesDesc.maxVertexCountPerArg,
                tIn.trianglesDesc.minPositionTruncateBitCount, iIn.flags, bIn.flags,
                bIn.clasDesc.maxTotalClasCount, bIn.clasDesc.maxClasCountPerArg);

    // --- Wrapper entry points
    HMODULE hNvapi = LoadLibraryA("nvapi64.dll");
    if (!hNvapi) { std::printf("nvapi64.dll not found (run from build\\)\n"); return 2; }
    using PFN_QI = void* (__cdecl*)(uint32_t);
    auto qi = reinterpret_cast<PFN_QI>(GetProcAddress(hNvapi, "nvapi_QueryInterface"));
    using PFN_Init   = NvAPI_Status(__cdecl*)();
    using PFN_GetReq = NvAPI_Status(__cdecl*)(ID3D12Device5*, const NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS*);
    using PFN_Exec   = NvAPI_Status(__cdecl*)(ID3D12GraphicsCommandList4*, const NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS*);
    auto fnInit   = reinterpret_cast<PFN_Init>(qi(0x0150e828));
    auto fnGetReq = reinterpret_cast<PFN_GetReq>(qi(0x5c9163f4));
    auto fnExec   = reinterpret_cast<PFN_Exec>(qi(0x67c798af));
    fnInit();
    ComPtr<ID3D12Device5> dev;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev)))) {
        std::printf("D3D12CreateDevice failed\n");
        return 2;
    }
    auto getReq = [&](const Inputs& in) {
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO info{};
        NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS p{};
        p.version = NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_VER;
        p.pInput  = &in;
        p.pInfo   = &info;
        fnGetReq(dev.Get(), &p);
        return info;
    };

    // --- Select the CLAS to rebuild
    auto blob = Upload(dev.Get(), data.data(), data.size());
    const uint64_t blobVa = blob->GetGPUVirtualAddress();
    std::vector<uint32_t> use;          // record indices, in BLAS / reference order
    std::map<uint32_t, uint32_t> tplOf; // game template index -> replay template index
    std::vector<uint32_t> tplRec;       // replay template -> first record using it
    uint32_t skipped = 0;
    for (uint32_t s = 0; s < M; ++s) {
        const Rec& r = recs[s];
        if (!(r.r[0] & 1u)) continue;
        const uint32_t b = r.r[1] & 0xFFu;
        if (only >= 0 && static_cast<int>(b) != only) continue;
        if ((r.r[0] & 0xEu) != 0xEu || (r.r[0] & 64u)) {
            ++skipped;
            std::printf("  skip CLAS slot %u (BLAS %u ref %u): flags=0x%x\n", s, b, r.r[1] >> 8, r.r[0]);
            continue;
        }
        if (!tplOf.count(r.r[4])) {
            tplOf[r.r[4]] = static_cast<uint32_t>(tplRec.size());
            tplRec.push_back(s);
        }
        use.push_back(s);
    }
    if (use.empty()) { std::printf("nothing to replay\n"); return 2; }
    std::printf("replay: %zu CLAS from %zu templates (%u skipped)\n", use.size(), tplRec.size(), skipped);

    // --- Templates
    const uint32_t U = static_cast<uint32_t>(tplRec.size());
    Inputs t1 = tIn; t1.maxArgCount = 1;
    Inputs tN = tIn; tN.maxArgCount = U;
    const auto tReq1 = getReq(t1), tReqN = getReq(tN);
    const uint64_t tSize = AlignUp(tReq1.resultDataMaxSizeInBytes, 256);
    auto tBuf = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tSize * U, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    std::vector<uint8_t> tArgs(static_cast<size_t>(U) * 72, 0);
    std::vector<uint64_t> tDst(U);
    for (uint32_t u = 0; u < U; ++u) {
        const Rec& r = recs[tplRec[u]];
        uint8_t* a = tArgs.data() + static_cast<size_t>(u) * 72;
        std::memcpy(a, r.tpl, 72);
        const uint32_t delta = r.r[5] & 0xFFu, lo = r.r[8];
        Put<uint64_t>(a + 24, blobVa + r.r[6] + delta);
        uint64_t hint = 0;
        if (r.r[0] & 16u) {
            const uint32_t tvs = r.r2[6] ? r.r2[6] : r.r2[2];
            hint = blobVa + r.r[13] + r.r[15] - static_cast<uint64_t>(lo) * tvs;
        }
        Put<uint64_t>(a + 32, hint);
        Put<uint64_t>(a + 64, 0); // bounding box limit not captured
        tDst[u] = tBuf->GetGPUVirtualAddress() + u * tSize;
    }

    // --- Instances (CLAS)
    const uint32_t C = static_cast<uint32_t>(use.size());
    Inputs i1 = iIn; i1.maxArgCount = 1;
    Inputs iN = iIn; iN.maxArgCount = C;
    const auto iReq1 = getReq(i1), iReqN = getReq(iN);
    const uint64_t cSize = AlignUp(iReq1.resultDataMaxSizeInBytes, 256), cStride = cSize * 2;
    auto cBuf = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, cStride * C, D3D12_RESOURCE_STATE_COPY_DEST, true);
    std::vector<uint8_t> iArgs(static_cast<size_t>(C) * 32, 0);
    std::vector<uint64_t> cDst(C);
    std::vector<uint32_t> clasBlas(C);
    for (uint32_t c = 0; c < C; ++c) {
        const Rec& r = recs[use[c]];
        uint8_t* a = iArgs.data() + static_cast<size_t>(c) * 32;
        std::memcpy(a, r.inst, 32);
        Put<uint64_t>(a + 8, tDst[tplOf[r.r[4]]]);
        const uint32_t vs = r.r2[2], lo = r.r[8];
        Put<uint64_t>(a + 16, blobVa + r.r[10] + r.r[12] - static_cast<uint64_t>(lo) * vs);
        cDst[c] = cBuf->GetGPUVirtualAddress() + c * cStride;
        clasBlas[c] = r.r[1] & 0xFFu;
    }

    // --- BLAS
    std::vector<amd_mg::NvClasToBlasArgs> bArgs;
    std::vector<uint64_t> clasArray(cDst);
    std::vector<uint32_t> bIndex;
    uint32_t maxPer = 0;
    for (uint32_t c = 0; c < C;) {
        uint32_t e = c;
        while (e < C && clasBlas[e] == clasBlas[c]) ++e;
        amd_mg::NvClasToBlasArgs a{};
        a.clasCount = e - c;
        a.reserved = 0;
        a.clasAddresses = 0; // patched after upload
        bArgs.push_back(a);
        bIndex.push_back(c);
        maxPer = std::max(maxPer, e - c);
        c = e;
    }
    const uint32_t B = static_cast<uint32_t>(bArgs.size());
    auto arrBuf = Upload(dev.Get(), clasArray.data(), clasArray.size() * 8);
    for (uint32_t b = 0; b < B; ++b) bArgs[b].clasAddresses = arrBuf->GetGPUVirtualAddress() + uint64_t(bIndex[b]) * 8;
    Inputs b1 = bIn; b1.maxArgCount = 1; b1.clasDesc.maxClasCountPerArg = maxPer; b1.clasDesc.maxTotalClasCount = maxPer;
    Inputs bN = bIn; bN.maxArgCount = B;
    bN.clasDesc.maxClasCountPerArg = std::max<uint32_t>(bN.clasDesc.maxClasCountPerArg, maxPer);
    bN.clasDesc.maxTotalClasCount  = std::max<uint32_t>(bN.clasDesc.maxTotalClasCount, C);
    const auto bReq1 = getReq(b1), bReqN = getReq(bN);
    const uint64_t bSize = AlignUp(bReq1.resultDataMaxSizeInBytes, 256), bStride = bSize * 2;
    auto bBuf = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bStride * B, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    std::vector<uint64_t> bDst(B);
    for (uint32_t b = 0; b < B; ++b) {
        bDst[b] = bBuf->GetGPUVirtualAddress() + b * bStride;
        std::printf("  BLAS %u: %u CLAS (game BLAS %u)\n", b, bArgs[b].clasCount, clasBlas[bIndex[b]]);
    }

    const uint64_t scratchNeed = std::max({ tReqN.scratchDataSizeInBytes, iReqN.scratchDataSizeInBytes, bReqN.scratchDataSizeInBytes });
    auto scratch = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, AlignUp(scratchNeed + 256, 65536), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    auto tArgBuf = Upload(dev.Get(), tArgs.data(), tArgs.size());
    auto tDstBuf = Upload(dev.Get(), tDst.data(), tDst.size() * 8);
    auto iArgBuf = Upload(dev.Get(), iArgs.data(), iArgs.size());
    auto cDstBuf = Upload(dev.Get(), cDst.data(), cDst.size() * 8);
    auto bArgBuf = Upload(dev.Get(), bArgs.data(), bArgs.size() * sizeof(bArgs[0]));
    auto bDstBuf = Upload(dev.Get(), bDst.data(), bDst.size() * 8);
    const uint32_t counts[2] = { C, B };
    auto countBuf = Upload(dev.Get(), counts, sizeof(counts));
    std::printf("sizes: template %llu, CLAS %llu, BLAS %llu (max %u CLAS), scratch %llu\n",
                static_cast<unsigned long long>(tSize), static_cast<unsigned long long>(cSize),
                static_cast<unsigned long long>(bSize), maxPer, static_cast<unsigned long long>(scratchNeed));

    // --- Record (compute queue, like the game's AsyncCompute)
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ComPtr<ID3D12CommandQueue> queue;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> alloc;
    dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList4> cl;
    dev->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));

    std::vector<uint8_t> pattern(static_cast<size_t>(cStride * C), 0xCD);
    auto patBuf = Upload(dev.Get(), pattern.data(), pattern.size());
    cl->CopyBufferRegion(cBuf.Get(), 0, patBuf.Get(), 0, cStride * C);
    D3D12_RESOURCE_BARRIER tb{};
    tb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    tb.Transition.pResource   = cBuf.Get();
    tb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    tb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    tb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &tb);
    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;

    NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS ep{};
    ep.version = NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS_VER;
    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC tDesc{};
    tDesc.inputs = tN;
    tDesc.batchScratchData        = scratch->GetGPUVirtualAddress();
    tDesc.destinationAddressArray = { tDstBuf->GetGPUVirtualAddress(), 8 };
    tDesc.indirectArgArray        = { tArgBuf->GetGPUVirtualAddress(), 72 };
    ep.pDesc = &tDesc;
    const NvAPI_Status s1 = fnExec(cl.Get(), &ep);
    cl->ResourceBarrier(1, &uav);
    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC iDesc{};
    iDesc.inputs = iN;
    iDesc.batchScratchData        = scratch->GetGPUVirtualAddress();
    iDesc.destinationAddressArray = { cDstBuf->GetGPUVirtualAddress(), 8 };
    iDesc.indirectArgArray        = { iArgBuf->GetGPUVirtualAddress(), 32 };
    iDesc.indirectArgCount        = gpuCount ? countBuf->GetGPUVirtualAddress() : 0;
    ep.pDesc = &iDesc;
    const NvAPI_Status s2 = fnExec(cl.Get(), &ep);
    cl->ResourceBarrier(1, &uav);
    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC bDesc{};
    bDesc.inputs = bN;
    bDesc.batchScratchData        = scratch->GetGPUVirtualAddress();
    bDesc.destinationAddressArray = { bDstBuf->GetGPUVirtualAddress(), 8 };
    bDesc.indirectArgArray        = { bArgBuf->GetGPUVirtualAddress(), sizeof(amd_mg::NvClasToBlasArgs) };
    bDesc.indirectArgCount        = gpuCount ? countBuf->GetGPUVirtualAddress() + 4 : 0;
    ep.pDesc = &bDesc;
    const NvAPI_Status s3 = fnExec(cl.Get(), &ep);
    cl->ResourceBarrier(1, &uav);

    auto rbC = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, cStride * C, D3D12_RESOURCE_STATE_COPY_DEST, false);
    std::swap(tb.Transition.StateBefore, tb.Transition.StateAfter);
    tb.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &tb);
    cl->CopyBufferRegion(rbC.Get(), 0, cBuf.Get(), 0, cStride * C);
    cl->Close();
    std::printf("exec template=%d instantiate=%d blas=%d\n", s1, s2, s3);

    ID3D12CommandList* lists[] = { cl.Get() };
    queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 3;
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    const DWORD w = WaitForSingleObject(ev, 10000);
    const HRESULT removed = dev->GetDeviceRemovedReason();
    std::printf("RESULT: %s (wait=%s, removed=0x%08X)\n",
                (w == WAIT_OBJECT_0 && SUCCEEDED(removed)) ? "OK" : "DEVICE LOST",
                w == WAIT_OBJECT_0 ? "signaled" : "timeout", static_cast<unsigned>(removed));
    if (w == WAIT_OBJECT_0 && SUCCEEDED(removed)) {
        void* mapped = nullptr;
        rbC->Map(0, nullptr, &mapped);
        const uint8_t* p = static_cast<const uint8_t*>(mapped);
        uint32_t over = 0, maxW = 0;
        for (uint32_t c = 0; c < C; ++c) {
            uint32_t last = 0;
            for (uint64_t b = 0; b < cStride; ++b) if (p[c * cStride + b] != 0xCD) last = static_cast<uint32_t>(b + 1);
            maxW = std::max(maxW, last);
            if (last > cSize) ++over;
        }
        rbC->Unmap(0, nullptr);
        std::printf("CLAS: reported size=%llu, max bytes written=%u, overflowing=%u/%u\n",
                    static_cast<unsigned long long>(cSize), maxW, over, C);
    }
    Sleep(500); // let the wrapper's DRED callback finish writing
    return (w == WAIT_OBJECT_0 && SUCCEEDED(removed)) ? 0 : 1;
}
