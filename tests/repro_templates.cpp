// ============================================================================
// repro_templates: standalone NvAPI cluster-template build + instantiation through the wrapper
// (build\nvapi64.dll), shaped like Alan Wake 2's usage, with one variable changed at a time.
//
//   repro_templates.exe [--fmt=f32|snorm16] [--idx=8|16|32] [--vstride=N] [--gpucount]
//                       [--n=templates] [--inst=instances] [--hint] [--fasttrace] [--blas]
//                       [--tri=N] [--smallsize] [--idxbase=N] [--empty=N] [--emptydst]
//                       [--idxspread=K] [--trunc=N] [--vboff=N] [--iboff=N] [--blas=N] [--span] [--aw2 (all AW2 traits)]
//
// Defaults reproduce the RTXMG-like known-good case (f32 vertices, 8-bit indices). The process
// reports whether the device survived; on removal the wrapper's DRED dump lands in
// build\amd_megageometry.log.
// ============================================================================
#include "amd_mg.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 722;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

constexpr uint32_t maxTri = 64, maxVtx = 192; // batch limits (AW2)

struct Options {
    bool     snorm16   = false;
    uint32_t idxBytes  = 1;
    uint32_t vstride   = 0;   // 0 = natural (NvAPI semantics)
    bool     gpuCount  = false;
    uint32_t templates = 64;
    uint32_t instances = 256;
    bool     hint      = false;
    bool     fastTrace = false;
    uint32_t tri       = 64;    // actual triangles per cluster (vertices = 3 * tri)
    bool     smallSize = false; // size slots with a per-cluster query using the actual counts
    uint32_t empty     = 0;     // last N template args all-zero (tri=0, null pointers): unused slots
    bool     emptyDst  = false; // ...and their destination address is 0
    uint32_t idxBase   = 0;
    uint32_t idxSpread = 1;     // index i = idxBase + i * spread (distinct values, > 255 collide under UINT8)
    uint32_t trunc     = 0;     // positionTruncateBitCount (AW2: 8)
    uint32_t blasPer   = 0;     // > 0: build BLAS from CLAS, N CLAS per BLAS (guarded, reported size)
    uint32_t vtxDecl   = 0;     // > 0: template declares this unique vertex count instead of the real one (3 * tri)
    float    instScale = 1.0f;  // instance vertex positions = hint positions * S (AW2: instances differ from the hint)
    bool     instRand  = false; // instance vertex positions random in [-1, 1] (template hint stays the grid)
    bool     pack      = false; // AW2 layout: each BLAS (reported size) immediately followed by its CLAS (reported size), one buffer
    int      scratchFill = -1;  // 0..255: fill the batch scratch with this byte right before the BLAS (AW2 reuses a dirty scratch)
    uint32_t geomCount = 1;     // > 1: template i gets geometry index i % K (AW2 BLAS mix geometry 0 and 1)
    bool     cid0      = false; // instance clusterIdOffset 0 (AW2: duplicate cluster IDs inside a BLAS)
    uint32_t blasBig   = 0;     // > 0: the first BLAS takes M CLAS, the others blasPer (AW2: 499 BLAS of 2..4458)
    uint32_t ibOff     = 0;     // index buffer start offset in bytes (AW2: 2-byte aligned starts)
    uint32_t vbOff     = 0;     // vertex buffer start offset in bytes (AW2: 4-byte aligned starts)
    bool     span      = false; // 6 GB reserved VA gaps: templates, CLAS and vertex data in different 4 GB windows     // added to every index (mesh-absolute indices, like AW2: index >= vertex count)
};

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

int16_t ToSnorm16(float f) {
    f = f < -1.f ? -1.f : (f > 1.f ? 1.f : f);
    return static_cast<int16_t>(std::lround(f * 32767.f));
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&](const char* k) { return a.substr(std::strlen(k)); };
        if (a == "--fmt=snorm16") o.snorm16 = true;
        else if (a == "--fmt=f32") o.snorm16 = false;
        else if (a.rfind("--idx=", 0) == 0) o.idxBytes = static_cast<uint32_t>(std::stoul(val("--idx="))) / 8u;
        else if (a.rfind("--vstride=", 0) == 0) o.vstride = static_cast<uint32_t>(std::stoul(val("--vstride=")));
        else if (a == "--gpucount") o.gpuCount = true;
        else if (a.rfind("--n=", 0) == 0) o.templates = static_cast<uint32_t>(std::stoul(val("--n=")));
        else if (a.rfind("--inst=", 0) == 0) o.instances = static_cast<uint32_t>(std::stoul(val("--inst=")));
        else if (a == "--hint") o.hint = true;
        else if (a == "--fasttrace") o.fastTrace = true;
        else if (a == "--blas") o.blasPer = 64;
        else if (a.rfind("--tri=", 0) == 0) o.tri = static_cast<uint32_t>(std::stoul(val("--tri=")));
        else if (a == "--smallsize") o.smallSize = true;
        else if (a.rfind("--empty=", 0) == 0) o.empty = static_cast<uint32_t>(std::stoul(val("--empty=")));
        else if (a == "--emptydst") o.emptyDst = true;
        else if (a.rfind("--idxspread=", 0) == 0) o.idxSpread = static_cast<uint32_t>(std::stoul(val("--idxspread=")));
        else if (a.rfind("--trunc=", 0) == 0) o.trunc = static_cast<uint32_t>(std::stoul(val("--trunc=")));
        else if (a.rfind("--geom=", 0) == 0) o.geomCount = static_cast<uint32_t>(std::stoul(val("--geom=")));
        else if (a == "--cid0") o.cid0 = true;
        else if (a == "--pack") o.pack = true;
        else if (a.rfind("--instscale=", 0) == 0) o.instScale = std::stof(val("--instscale="));
        else if (a == "--instrand") o.instRand = true;
        else if (a.rfind("--vtxdecl=", 0) == 0) o.vtxDecl = static_cast<uint32_t>(std::stoul(val("--vtxdecl=")));
        else if (a.rfind("--scratchfill=", 0) == 0) o.scratchFill = static_cast<int>(std::stoul(val("--scratchfill="), nullptr, 0) & 0xFF);
        else if (a.rfind("--blasbig=", 0) == 0) o.blasBig = static_cast<uint32_t>(std::stoul(val("--blasbig=")));
        else if (a.rfind("--blas=", 0) == 0) o.blasPer = static_cast<uint32_t>(std::stoul(val("--blas=")));
        else if (a.rfind("--iboff=", 0) == 0) o.ibOff = static_cast<uint32_t>(std::stoul(val("--iboff=")));
        else if (a.rfind("--vboff=", 0) == 0) o.vbOff = static_cast<uint32_t>(std::stoul(val("--vboff=")));
        else if (a == "--span") o.span = true;
        else if (a == "--aw2") { o.snorm16 = true; o.idxBytes = 2; o.hint = true; o.vstride = 8; o.trunc = 8;
                                 o.vbOff = 4; o.idxBase = 49; o.idxSpread = 3; o.span = true; o.fastTrace = true; o.gpuCount = true; }
        else if (a.rfind("--idxbase=", 0) == 0) o.idxBase = static_cast<uint32_t>(std::stoul(val("--idxbase=")));
        else { std::printf("unknown option %s\n", a.c_str()); return 2; }
    }
    if (o.idxBytes != 1 && o.idxBytes != 2 && o.idxBytes != 4) {
        std::printf("--idx must be 8, 16 or 32\n");
        return 2;
    }

    HMODULE hNvapi = LoadLibraryA("nvapi64.dll");
    if (!hNvapi) { std::printf("nvapi64.dll not found (run from build\\)\n"); return 2; }
    using PFN_QI = void* (__cdecl*)(uint32_t);
    auto qi = reinterpret_cast<PFN_QI>(GetProcAddress(hNvapi, "nvapi_QueryInterface"));
    using PFN_Init    = NvAPI_Status(__cdecl*)();
    using PFN_GetReq  = NvAPI_Status(__cdecl*)(ID3D12Device5*, const NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS*);
    using PFN_Exec    = NvAPI_Status(__cdecl*)(ID3D12GraphicsCommandList4*, const NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS*);
    auto fnInit   = reinterpret_cast<PFN_Init>(qi(0x0150e828));
    auto fnGetReq = reinterpret_cast<PFN_GetReq>(qi(0x5c9163f4));
    auto fnExec   = reinterpret_cast<PFN_Exec>(qi(0x67c798af));
    fnInit();

    ComPtr<ID3D12Device5> dev;
    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&dev)))) {
        std::printf("D3D12CreateDevice failed\n");
        return 2;
    }

    // --- Geometry: 64 triangles with 192 unshared vertices (AW2 limits: maxTri 64, maxVtx 192)
    const uint32_t triCount = o.tri, vtxCount = 3 * o.tri;
    const uint32_t vtxSize  = o.snorm16 ? 8u : 12u;  // DXGI element size (RGBA16 / RGB32F)
    const uint32_t vstride  = o.vstride ? o.vstride : vtxSize;
    std::vector<uint8_t> verts(static_cast<size_t>(vstride) * (vtxCount * (o.instances + 1) + o.idxBase + 3 * o.tri * o.idxSpread), 0);
    for (uint32_t inst = 0; inst <= o.instances; ++inst) {
        for (uint32_t v = 0; v < vtxCount; ++v) {
            const uint32_t t = v / 3, c = v % 3;
            float x = (t % 8) * 0.1f + (c == 1 ? 0.05f : 0.f) - 0.5f;
            float y = (t / 8) * 0.1f + (c == 2 ? 0.05f : 0.f) - 0.5f;
            float z = 0.001f * static_cast<float>(inst % 100);
            if (inst > 0 && o.instRand) {
                // Block 0 is the template hint; instance blocks get unrelated positions.
                uint32_t h = (inst * 2654435761u) ^ (v * 40503u + 0x9E3779B9u);
                auto rnd = [&h]() { h ^= h << 13; h ^= h >> 17; h ^= h << 5; return (h & 0xFFFFu) / 32767.5f - 1.0f; };
                x = rnd(); y = rnd(); z = rnd();
            } else if (inst > 0 && o.instScale != 1.0f) {
                auto cl = [](float f) { return f < -1.f ? -1.f : (f > 1.f ? 1.f : f); };
                x = cl(x * o.instScale); y = cl(y * o.instScale); z = cl(z * o.instScale + 0.25f);
            }
            uint8_t* dst = verts.data() + (static_cast<size_t>(inst) * vtxCount + v) * vstride;
            if (o.snorm16) {
                int16_t q[4] = { ToSnorm16(x), ToSnorm16(y), ToSnorm16(z), 32767 };
                std::memcpy(dst, q, sizeof(q));
            } else {
                float f[3] = { x, y, z };
                std::memcpy(dst, f, sizeof(f));
            }
        }
    }
    std::vector<uint8_t> indices(static_cast<size_t>(triCount) * 3 * o.idxBytes, 0);
    for (uint32_t i = 0; i < triCount * 3; ++i) {
        const uint32_t v = i * o.idxSpread + o.idxBase;
        std::memcpy(indices.data() + static_cast<size_t>(i) * o.idxBytes, &v, o.idxBytes); // little endian
    }
    std::printf("index base=%u spread=%u (indices %u..%u, unique vertex count %u) trunc=%u vboff=%u span=%d\n", o.idxBase, o.idxSpread, o.idxBase, o.idxBase + (triCount * 3 - 1) * o.idxSpread, vtxCount, o.trunc, o.vbOff, o.span ? 1 : 0);
    // Reserved (unbacked) VA ranges push the following allocations into other 4 GB windows.
    std::vector<ComPtr<ID3D12Resource>> vaGaps;
    auto gap = [&]() {
        if (!o.span) return;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 6ull << 30;
        rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1; rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        if (FAILED(dev->CreateReservedResource(&rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r)))) {
            std::printf("CreateReservedResource(6 GB) failed\n");
            return;
        }
        vaGaps.push_back(r);
    };
    if (o.vbOff) verts.insert(verts.begin(), o.vbOff, 0);
    auto vb = Upload(dev.Get(), verts.data(), verts.size());
    const D3D12_GPU_VIRTUAL_ADDRESS vbVa = vb->GetGPUVirtualAddress() + o.vbOff;
    if (o.ibOff) indices.insert(indices.begin(), o.ibOff, 0);
    auto ib = Upload(dev.Get(), indices.data(), indices.size());

    const DXGI_FORMAT vfmt = o.snorm16 ? DXGI_FORMAT_R16G16B16A16_SNORM : DXGI_FORMAT_R32G32B32_FLOAT;
    const uint32_t idxFmtNv = o.idxBytes; // NvAPI: 1 = 8 bit, 2 = 16 bit, 4 = 32 bit

    auto makeInputs = [&](NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE type, uint32_t maxArgs,
                          uint32_t limTri = maxTri, uint32_t limVtx = maxVtx) {
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS in{};
        in.maxArgCount = maxArgs;
        in.flags = (o.fastTrace && type == NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES)
            ? NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_FAST_TRACE
            : NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_NONE;
        in.type = type;
        in.mode = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_EXPLICIT_DESTINATIONS;
        in.trianglesDesc.vertexFormat                 = vfmt;
        in.trianglesDesc.maxGeometryIndexValue        = 63;
        in.trianglesDesc.maxUniqueGeometryCountPerArg = 1;
        in.trianglesDesc.maxTriangleCountPerArg       = limTri;
        in.trianglesDesc.maxVertexCountPerArg         = limVtx;
        in.trianglesDesc.maxTotalTriangleCount        = limTri * maxArgs;
        in.trianglesDesc.maxTotalVertexCount          = limVtx * maxArgs;
        in.trianglesDesc.minPositionTruncateBitCount  = o.trunc;
        return in;
    };
    auto getReq = [&](const NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS& in) {
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO info{};
        NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS p{};
        p.version = NVAPI_GET_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO_PARAMS_VER;
        p.pInput  = &in;
        p.pInfo   = &info;
        fnGetReq(dev.Get(), &p);
        return info;
    };

    // --- Template build (explicit destinations)
    auto tIn   = makeInputs(NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES, o.templates);
    auto tReq1 = getReq(makeInputs(NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES, 1,
                                   o.smallSize ? triCount : maxTri, o.smallSize ? vtxCount : maxVtx));
    auto tReqN = getReq(tIn);
    const uint64_t tSize = AlignUp(tReq1.resultDataMaxSizeInBytes, 32);

    std::vector<amd_mg::NvTrianglesToTemplateArgs> tArgs(o.templates);
    for (uint32_t i = 0; i < o.templates; ++i) {
        auto& a = tArgs[i];
        std::memset(&a, 0, sizeof(a));
        a.clusterId    = i;
        const uint32_t vtxDeclared = o.vtxDecl ? o.vtxDecl : vtxCount;
        a.packedCounts = triCount | (vtxDeclared << 9) | (o.trunc << 18) | (idxFmtNv << 24);
        a.baseGeometryIndexAndFlags = o.geomCount > 1 ? i % o.geomCount : 0;
        a.indexBufferStride  = 0;
        a.vertexBufferStride = static_cast<uint16_t>(o.vstride);
        a.indexBuffer  = ib->GetGPUVirtualAddress() + o.ibOff;
        a.vertexBuffer = o.hint ? vbVa : 0;
    }
    if (o.empty >= o.templates) { std::printf("--empty must be < --n\n"); return 2; }
    const uint32_t validTemplates = o.templates - o.empty;
    for (uint32_t i = validTemplates; i < o.templates; ++i) std::memset(&tArgs[i], 0, sizeof(tArgs[i]));
    std::printf("empty template args=%u (dst %s)\n", o.empty, o.emptyDst ? "null" : "valid");
    auto tArgBuf = Upload(dev.Get(), tArgs.data(), tArgs.size() * sizeof(tArgs[0]));
    // Every slot is followed by a guard of the same size, prefilled with 0xCD: an implementation
    // that writes more than the size it reported shows up as modified guard bytes.
    const uint64_t tStride = tSize * 2;
    gap();
    auto tBuf    = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, tStride * o.templates,
                              D3D12_RESOURCE_STATE_COPY_DEST, true);
    std::vector<uint64_t> tDst(o.templates);
    for (uint32_t i = 0; i < o.templates; ++i) tDst[i] = tBuf->GetGPUVirtualAddress() + i * tStride;
    std::vector<uint64_t> tDstUpload = tDst;
    if (o.emptyDst) for (uint32_t i = validTemplates; i < o.templates; ++i) tDstUpload[i] = 0;
    auto tDstBuf = Upload(dev.Get(), tDstUpload.data(), tDstUpload.size() * 8);

    // --- Instantiation (explicit destinations)
    auto iIn   = makeInputs(NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_INSTANTIATE_CLUSTER_TEMPLATES, o.instances);
    auto iReq1 = getReq(makeInputs(NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_INSTANTIATE_CLUSTER_TEMPLATES, 1,
                                   o.smallSize ? triCount : maxTri, o.smallSize ? vtxCount : maxVtx));
    auto iReqN = getReq(iIn);
    const uint64_t cSize = AlignUp(iReq1.resultDataMaxSizeInBytes, 128);

    std::vector<amd_mg::NvInstantiateTemplateArgs> iArgs(o.instances);
    for (uint32_t i = 0; i < o.instances; ++i) {
        auto& a = iArgs[i];
        a.clusterIdOffset     = o.cid0 ? 0u : 1000u * i;
        a.geometryIndexOffset = 0;
        a.clusterTemplate     = tDst[i % validTemplates];
        a.vertexBufferStartAddress  = vbVa + static_cast<uint64_t>(i + 1) * vtxCount * vstride;
        a.vertexBufferStrideInBytes = o.vstride;
    }
    auto iArgBuf = Upload(dev.Get(), iArgs.data(), iArgs.size() * sizeof(iArgs[0]));
    const uint64_t cStride = cSize * 2;
    gap();
    auto cBuf    = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, cStride * o.instances,
                              D3D12_RESOURCE_STATE_COPY_DEST, true);
    std::vector<uint64_t> cDst(o.instances);
    for (uint32_t i = 0; i < o.instances; ++i) cDst[i] = cBuf->GetGPUVirtualAddress() + i * cStride;
    // --pack: AW2 places each BLAS right before its own CLAS, both at exactly the reported size
    // (256-aligned), in one buffer. Guard checks do not apply to this layout.
    ComPtr<ID3D12Resource> packBuf;
    uint64_t packBlasSize = 0, packClasSize = 0, packGroup = 0;
    if (o.pack && o.blasPer && !o.blasBig) {
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS pb{};
        pb.maxArgCount = 1;
        pb.type = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        pb.mode = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_EXPLICIT_DESTINATIONS;
        pb.clasDesc.maxTotalClasCount  = o.blasPer;
        pb.clasDesc.maxClasCountPerArg = o.blasPer;
        packBlasSize = AlignUp(getReq(pb).resultDataMaxSizeInBytes, 256);
        packClasSize = AlignUp(iReq1.resultDataMaxSizeInBytes, 256);
        packGroup    = packBlasSize + packClasSize * o.blasPer;
        const uint32_t groups = o.instances / o.blasPer;
        const uint64_t rest   = static_cast<uint64_t>(o.instances - groups * o.blasPer) * packClasSize;
        packBuf = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, packGroup * groups + rest + 256,
                             D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
        const uint64_t base = packBuf->GetGPUVirtualAddress();
        for (uint32_t i = 0; i < o.instances; ++i) {
            const uint32_t g = i / o.blasPer, j = i % o.blasPer;
            cDst[i] = g < groups ? base + g * packGroup + packBlasSize + j * packClasSize
                                 : base + groups * packGroup + static_cast<uint64_t>(i - groups * o.blasPer) * packClasSize;
        }
        std::printf("pack: BLAS %llu bytes + %u CLAS x %llu bytes per group, %u groups (guard checks not meaningful)\n",
                    static_cast<unsigned long long>(packBlasSize), o.blasPer,
                    static_cast<unsigned long long>(packClasSize), groups);
    }
    auto cDstBuf = Upload(dev.Get(), cDst.data(), cDst.size() * 8);
    // --- BLAS from CLAS (optional): blasCount BLAS of o.blasPer consecutive CLAS each.
    const uint32_t blasBig   = (o.blasPer && o.blasBig && o.blasBig <= o.instances) ? o.blasBig : 0;
    const uint32_t blasCount = !o.blasPer ? 0
        : blasBig ? 1 + (o.instances - blasBig) / o.blasPer : o.instances / o.blasPer;
    const uint32_t blasMaxPer = blasBig > o.blasPer ? blasBig : o.blasPer;
    const uint32_t blasTotal  = blasBig ? blasBig + (blasCount - 1) * o.blasPer : blasCount * o.blasPer;
    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS bIn{};
    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_REQUIREMENTS_INFO bReq1{}, bReqN{};
    uint64_t bSize = 0, bStride = 0, bBigSize = 0, bBigStride = 0, bBigOff = 0, bBufBytes = 0;
    ComPtr<ID3D12Resource> bBuf, bArgBuf, bDstBuf;
    if (blasCount) {
        bIn.maxArgCount = blasCount;
        bIn.flags = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_FLAG_NONE;
        bIn.type  = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_TYPE_BUILD_BLAS_FROM_CLAS;
        bIn.mode  = NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_MODE_EXPLICIT_DESTINATIONS;
        bIn.clasDesc.maxTotalClasCount  = blasTotal;
        bIn.clasDesc.maxClasCountPerArg = blasMaxPer;
        // Slot sizes come from maxOps=1 queries for the slot's own CLAS count, like AW2. With
        // --blasbig the big BLAS (argument 0) lives after the small slots.
        auto b1 = bIn;
        b1.maxArgCount = 1;
        b1.clasDesc.maxTotalClasCount  = o.blasPer;
        b1.clasDesc.maxClasCountPerArg = o.blasPer;
        bReq1 = getReq(b1);
        bReqN = getReq(bIn);
        bSize   = AlignUp(bReq1.resultDataMaxSizeInBytes, 256);
        bStride = bSize * 2;
        const uint32_t smallCount = blasBig ? blasCount - 1 : blasCount;
        bBigOff   = bStride * smallCount;
        bBufBytes = bBigOff;
        if (blasBig) {
            auto bb1 = b1;
            bb1.clasDesc.maxTotalClasCount  = blasBig;
            bb1.clasDesc.maxClasCountPerArg = blasBig;
            bBigSize   = AlignUp(getReq(bb1).resultDataMaxSizeInBytes, 256);
            bBigStride = bBigSize * 2;
            bBufBytes += bBigStride;
        }
        bBuf = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, bBufBytes, D3D12_RESOURCE_STATE_COPY_DEST, true);
        std::vector<amd_mg::NvClasToBlasArgs> bArgs(blasCount);
        std::vector<uint64_t> bDst(blasCount);
        uint64_t firstClas = 0;
        for (uint32_t b = 0; b < blasCount; ++b) {
            const uint32_t per = (blasBig && b == 0) ? blasBig : o.blasPer;
            bArgs[b].clasCount     = per;
            bArgs[b].reserved      = 0;
            bArgs[b].clasAddresses = cDstBuf->GetGPUVirtualAddress() + firstClas * 8ull;
            firstClas += per;
            bDst[b] = bBuf->GetGPUVirtualAddress() + (blasBig ? (b == 0 ? bBigOff : (b - 1) * bStride) : b * bStride);
            if (packBuf) bDst[b] = packBuf->GetGPUVirtualAddress() + b * packGroup;
        }
        bArgBuf = Upload(dev.Get(), bArgs.data(), bArgs.size() * sizeof(bArgs[0]));
        bDstBuf = Upload(dev.Get(), bDst.data(), bDst.size() * 8);
        if (blasBig) std::printf("BLAS: first of %u CLAS (size=%llu), max per arg %u, total %u CLAS\n", blasBig,
                                 static_cast<unsigned long long>(bBigSize), blasMaxPer, blasTotal);
        std::printf("BLAS: %u x %u CLAS, size=%llu (batch scratch %llu)\n", blasCount, o.blasPer,
                    static_cast<unsigned long long>(bReq1.resultDataMaxSizeInBytes),
                    static_cast<unsigned long long>(bReqN.scratchDataSizeInBytes));
    }
    const uint32_t countVal[3] = { o.instances, o.templates, blasCount };
    auto countBuf = Upload(dev.Get(), countVal, sizeof(countVal));

    uint64_t scratchNeed = tReqN.scratchDataSizeInBytes > iReqN.scratchDataSizeInBytes
        ? tReqN.scratchDataSizeInBytes : iReqN.scratchDataSizeInBytes;
    if (bReqN.scratchDataSizeInBytes > scratchNeed) scratchNeed = bReqN.scratchDataSizeInBytes;
    const uint64_t scratchSize = AlignUp(scratchNeed + 256, 256);
    std::printf("VA: vb=0x%llx ib=0x%llx templates=0x%llx CLAS=0x%llx\n",
                static_cast<unsigned long long>(vbVa), static_cast<unsigned long long>(ib->GetGPUVirtualAddress()),
                static_cast<unsigned long long>(tBuf->GetGPUVirtualAddress()), static_cast<unsigned long long>(cBuf->GetGPUVirtualAddress()));
    auto scratch = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_DEFAULT, scratchSize, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, true);
    ComPtr<ID3D12Resource> scratchPat;
    if (o.scratchFill >= 0) {
        std::vector<uint8_t> sp(static_cast<size_t>(scratchSize), static_cast<uint8_t>(o.scratchFill));
        scratchPat = Upload(dev.Get(), sp.data(), sp.size());
        std::printf("scratch: %llu bytes, filled with 0x%02X before the BLAS\n",
                    static_cast<unsigned long long>(scratchSize), o.scratchFill);
    }

    std::printf("fmt=%s idx=%u-bit vstride=%u gpucount=%d templates=%u instances=%u hint=%d fasttrace=%d\n",
                o.snorm16 ? "R16G16B16A16_SNORM" : "R32G32B32_FLOAT", o.idxBytes * 8, o.vstride, o.gpuCount ? 1 : 0,
                o.templates, o.instances, o.hint ? 1 : 0, o.fastTrace ? 1 : 0);
    std::printf("template size=%llu (batch scratch %llu)  CLAS size=%llu (batch scratch %llu)\n",
                tReq1.resultDataMaxSizeInBytes, tReqN.scratchDataSizeInBytes,
                iReq1.resultDataMaxSizeInBytes, iReqN.scratchDataSizeInBytes);

    // --- Record
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE; // AW2 runs these on its AsyncCompute queue
    ComPtr<ID3D12CommandQueue> queue;
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> alloc;
    dev->CreateCommandAllocator(qd.Type, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList4> cl;
    dev->CreateCommandList(0, qd.Type, alloc.Get(), nullptr, IID_PPV_ARGS(&cl));

    // Prefill template/CLAS buffers with the guard pattern, then move them to the AS state.
    uint64_t fillBytes = (tStride * o.templates > cStride * o.instances) ? tStride * o.templates : cStride * o.instances;
    if (bBufBytes > fillBytes) fillBytes = bBufBytes;
    std::vector<uint8_t> pattern(static_cast<size_t>(fillBytes), 0xCD);
    auto patBuf = Upload(dev.Get(), pattern.data(), pattern.size());
    cl->CopyBufferRegion(tBuf.Get(), 0, patBuf.Get(), 0, tStride * o.templates);
    cl->CopyBufferRegion(cBuf.Get(), 0, patBuf.Get(), 0, cStride * o.instances);
    if (blasCount) {
        cl->CopyBufferRegion(bBuf.Get(), 0, patBuf.Get(), 0, bBufBytes);
        D3D12_RESOURCE_BARRIER bb{};
        bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb.Transition.pResource   = bBuf.Get();
        bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cl->ResourceBarrier(1, &bb);
    }
    {
        D3D12_RESOURCE_BARRIER b[2]{};
        for (int k = 0; k < 2; ++k) {
            b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[k].Transition.pResource   = k == 0 ? tBuf.Get() : cBuf.Get();
            b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            b[k].Transition.StateAfter  = D3D12_RESOURCE_STATE_UNORDERED_ACCESS; // AS state cannot be transitioned
        }
        cl->ResourceBarrier(2, b);
    }

    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC tDesc{};
    tDesc.inputs = tIn;
    tDesc.batchScratchData = scratch->GetGPUVirtualAddress();
    tDesc.destinationAddressArray = { tDstBuf->GetGPUVirtualAddress(), 8 };
    tDesc.indirectArgArray        = { tArgBuf->GetGPUVirtualAddress(), sizeof(amd_mg::NvTrianglesToTemplateArgs) };
    tDesc.indirectArgCount        = 0;
    NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS ep{};
    ep.version = NVAPI_RAYTRACING_EXECUTE_MULTI_INDIRECT_CLUSTER_OPERATION_PARAMS_VER;
    ep.pDesc   = &tDesc;
    NvAPI_Status s1 = fnExec(cl.Get(), &ep);

    D3D12_RESOURCE_BARRIER uav{};
    uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl->ResourceBarrier(1, &uav);

    NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC iDesc{};
    iDesc.inputs = iIn;
    iDesc.batchScratchData = scratch->GetGPUVirtualAddress();
    iDesc.destinationAddressArray = { cDstBuf->GetGPUVirtualAddress(), 8 };
    iDesc.indirectArgArray        = { iArgBuf->GetGPUVirtualAddress(), sizeof(amd_mg::NvInstantiateTemplateArgs) };
    iDesc.indirectArgCount        = o.gpuCount ? countBuf->GetGPUVirtualAddress() : 0;
    ep.pDesc = &iDesc;
    NvAPI_Status s2 = fnExec(cl.Get(), &ep);
    cl->ResourceBarrier(1, &uav);
    NvAPI_Status s3 = NVAPI_OK;
    ComPtr<ID3D12Resource> rbB;
    if (blasCount) {
        NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_DESC bDesc{};
        bDesc.inputs = bIn;
        bDesc.batchScratchData = scratch->GetGPUVirtualAddress();
        if (scratchPat) {
            D3D12_RESOURCE_BARRIER sb{};
            sb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            sb.Transition.pResource   = scratch.Get();
            sb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            sb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            sb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_DEST;
            cl->ResourceBarrier(1, &sb);
            cl->CopyBufferRegion(scratch.Get(), 0, scratchPat.Get(), 0, scratchSize);
            std::swap(sb.Transition.StateBefore, sb.Transition.StateAfter);
            cl->ResourceBarrier(1, &sb);
        }
        bDesc.destinationAddressArray = { bDstBuf->GetGPUVirtualAddress(), 8 };
        bDesc.indirectArgArray        = { bArgBuf->GetGPUVirtualAddress(), sizeof(amd_mg::NvClasToBlasArgs) };
        bDesc.indirectArgCount        = o.gpuCount ? countBuf->GetGPUVirtualAddress() + 8 : 0;
        ep.pDesc = &bDesc;
        s3 = fnExec(cl.Get(), &ep);
        cl->ResourceBarrier(1, &uav);
        rbB = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, bBufBytes, D3D12_RESOURCE_STATE_COPY_DEST, false);
        D3D12_RESOURCE_BARRIER bb{};
        bb.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        bb.Transition.pResource   = bBuf.Get();
        bb.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        bb.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        bb.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &bb);
        cl->CopyBufferRegion(rbB.Get(), 0, bBuf.Get(), 0, bBufBytes);
    }
    // Read everything back for the guard check.
    auto rbT = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, tStride * o.templates, D3D12_RESOURCE_STATE_COPY_DEST, false);
    auto rbC = MakeBuffer(dev.Get(), D3D12_HEAP_TYPE_READBACK, cStride * o.instances, D3D12_RESOURCE_STATE_COPY_DEST, false);
    {
        D3D12_RESOURCE_BARRIER b[2]{};
        for (int k = 0; k < 2; ++k) {
            b[k].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b[k].Transition.pResource   = k == 0 ? tBuf.Get() : cBuf.Get();
            b[k].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b[k].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            b[k].Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
        }
        cl->ResourceBarrier(2, b);
    }
    cl->CopyBufferRegion(rbT.Get(), 0, tBuf.Get(), 0, tStride * o.templates);
    cl->CopyBufferRegion(rbC.Get(), 0, cBuf.Get(), 0, cStride * o.instances);
    cl->Close();
    std::printf("exec template=%d instantiate=%d blas=%d\n", s1, s2, s3);

    ID3D12CommandList* lists[] = { cl.Get() };
    std::printf("[cp] execute\n");
    queue->ExecuteCommandLists(1, lists);
    std::printf("[cp] executed\n");
    ComPtr<ID3D12Fence> fence;
    const HRESULT hrFence = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    std::printf("[cp] CreateFence hr=0x%08X removed=0x%08X\n", static_cast<unsigned>(hrFence), static_cast<unsigned>(dev->GetDeviceRemovedReason()));
    if (FAILED(hrFence)) { return 3; }
    queue->Signal(fence.Get(), 1);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, ev);
    const DWORD w = WaitForSingleObject(ev, 10000);
    std::printf("[cp] waited\n");
    const HRESULT removed = dev->GetDeviceRemovedReason();
    std::printf("RESULT: %s (wait=%s, removed=0x%08X)\n",
                (w == WAIT_OBJECT_0 && SUCCEEDED(removed)) ? "OK" : "DEVICE LOST",
                w == WAIT_OBJECT_0 ? "signaled" : "timeout", static_cast<unsigned>(removed));
    if (w == WAIT_OBJECT_0 && SUCCEEDED(removed)) {
        auto check = [](ID3D12Resource* rb, uint64_t size, uint64_t stride, uint32_t count, const char* what, uint64_t base = 0) {
            void* mapped = nullptr;
            D3D12_RANGE r{ static_cast<SIZE_T>(base), static_cast<SIZE_T>(base + stride * count) };
            rb->Map(0, &r, &mapped);
            const uint8_t* p = static_cast<const uint8_t*>(mapped) + base;
            uint32_t overflowed = 0, maxOver = 0, usedMax = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const uint8_t* slot = p + i * stride;
                uint32_t lastWritten = 0;
                for (uint64_t b = 0; b < stride; ++b) {
                    if (slot[b] != 0xCD) lastWritten = static_cast<uint32_t>(b + 1);
                }
                if (lastWritten > usedMax) usedMax = lastWritten;
                if (lastWritten > size) {
                    ++overflowed;
                    if (lastWritten - size > maxOver) maxOver = static_cast<uint32_t>(lastWritten - size);
                }
            }
            rb->Unmap(0, nullptr);
            std::printf("%s: reported size=%llu, max bytes actually written=%u, slots overflowing=%u/%u (max +%u bytes)\n",
                        what, static_cast<unsigned long long>(size), usedMax, overflowed, count, maxOver);
        };
        std::printf("[cp] check\n");
        check(rbT.Get(), tSize, tStride, o.templates, "templates");
        check(rbC.Get(), cSize, cStride, o.instances, "CLAS     ");
        if (blasCount) check(rbB.Get(), bSize, bStride, blasBig ? blasCount - 1 : blasCount, "BLAS     ");
        if (blasBig) check(rbB.Get(), bBigSize, bBigStride, 1, "BLAS big ", bBigOff);
    }
    Sleep(500); // let the wrapper's DRED callback finish writing
    return (w == WAIT_OBJECT_0 && SUCCEEDED(removed)) ? 0 : 1;
}
