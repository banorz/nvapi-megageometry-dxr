#include "amd_mg.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace amd_mg {

namespace {

bool  g_logInitialized = false;
bool  g_logEnabled     = false;
FILE* g_logFile        = nullptr;
std::mutex g_logMutex;

struct ExtnSlotInfo {
    bool     active   = false;
    uint32_t uavSlot  = 0;
    uint32_t uavSpace = 0;
};

std::mutex g_extnMutex;
std::unordered_map<ID3D12Device*, ExtnSlotInfo> g_deviceSlots;
thread_local ExtnSlotInfo g_tlsSlot;

using PFN_CreateStateObject = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device5* pThis,
    const D3D12_STATE_OBJECT_DESC* pDesc,
    REFIID riid,
    void** ppStateObject);

using PFN_CreateComputePipelineState = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device* pThis,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    void** ppPipelineState);

using PFN_CheckFeatureSupport = HRESULT(STDMETHODCALLTYPE*)(
    ID3D12Device* pThis,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize);

PFN_CreateStateObject          g_origCreateStateObject          = nullptr;
PFN_CreateComputePipelineState g_origCreateComputePipelineState = nullptr;
PFN_CheckFeatureSupport        g_origCheckFeatureSupport        = nullptr;
std::unordered_map<void**, PFN_CreateStateObject>   g_origCreateStateObjectByVtable;
std::unordered_map<void**, PFN_CheckFeatureSupport> g_origCheckFeatureSupportByVtable;
std::mutex g_hookMutex;
HMODULE    g_selfModule = nullptr;

PFN_CreateStateObject GetOrigCreateStateObject(ID3D12Device5* pThis) {
    if (pThis) {
        void** vtable = *reinterpret_cast<void***>(pThis);
        std::lock_guard<std::mutex> lock(g_hookMutex);
        auto it = g_origCreateStateObjectByVtable.find(vtable);
        if (it != g_origCreateStateObjectByVtable.end() && it->second) {
            return it->second;
        }
    }
    return g_origCreateStateObject;
}

PFN_CheckFeatureSupport GetOrigCheckFeatureSupport(ID3D12Device* pThis) {
    if (pThis) {
        void** vtable = *reinterpret_cast<void***>(pThis);
        std::lock_guard<std::mutex> lock(g_hookMutex);
        auto it = g_origCheckFeatureSupportByVtable.find(vtable);
        if (it != g_origCheckFeatureSupportByVtable.end() && it->second) {
            return it->second;
        }
    }
    return g_origCheckFeatureSupport;
}

HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
    ID3D12Device* pThis,
    const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    void** ppPipelineState)
{
    if (!pDesc || !pDesc->CS.pShaderBytecode || pDesc->CS.BytecodeLength == 0) {
        return g_origCreateComputePipelineState(pThis, pDesc, riid, ppPipelineState);
    }

    uint32_t uavSlot  = 0;
    uint32_t uavSpace = 0;
    if (GetActiveShaderExtnSlot(pThis, uavSlot, uavSpace)) {
        std::vector<uint8_t> patched;
        if (PatchDxilClusterShader(
                pDesc->CS.pShaderBytecode,
                pDesc->CS.BytecodeLength,
                uavSlot,
                uavSpace,
                patched)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC modifiedDesc = *pDesc;
            modifiedDesc.CS.pShaderBytecode = patched.data();
            modifiedDesc.CS.BytecodeLength  = patched.size();
            HRESULT hr = g_origCreateComputePipelineState(pThis, &modifiedDesc, riid, ppPipelineState);
            Log("[D3D12] CreateComputePipelineState patched DXIL (slot=u%u, space=%u) -> hr=0x%08X",
                uavSlot, uavSpace, hr);
            if (SUCCEEDED(hr)) {
                return hr;
            }
        }
    }
    return g_origCreateComputePipelineState(pThis, pDesc, riid, ppPipelineState);
}

HRESULT STDMETHODCALLTYPE Hooked_CheckFeatureSupport(
    ID3D12Device* pThis,
    D3D12_FEATURE Feature,
    void* pFeatureSupportData,
    UINT FeatureSupportDataSize)
{
    PFN_CheckFeatureSupport origFn = GetOrigCheckFeatureSupport(pThis);
    HRESULT hr = origFn(pThis, Feature, pFeatureSupportData, FeatureSupportDataSize);
    if (Feature == D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2 &&
        FeatureSupportDataSize == sizeof(D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2) &&
        pFeatureSupportData) {
        auto* rt2 = static_cast<D3D12_FEATURE_DATA_D3D12_OPTIONS_RAYTRACING2*>(pFeatureSupportData);
        ComPtr<ID3D12DeviceRaytracing2> pDevRt2;
        bool hasRt2 = SUCCEEDED(pThis->QueryInterface(IID_PPV_ARGS(&pDevRt2))) && (pDevRt2 != nullptr);
        Log("[D3D12] CheckFeatureSupport(OPTIONS_RAYTRACING2): orig hr=0x%08X, Clusters=%d, hasRt2=%d",
            hr, rt2->ClustersAndPTLASSupported, hasRt2 ? 1 : 0);
        const char* forceDxr = std::getenv("AMD_MG_FORCE_DXR_CAPS");
        if (IsMegaGeometryDisabled()) {
            // A/B without Mega Geometry: engines with a native DXR 1.722 cluster path fall back too.
            rt2->ClustersAndPTLASSupported = FALSE;
        } else if (hasRt2 && forceDxr && std::atoi(forceDxr) != 0) {
            rt2->ClustersAndPTLASSupported = TRUE;
            hr = S_OK;
        }
    } else if (Feature == D3D12_FEATURE_SHADER_MODEL &&
               FeatureSupportDataSize == sizeof(D3D12_FEATURE_DATA_SHADER_MODEL) &&
               pFeatureSupportData) {
        auto* sm = static_cast<D3D12_FEATURE_DATA_SHADER_MODEL*>(pFeatureSupportData);
        if (FAILED(hr) || sm->HighestShaderModel < D3D_SHADER_MODEL_6_10) {
            sm->HighestShaderModel = D3D_SHADER_MODEL_6_10;
            hr = S_OK;
        }
    }
    return hr;
}

// Intercepted ID3D12Device5::CreateStateObject
HRESULT STDMETHODCALLTYPE Hooked_CreateStateObject(
    ID3D12Device5* pThis,
    const D3D12_STATE_OBJECT_DESC* pDesc,
    REFIID riid,
    void** ppStateObject)
{
    PFN_CreateStateObject origFn = GetOrigCreateStateObject(pThis);
    if (!pDesc || !pDesc->pSubobjects || pDesc->NumSubobjects == 0) {
        return origFn(pThis, pDesc, riid, ppStateObject);
    }

    uint32_t uavSlot  = 0;
    uint32_t uavSpace = 0;
    bool hasExtnSlot  = GetActiveShaderExtnSlot(pThis, uavSlot, uavSpace);

    // Clone subobject array so we can patch DXIL libraries and inject ALLOW_CLUSTERED_GEOMETRY
    std::vector<D3D12_STATE_SUBOBJECT> clonedSubobjects(
        pDesc->pSubobjects, pDesc->pSubobjects + pDesc->NumSubobjects);
    std::vector<D3D12_DXIL_LIBRARY_DESC> patchedLibDescs;
    std::vector<std::vector<uint8_t>>    patchedBlobs;
    std::vector<D3D12_RAYTRACING_PIPELINE_CONFIG1> promotedConfigs;
    std::vector<D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION> rebasedAssocs;

    patchedLibDescs.reserve(pDesc->NumSubobjects);
    patchedBlobs.reserve(pDesc->NumSubobjects);
    promotedConfigs.reserve(pDesc->NumSubobjects);
    rebasedAssocs.reserve(pDesc->NumSubobjects);

    bool anyPatched = false;

    for (uint32_t i = 0; i < pDesc->NumSubobjects; ++i) {
        auto& sub = clonedSubobjects[i];

        if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY && sub.pDesc) {
            const auto* origLib = static_cast<const D3D12_DXIL_LIBRARY_DESC*>(sub.pDesc);
            std::vector<uint8_t> patched;
            if (PatchDxilClusterShader(
                    origLib->DXILLibrary.pShaderBytecode,
                    origLib->DXILLibrary.BytecodeLength,
                    hasExtnSlot ? uavSlot : 63,
                    hasExtnSlot ? uavSpace : 0,
                    patched)) {
                patchedBlobs.push_back(std::move(patched));
                D3D12_DXIL_LIBRARY_DESC newLib = *origLib;
                newLib.DXILLibrary.pShaderBytecode = patchedBlobs.back().data();
                newLib.DXILLibrary.BytecodeLength  = patchedBlobs.back().size();
                patchedLibDescs.push_back(newLib);
                sub.pDesc  = &patchedLibDescs.back();
                anyPatched = true;
            }
        } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1 && sub.pDesc) {
            const auto* origCfg1 = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG1*>(sub.pDesc);
            D3D12_RAYTRACING_PIPELINE_CONFIG1 newCfg1 = *origCfg1;
            newCfg1.Flags |= D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_CLUSTERED_GEOMETRY;
            promotedConfigs.push_back(newCfg1);
            sub.pDesc  = &promotedConfigs.back();
            anyPatched = true;
        } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG && sub.pDesc) {
            const auto* origCfg = static_cast<const D3D12_RAYTRACING_PIPELINE_CONFIG*>(sub.pDesc);
            D3D12_RAYTRACING_PIPELINE_CONFIG1 newCfg1{};
            newCfg1.MaxTraceRecursionDepth = origCfg->MaxTraceRecursionDepth;
            newCfg1.Flags                  = D3D12_RAYTRACING_PIPELINE_FLAG_ALLOW_CLUSTERED_GEOMETRY;
            promotedConfigs.push_back(newCfg1);
            sub.Type   = D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG1;
            sub.pDesc  = &promotedConfigs.back();
            anyPatched = true;
        } else if (sub.Type == D3D12_STATE_SUBOBJECT_TYPE_SUBOBJECT_TO_EXPORTS_ASSOCIATION && sub.pDesc) {
            const auto* origAssoc = static_cast<const D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION*>(sub.pDesc);
            if (origAssoc->pSubobjectToAssociate >= pDesc->pSubobjects &&
                origAssoc->pSubobjectToAssociate < pDesc->pSubobjects + pDesc->NumSubobjects) {
                size_t targetIdx = static_cast<size_t>(origAssoc->pSubobjectToAssociate - pDesc->pSubobjects);
                D3D12_SUBOBJECT_TO_EXPORTS_ASSOCIATION newAssoc = *origAssoc;
                newAssoc.pSubobjectToAssociate = &clonedSubobjects[targetIdx];
                rebasedAssocs.push_back(newAssoc);
                sub.pDesc = &rebasedAssocs.back();
            }
        }
    }

    if (anyPatched) {
        D3D12_STATE_OBJECT_DESC modifiedDesc = *pDesc;
        modifiedDesc.pSubobjects = clonedSubobjects.data();
        HRESULT hr = origFn(pThis, &modifiedDesc, riid, ppStateObject);
        Log("[D3D12] CreateStateObject patched (dev=%p, %zu DXIL blob(s), hasExtn=%d, slot=u%u) -> hr=0x%08X",
            pThis, patchedBlobs.size(), hasExtnSlot ? 1 : 0, uavSlot, hr);
        return hr;
    }

    HRESULT hr = origFn(pThis, pDesc, riid, ppStateObject);
    Log("[D3D12] CreateStateObject unpatched (dev=%p, subs=%u) -> hr=0x%08X",
        pThis, pDesc->NumSubobjects, hr);
    return hr;
}

} // namespace

void LogInit() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logInitialized) {
        return;
    }
    g_logInitialized = true;
    const char* envLog = std::getenv("AMD_MG_LOG");
    if (!envLog || std::atoi(envLog) != 0) {
        g_logEnabled = true;
        char dllPath[MAX_PATH] = {};
        HMODULE selfMod = nullptr;
        GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCSTR>(&LogInit),
            &selfMod);
        if (selfMod && GetModuleFileNameA(selfMod, dllPath, MAX_PATH) > 0) {
            char* lastSlash = std::strrchr(dllPath, '\\');
            if (lastSlash) {
                *(lastSlash + 1) = '\0';
            }
            std::string fullLogPath = std::string(dllPath) + "amd_megageometry.log";
            g_logFile = std::fopen(fullLogPath.c_str(), "a");
        }
        if (!g_logFile) {
            g_logFile = std::fopen("amd_megageometry.log", "a");
        }
    }
}

void Log(const char* fmt, ...) {
    LogInit();
    if (!g_logEnabled) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_logMutex);
    char buf[2048];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (g_logFile) {
        std::fprintf(g_logFile, "%s\n", buf);
        std::fflush(g_logFile);
    }
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
}

bool IsMegaGeometryDisabled() {
    // Win32 process environment (not the CRT copy): every module, and SetEnvironmentVariable
    // after load, see the same value.
    char value[16] = {};
    const DWORD len = GetEnvironmentVariableA("AMD_MG_DISABLE", value, sizeof(value));
    const bool disabled = len > 0 && len < sizeof(value) && std::atoi(value) != 0;
    static std::atomic<bool> s_logged{ false };
    if (disabled && !s_logged.exchange(true)) {
        Log("[DLL] AMD_MG_DISABLE=1: reporting Cluster Operations / Partitioned TLAS as unsupported (A/B without Mega Geometry)");
    }
    return disabled;
}

void TryEnableD3D12PreviewFeatures() {
    static std::mutex s_previewMutex;
    std::lock_guard<std::mutex> lock(s_previewMutex);

    static bool s_succeeded = false;
    if (s_succeeded) {
        return;
    }

    DWORD devMode = 0;
    DWORD dataSize = sizeof(devMode);
    RegGetValueA(
        HKEY_LOCAL_MACHINE,
        "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AppModelUnlock",
        "AllowDevelopmentWithoutDevLicense",
        RRF_RT_REG_DWORD,
        nullptr,
        &devMode,
        &dataSize);

    if (devMode != 0) {
        auto* exportedVer = reinterpret_cast<UINT*>(
            GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion"));
        if (exportedVer && *exportedVer < 722) {
            DWORD oldProt = 0;
            if (VirtualProtect(exportedVer, sizeof(UINT), PAGE_READWRITE, &oldProt)) {
                UINT prevVer = *exportedVer;
                *exportedVer = 722;
                VirtualProtect(exportedVer, sizeof(UINT), oldProt, &oldProt);
                Log("[D3D12] Patched executable D3D12SDKVersion export (%u -> 722)", prevVer);
            }
        }

        ComPtr<ID3D12SDKConfiguration> sdkConfig;
        if (SUCCEEDED(D3D12GetInterface(CLSID_D3D12SDKConfiguration, IID_PPV_ARGS(&sdkConfig))) && sdkConfig) {
            HRESULT hrSdk = sdkConfig->SetSDKVersion(722, ".\\D3D12\\");
            if (FAILED(hrSdk)) {
                hrSdk = sdkConfig->SetSDKVersion(722, ".\\");
            }
            Log("[D3D12] ID3D12SDKConfiguration::SetSDKVersion(722) -> 0x%08X", hrSdk);
        }
    }

    // Try enabling both or individually
    UUID featuresBoth[2] = { D3D12RaytracingPreview, D3D12ExperimentalShaderModels };
    HRESULT hr = D3D12EnableExperimentalFeatures(2, featuresBoth, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        s_succeeded = true;
        Log("[D3D12] Enabled D3D12RaytracingPreview + D3D12ExperimentalShaderModels (hr=0x%08X)", hr);
    } else {
        UUID featureSm[1] = { D3D12ExperimentalShaderModels };
        HRESULT hrSm = D3D12EnableExperimentalFeatures(1, featureSm, nullptr, nullptr);
        Log("[D3D12] D3D12EnableExperimentalFeatures(both=0x%08X, SM only=0x%08X)", hr, hrSm);
        if (SUCCEEDED(hrSm)) {
            s_succeeded = true;
        }
    }
}

void HookDeviceForStateObjects(ID3D12Device* pDevice) {
    if (!pDevice) {
        return;
    }

    ComPtr<ID3D12Device5> pDev5;
    if (FAILED(pDevice->QueryInterface(IID_PPV_ARGS(&pDev5))) || !pDev5) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);

    void** vtable = *reinterpret_cast<void***>(pDev5.Get());
    constexpr size_t kCheckFeatureSupportSlot = 13;
    constexpr size_t kCreateStateObjectSlot   = 62;

    DWORD oldProtect = 0;
    if (vtable[kCheckFeatureSupportSlot] != reinterpret_cast<void*>(&Hooked_CheckFeatureSupport) &&
        g_origCheckFeatureSupportByVtable.find(vtable) == g_origCheckFeatureSupportByVtable.end()) {
        if (VirtualProtect(&vtable[kCheckFeatureSupportSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            auto origFn = reinterpret_cast<PFN_CheckFeatureSupport>(vtable[kCheckFeatureSupportSlot]);
            g_origCheckFeatureSupportByVtable[vtable] = origFn;
            if (!g_origCheckFeatureSupport) {
                g_origCheckFeatureSupport = origFn;
            }
            vtable[kCheckFeatureSupportSlot] = reinterpret_cast<void*>(&Hooked_CheckFeatureSupport);
            VirtualProtect(&vtable[kCheckFeatureSupportSlot], sizeof(void*), oldProtect, &oldProtect);
            Log("[D3D12] Hooked ID3D12Device::CheckFeatureSupport (vtable=%p[%zu] = %p)",
                vtable, kCheckFeatureSupportSlot, origFn);
        }
    }

    if (vtable[kCreateStateObjectSlot] != reinterpret_cast<void*>(&Hooked_CreateStateObject) &&
        g_origCreateStateObjectByVtable.find(vtable) == g_origCreateStateObjectByVtable.end()) {
        if (VirtualProtect(&vtable[kCreateStateObjectSlot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProtect)) {
            auto origFn = reinterpret_cast<PFN_CreateStateObject>(vtable[kCreateStateObjectSlot]);
            g_origCreateStateObjectByVtable[vtable] = origFn;
            if (!g_origCreateStateObject) {
                g_origCreateStateObject = origFn;
            }
            vtable[kCreateStateObjectSlot] = reinterpret_cast<void*>(&Hooked_CreateStateObject);
            VirtualProtect(&vtable[kCreateStateObjectSlot], sizeof(void*), oldProtect, &oldProtect);
            Log("[D3D12] Hooked ID3D12Device5::CreateStateObject (vtable=%p[%zu] = %p)",
                vtable, kCreateStateObjectSlot, origFn);
        }
    }
}

namespace {

bool EndsWithInsensitiveA(const char* str, const char* suffix) {
    if (!str || !suffix) return false;
    size_t lenStr = std::strlen(str);
    size_t lenSuf = std::strlen(suffix);
    if (lenStr < lenSuf) return false;
    return _stricmp(str + lenStr - lenSuf, suffix) == 0;
}

bool EndsWithInsensitiveW(const wchar_t* str, const wchar_t* suffix) {
    if (!str || !suffix) return false;
    size_t lenStr = std::wcslen(str);
    size_t lenSuf = std::wcslen(suffix);
    if (lenStr < lenSuf) return false;
    return _wcsicmp(str + lenStr - lenSuf, suffix) == 0;
}

void PatchModuleIat(HMODULE targetMod);
void PatchModuleEatForNvapi(HMODULE targetMod);

PFN_nvapi_QueryInterface g_chainedQuery = nullptr;
std::once_flag           g_chainInitFlag;

FARPROC GetSelfNvapiQueryInterface() {
    return g_selfModule ? GetProcAddress(g_selfModule, "nvapi_QueryInterface") : nullptr;
}

FARPROC WINAPI Hooked_GetProcAddress(HMODULE hModule, LPCSTR lpProcName) {
    FARPROC orig   = GetProcAddress(hModule, lpProcName);
    if (reinterpret_cast<uintptr_t>(lpProcName) > 0xFFFF) {
        if (std::strcmp(lpProcName, "D3D12CreateDevice") == 0) {
            TryEnableD3D12PreviewFeatures();
        }
        FARPROC selfFn = GetSelfNvapiQueryInterface();
        if (selfFn && std::strcmp(lpProcName, "nvapi_QueryInterface") == 0) {
            if (orig && orig != selfFn) {
                SetChainedQueryInterface(reinterpret_cast<PFN_nvapi_QueryInterface>(orig));
                Log("[IAT] GetProcAddress(mod=%p, \"nvapi_QueryInterface\") -> chained orig=%p", hModule, orig);
            }
            return selfFn;
        }
    }
    return orig;
}

void PatchModuleEatForNvapi(HMODULE targetMod) {
    if (!targetMod || targetMod == g_selfModule || g_chainedQuery != nullptr) return;
    FARPROC selfFn = GetSelfNvapiQueryInterface();
    if (!selfFn) return;

    auto* base = reinterpret_cast<uint8_t*>(targetMod);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const auto& expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (expDir.VirtualAddress == 0 || expDir.Size == 0) return;

    auto* exp       = reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base + expDir.VirtualAddress);
    auto* names     = reinterpret_cast<DWORD*>(base + exp->AddressOfNames);
    auto* ordinals  = reinterpret_cast<WORD*>(base + exp->AddressOfNameOrdinals);
    auto* functions = reinterpret_cast<DWORD*>(base + exp->AddressOfFunctions);

    for (DWORD i = 0; i < exp->NumberOfNames; ++i) {
        const char* name = reinterpret_cast<const char*>(base + names[i]);
        if (std::strcmp(name, "nvapi_QueryInterface") == 0) {
            WORD  ord     = ordinals[i];
            DWORD origRva = functions[ord];
            auto  origFn  = reinterpret_cast<PFN_nvapi_QueryInterface>(base + origRva);
            if (reinterpret_cast<FARPROC>(origFn) == selfFn) {
                return;
            }
            SetChainedQueryInterface(origFn);

            // Allocate a 64-byte stub within <2GB above `base` so a 32-bit RVA in EAT can reach it
            uint8_t* stub = nullptr;
            uintptr_t startAddr = (reinterpret_cast<uintptr_t>(base) + nt->OptionalHeader.SizeOfImage + 0xFFFFull) & ~0xFFFFull;
            for (uintptr_t step = 0; step < 0x70000000ull; step += 0x10000ull) {
                stub = static_cast<uint8_t*>(VirtualAlloc(
                    reinterpret_cast<void*>(startAddr + step),
                    4096,
                    MEM_COMMIT | MEM_RESERVE,
                    PAGE_EXECUTE_READWRITE));
                if (stub) break;
            }
            if (stub) {
                // 48 B8 <8-byte selfFn> FF E0 (mov rax, imm64; jmp rax - 12 bytes, non-RIP-relative)
                stub[0] = 0x48;
                stub[1] = 0xB8;
                uintptr_t targetAddr = reinterpret_cast<uintptr_t>(selfFn);
                std::memcpy(&stub[2], &targetAddr, sizeof(targetAddr));
                stub[10] = 0xFF;
                stub[11] = 0xE0;
                std::memset(&stub[12], 0x90, 20);
                FlushInstructionCache(GetCurrentProcess(), stub, 32);

                DWORD oldProt = 0;
                if (VirtualProtect(&functions[ord], sizeof(DWORD), PAGE_READWRITE, &oldProt)) {
                    functions[ord] = static_cast<DWORD>(stub - base);
                    VirtualProtect(&functions[ord], sizeof(DWORD), oldProt, &oldProt);
                    Log("[EAT] Patched nvapi_QueryInterface export on module %p (chained orig=%p -> stub=%p)",
                        targetMod, origFn, stub);
                }
            }
            return;
        }
    }
}

HMODULE WINAPI Hooked_LoadLibraryA(LPCSTR lpLibFileName) {
    if (EndsWithInsensitiveA(lpLibFileName, "nvapi64.dll") && g_selfModule) {
        Log("[IAT] Redirected LoadLibraryA(\"%s\") -> self (%p)", lpLibFileName, g_selfModule);
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&Hooked_LoadLibraryA), &pinned);
        return g_selfModule;
    }
    HMODULE mod = LoadLibraryA(lpLibFileName);
    if (mod) {
        if (EndsWithInsensitiveA(lpLibFileName, "sl.common.dll") || EndsWithInsensitiveA(lpLibFileName, "sl.interposer.dll")) {
            PatchModuleIat(mod);
        }
        if (EndsWithInsensitiveA(lpLibFileName, "dxgi.dll") || EndsWithInsensitiveA(lpLibFileName, "fakenvapi.dll")) {
            PatchModuleIat(mod);
            PatchModuleEatForNvapi(mod);
        }
    }
    return mod;
}

HMODULE WINAPI Hooked_LoadLibraryW(LPCWSTR lpLibFileName) {
    if (EndsWithInsensitiveW(lpLibFileName, L"nvapi64.dll") && g_selfModule) {
        Log("[IAT] Redirected LoadLibraryW(\"%ls\") -> self (%p)", lpLibFileName, g_selfModule);
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&Hooked_LoadLibraryW), &pinned);
        return g_selfModule;
    }
    HMODULE mod = LoadLibraryW(lpLibFileName);
    if (mod) {
        if (EndsWithInsensitiveW(lpLibFileName, L"sl.common.dll") || EndsWithInsensitiveW(lpLibFileName, L"sl.interposer.dll")) {
            PatchModuleIat(mod);
        }
        if (EndsWithInsensitiveW(lpLibFileName, L"dxgi.dll") || EndsWithInsensitiveW(lpLibFileName, L"fakenvapi.dll")) {
            PatchModuleIat(mod);
            PatchModuleEatForNvapi(mod);
        }
    }
    return mod;
}

HMODULE WINAPI Hooked_LoadLibraryExA(LPCSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (EndsWithInsensitiveA(lpLibFileName, "nvapi64.dll") && g_selfModule) {
        Log("[IAT] Redirected LoadLibraryExA(\"%s\", flags=0x%X) -> self (%p)", lpLibFileName, dwFlags, g_selfModule);
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&Hooked_LoadLibraryExA), &pinned);
        return g_selfModule;
    }
    HMODULE mod = LoadLibraryExA(lpLibFileName, hFile, dwFlags);
    if (mod) {
        if (EndsWithInsensitiveA(lpLibFileName, "sl.common.dll") || EndsWithInsensitiveA(lpLibFileName, "sl.interposer.dll")) {
            PatchModuleIat(mod);
        }
        if (EndsWithInsensitiveA(lpLibFileName, "dxgi.dll") || EndsWithInsensitiveA(lpLibFileName, "fakenvapi.dll")) {
            PatchModuleIat(mod);
            PatchModuleEatForNvapi(mod);
        }
    }
    return mod;
}

HMODULE WINAPI Hooked_LoadLibraryExW(LPCWSTR lpLibFileName, HANDLE hFile, DWORD dwFlags) {
    if (EndsWithInsensitiveW(lpLibFileName, L"nvapi64.dll") && g_selfModule) {
        Log("[IAT] Redirected LoadLibraryExW(\"%ls\", flags=0x%X) -> self (%p)", lpLibFileName, dwFlags, g_selfModule);
        HMODULE pinned = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(&Hooked_LoadLibraryExW), &pinned);
        return g_selfModule;
    }
    HMODULE mod = LoadLibraryExW(lpLibFileName, hFile, dwFlags);
    if (mod) {
        if (EndsWithInsensitiveW(lpLibFileName, L"sl.common.dll") || EndsWithInsensitiveW(lpLibFileName, L"sl.interposer.dll")) {
            PatchModuleIat(mod);
        }
        if (EndsWithInsensitiveW(lpLibFileName, L"dxgi.dll") || EndsWithInsensitiveW(lpLibFileName, L"fakenvapi.dll")) {
            PatchModuleIat(mod);
            PatchModuleEatForNvapi(mod);
        }
    }
    return mod;
}

using PFN_D3D12CreateDevice = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
PFN_D3D12CreateDevice g_origD3D12CreateDevice = nullptr;

HRESULT WINAPI Hooked_D3D12CreateDevice(
    IUnknown* pAdapter,
    D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid,
    void** ppDevice)
{
    TryEnableD3D12PreviewFeatures();
    EnableDredBeforeDeviceCreation();
    PFN_D3D12CreateDevice fn = g_origD3D12CreateDevice ? g_origD3D12CreateDevice : &D3D12CreateDevice;
    HRESULT hr = fn(pAdapter, MinimumFeatureLevel, riid, ppDevice);
    Log("[IAT] Hooked_D3D12CreateDevice(level=0x%X) -> hr=0x%08X", MinimumFeatureLevel, hr);
    if (SUCCEEDED(hr) && ppDevice && *ppDevice) {
        ComPtr<ID3D12Device> pDev;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppDevice)->QueryInterface(IID_PPV_ARGS(&pDev))) && pDev) {
            HookDeviceForStateObjects(pDev.Get());
            HookDeviceCommandListsForStateShadow(pDev.Get());
            WatchDeviceRemoval(pDev.Get());
        }
    }
    return hr;
}

void PatchModuleIat(HMODULE targetMod) {
    if (!targetMod || targetMod == g_selfModule) return;
    auto* base = reinterpret_cast<uint8_t*>(targetMod);
    auto* dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;

    const auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0 || impDir.Size == 0) return;

    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
    for (; desc->Name != 0; ++desc) {
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto* origThunk = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->OriginalFirstThunk);
        auto* iatThunk  = reinterpret_cast<IMAGE_THUNK_DATA*>(base + desc->FirstThunk);

        for (; origThunk->u1.AddressOfData != 0; ++origThunk, ++iatThunk) {
            if (IMAGE_SNAP_BY_ORDINAL(origThunk->u1.Ordinal)) continue;
            auto* byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + origThunk->u1.AddressOfData);
            const char* funcName = reinterpret_cast<const char*>(byName->Name);

            void* replacement = nullptr;
            if (std::strcmp(funcName, "LoadLibraryA") == 0) {
                replacement = reinterpret_cast<void*>(&Hooked_LoadLibraryA);
            } else if (std::strcmp(funcName, "LoadLibraryW") == 0) {
                replacement = reinterpret_cast<void*>(&Hooked_LoadLibraryW);
            } else if (std::strcmp(funcName, "LoadLibraryExA") == 0) {
                replacement = reinterpret_cast<void*>(&Hooked_LoadLibraryExA);
            } else if (std::strcmp(funcName, "LoadLibraryExW") == 0) {
                replacement = reinterpret_cast<void*>(&Hooked_LoadLibraryExW);
            } else if (std::strcmp(funcName, "GetProcAddress") == 0) {
                replacement = reinterpret_cast<void*>(&Hooked_GetProcAddress);
            } else if (std::strcmp(funcName, "D3D12CreateDevice") == 0 &&
                       targetMod != GetModuleHandleA("sl.interposer.dll")) {
                if (!g_origD3D12CreateDevice &&
                    iatThunk->u1.Function != reinterpret_cast<ULONG_PTR>(&Hooked_D3D12CreateDevice)) {
                    g_origD3D12CreateDevice = reinterpret_cast<PFN_D3D12CreateDevice>(iatThunk->u1.Function);
                }
                replacement = reinterpret_cast<void*>(&Hooked_D3D12CreateDevice);
            }

            if (replacement) {
                DWORD oldProt = 0;
                if (VirtualProtect(&iatThunk->u1.Function, sizeof(void*), PAGE_READWRITE, &oldProt)) {
                    iatThunk->u1.Function = reinterpret_cast<ULONG_PTR>(replacement);
                    VirtualProtect(&iatThunk->u1.Function, sizeof(void*), oldProt, &oldProt);
                }
            }
        }
    }
}

} // namespace

void SetChainedQueryInterface(PFN_nvapi_QueryInterface fn) {
    if (g_chainedQuery != nullptr) {
        return;
    }
    FARPROC selfFn = GetSelfNvapiQueryInterface();
    if (fn && reinterpret_cast<FARPROC>(fn) != selfFn) {
        g_chainedQuery = fn;
    }
}

PFN_nvapi_QueryInterface GetChainedQueryInterface() {
    if (g_chainedQuery) {
        return g_chainedQuery;
    }
    std::call_once(g_chainInitFlag, []() {
        if (g_chainedQuery) {
            return;
        }
        FARPROC selfFn = GetSelfNvapiQueryInterface();
        const char* candidates[] = { "nvapi64_orig.dll", "fakenvapi.dll" };
        for (const char* name : candidates) {
            HMODULE mod = GetModuleHandleA(name);
            if (!mod) {
                mod = LoadLibraryA(name);
            }
            if (mod && mod != g_selfModule) {
                auto fn = reinterpret_cast<PFN_nvapi_QueryInterface>(
                    GetProcAddress(mod, "nvapi_QueryInterface"));
                if (fn && reinterpret_cast<FARPROC>(fn) != selfFn) {
                    g_chainedQuery = fn;
                    Log("[NvAPI] Chained secondary NvAPI module: %s (%p)", name, mod);
                    break;
                }
            }
        }
    });
    return g_chainedQuery;
}

void InstallProcessIatHooks(HMODULE selfModule) {
    g_selfModule = selfModule;

    // Immediately upgrade main executable's exported D3D12SDKVersion to 722 so the very first
    // D3D12CreateDevice (e.g. OptiScaler's GPU capability probe) loads D3D12Core.dll v722.
    if (auto* exportedVer = reinterpret_cast<UINT*>(
            GetProcAddress(GetModuleHandleW(nullptr), "D3D12SDKVersion"))) {
        if (*exportedVer < 722) {
            DWORD oldProt = 0;
            if (VirtualProtect(exportedVer, sizeof(UINT), PAGE_READWRITE, &oldProt)) {
                UINT prevVer = *exportedVer;
                *exportedVer = 722;
                VirtualProtect(exportedVer, sizeof(UINT), oldProt, &oldProt);
                Log("[D3D12] Patched executable D3D12SDKVersion export (%u -> 722)", prevVer);
            }
        }
    }

    PatchModuleIat(GetModuleHandleW(nullptr));
    if (HMODULE slInt = GetModuleHandleA("sl.interposer.dll")) {
        PatchModuleIat(slInt);
    }
    if (HMODULE slCom = GetModuleHandleA("sl.common.dll")) {
        PatchModuleIat(slCom);
    }
    if (HMODULE dxgiMod = GetModuleHandleA("dxgi.dll")) {
        PatchModuleIat(dxgiMod);
        PatchModuleEatForNvapi(dxgiMod);
    }
    if (HMODULE fakeNv = GetModuleHandleA("fakenvapi.dll")) {
        PatchModuleIat(fakeNv);
        PatchModuleEatForNvapi(fakeNv);
    }
    Log("[IAT] Installed IAT/EAT hooks on main executable, Streamline, and OptiScaler modules");
}

void SetDeviceShaderExtnSlot(ID3D12Device* pDevice, uint32_t uavSlot, uint32_t uavSpace, bool isThreadLocal) {
    HookDeviceForStateObjects(pDevice);
    bool active = (uavSlot != ~0u);
    if (isThreadLocal) {
        g_tlsSlot = { active, uavSlot, uavSpace };
    } else {
        std::lock_guard<std::mutex> lock(g_extnMutex);
        g_deviceSlots[pDevice] = { active, uavSlot, uavSpace };
    }
    Log("[NvAPI] SetNvShaderExtnSlotSpace(dev=%p, slot=%u, space=%u, tls=%d)",
        pDevice, uavSlot, uavSpace, isThreadLocal ? 1 : 0);
}

bool GetActiveShaderExtnSlot(ID3D12Device* pDevice, uint32_t& outSlot, uint32_t& outSpace) {
    if (g_tlsSlot.active) {
        outSlot  = g_tlsSlot.uavSlot;
        outSpace = g_tlsSlot.uavSpace;
        return true;
    }
    std::lock_guard<std::mutex> lock(g_extnMutex);
    auto it = g_deviceSlots.find(pDevice);
    if (it != g_deviceSlots.end() && it->second.active) {
        outSlot  = it->second.uavSlot;
        outSpace = it->second.uavSpace;
        return true;
    }
    return false;
}

} // namespace amd_mg
