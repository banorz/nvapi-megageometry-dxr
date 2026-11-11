// repro_rtpso.exe <library.dxil> [--no-cluster-flag]
//
// Compiles a real RTXMG raytracing DXIL library (native DXR, original NvAPI, or wrapper-patched)
// into a full RTPSO with the driver, outside the game/demo. Used to reproduce driver
// (amdxc64.dll) crashes inside CreateStateObject. Access violations are caught with SEH so
// several blobs can be compared from a script.
//
// Must run from build\ (Agility SDK 1.722 is loaded from .\D3D12\).

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <fstream>
#include <vector>

using Microsoft::WRL::ComPtr;

extern "C" {
__declspec(dllexport) extern const UINT D3D12SDKVersion = 722;
__declspec(dllexport) extern const char* D3D12SDKPath = ".\\D3D12\\";
}

namespace {

DWORD g_exceptionCode = 0;

int SehFilter(DWORD code) {
    g_exceptionCode = code;
    return EXCEPTION_EXECUTE_HANDLER;
}

HRESULT GuardedCreateStateObject(ID3D12Device5* dev, const D3D12_STATE_OBJECT_DESC* desc, ID3D12StateObject** out) {
    __try {
        return dev->CreateStateObject(desc, IID_PPV_ARGS(out));
    } __except (SehFilter(GetExceptionCode())) {
        return static_cast<HRESULT>(0xDEAD0000u);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: repro_rtpso <library.dxil> [--no-cluster-flag] [--only=Export1,Export2,...]\n");
        return 2;
    }
    bool clusterFlag = true;
    std::vector<std::wstring> only;
    for (int a = 2; a < argc; ++a) {
        if (std::strcmp(argv[a], "--no-cluster-flag") == 0) {
            clusterFlag = false;
        } else if (std::strncmp(argv[a], "--only=", 7) == 0) {
            std::string list = argv[a] + 7;
            size_t pos = 0;
            while (pos <= list.size()) {
                size_t comma = list.find(',', pos);
                std::string name = list.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
                if (!name.empty()) {
                    only.emplace_back(name.begin(), name.end());
                }
                if (comma == std::string::npos) {
                    break;
                }
                pos = comma + 1;
            }
        }
    }

    std::ifstream ifs(argv[1], std::ios::binary | std::ios::ate);
    if (!ifs) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<char> blob(static_cast<size_t>(ifs.tellg()));
    ifs.seekg(0);
    ifs.read(blob.data(), blob.size());

    UUID features[2] = { D3D12RaytracingPreview, D3D12ExperimentalShaderModels };
    HRESULT hr = D3D12EnableExperimentalFeatures(2, features, nullptr, nullptr);
    std::printf("EnableExperimentalFeatures: 0x%08X\n", static_cast<unsigned>(hr));

    ComPtr<ID3D12Device5> device;
    hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        std::printf("D3D12CreateDevice failed: 0x%08X\n", static_cast<unsigned>(hr));
        return 2;
    }

    // Global root signature covering every binding used by the RTXMG path-tracing library
    // (space0: b0-b7, t0-t31, u0-u31, u1000 for the NvAPI fake UAV, s0-s7) plus bindless heaps.
    D3D12_DESCRIPTOR_RANGE ranges[4]{};
    ranges[0] = { D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 8, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    ranges[1] = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 32, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    ranges[2] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 32, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    ranges[3] = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 1000, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND };
    D3D12_DESCRIPTOR_RANGE samplerRange{ D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, 8, 0, 0, 0 };

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = { 4, ranges };
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable = { 1, &samplerRange };

    D3D12_ROOT_SIGNATURE_DESC rsDesc{};
    rsDesc.NumParameters = 2;
    rsDesc.pParameters = params;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED |
                   D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
    ComPtr<ID3DBlob> rsBlob, rsErr;
    hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &rsErr);
    if (FAILED(hr)) {
        std::printf("SerializeRootSignature failed: %s\n", rsErr ? static_cast<char*>(rsErr->GetBufferPointer()) : "?");
        return 2;
    }
    ComPtr<ID3D12RootSignature> rootSig;
    device->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(), IID_PPV_ARGS(&rootSig));

    auto included = [&](const wchar_t* name) {
        if (only.empty()) {
            return true;
        }
        for (const auto& n : only) {
            if (n == name) {
                return true;
            }
        }
        return false;
    };

    D3D12_DXIL_LIBRARY_DESC lib{};
    lib.DXILLibrary = { blob.data(), blob.size() };
    std::vector<D3D12_EXPORT_DESC> exports;
    for (const auto& n : only) {
        exports.push_back({ n.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE });
    }
    lib.NumExports = static_cast<UINT>(exports.size());
    lib.pExports = exports.empty() ? nullptr : exports.data();

    struct HitGroupSpec { const wchar_t* name; const wchar_t* closestHit; const wchar_t* anyHit; };
    const HitGroupSpec specs[] = {
        { L"HG_Main", L"ClosestHit", nullptr },
        { L"HG_ClusterLod", L"ClusterLodClosestHit", L"ClusterLodAnyHit" },
        { L"HG_ClusterLodShadow", L"ClusterLodShadowClosestHit", L"ClusterLodShadowAnyHit" },
    };
    std::vector<D3D12_HIT_GROUP_DESC> hitGroups;
    for (const auto& s : specs) {
        D3D12_HIT_GROUP_DESC hg{};
        hg.HitGroupExport = s.name;
        hg.Type = D3D12_HIT_GROUP_TYPE_TRIANGLES;
        hg.ClosestHitShaderImport = included(s.closestHit) ? s.closestHit : nullptr;
        hg.AnyHitShaderImport = (s.anyHit && included(s.anyHit)) ? s.anyHit : nullptr;
        if (hg.ClosestHitShaderImport || hg.AnyHitShaderImport) {
            hitGroups.push_back(hg);
        }
    }

    D3D12_RAYTRACING_SHADER_CONFIG shaderCfg{ 128, 8 };
    D3D12_RAYTRACING_PIPELINE_CONFIG1 pipeCfg{ 2, clusterFlag ? D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_CLUSTERED_GEOMETRY
                                                              : D3D12_RAYTRACING_PIPELINE_FLAG_NONE };
    D3D12_GLOBAL_ROOT_SIGNATURE globalRs{ rootSig.Get() };

    std::vector<D3D12_STATE_SUBOBJECT> subs = { { D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib } };
    for (const auto& hg : hitGroups) {
        subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &hg });
    }
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderCfg });
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1, &pipeCfg });
    subs.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &globalRs });
    D3D12_STATE_OBJECT_DESC soDesc{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, static_cast<UINT>(subs.size()), subs.data() };

    ComPtr<ID3D12StateObject> so;
    hr = GuardedCreateStateObject(device.Get(), &soDesc, &so);
    if (static_cast<unsigned>(hr) == 0xDEAD0000u) {
        std::printf("RESULT %s: CRASH exception 0x%08X inside CreateStateObject\n", argv[1], static_cast<unsigned>(g_exceptionCode));
        return 3;
    }
    std::printf("RESULT %s: CreateStateObject hr=0x%08X\n", argv[1], static_cast<unsigned>(hr));
    return SUCCEEDED(hr) ? 0 : 1;
}
