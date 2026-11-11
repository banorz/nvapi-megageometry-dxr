// ============================================================================
// amd-megageometry: compute-state shadowing for game command lists
//
// The argument converter records a compute dispatch (own root signature + PSO) on the game's
// command list in the middle of its NvAPI cluster call. NvAPI on NVIDIA does not touch the
// bound pipeline/root state, so engines may keep using the state they bound before the call
// (nvrhi re-binds; Northlight/Alan Wake 2 apparently does not: GPU fault right after the first
// cluster batch). We hook the state setters, keep a per-list shadow of the compute root
// signature, root arguments and last pipeline, and replay it after our dispatches.
//
// D3D12 runtime layout (Agility 1.722, verified on amdxc64): an OPEN command list points to a
// per-list vtable allocated on the heap whose entries jump straight into the driver; a CLOSED
// list points to a static vtable inside D3D12Core.dll, and Reset() switches back to the
// per-list one. So we patch (1) the static closed-state Reset, (2) the device's
// CreateCommandList, and from both re-patch the per-list heap vtable of the list.
// ============================================================================
#include "amd_mg.h"

#include <array>
#include <atomic>
#include <cstdlib>
#include <memory>
#include <shared_mutex>

namespace amd_mg {

constexpr uint32_t kMaxRootParams = 64; // root signatures are limited to 64 DWORDs

enum class RootArgKind : uint8_t { None, Table, Constants, Cbv, Srv, Uav };

struct RootArg {
    RootArgKind kind      = RootArgKind::None;
    uint64_t    value     = 0;   // descriptor handle / GPU VA
    uint64_t    constMask = 0;   // which DWORDs of `constants` were set
    std::array<uint32_t, kMaxRootParams> constants{};
};

enum class PipelineKind : uint8_t { None, Pso, StateObject };

struct CmdListComputeState {
    ID3D12RootSignature* computeRootSig = nullptr;
    PipelineKind         pipelineKind   = PipelineKind::None;
    ID3D12PipelineState* pso            = nullptr;
    ID3D12StateObject*   stateObject    = nullptr;
    std::array<RootArg, kMaxRootParams> args{};
    uint32_t             maxArg = 0;    // 1 + highest root parameter index touched

    void ClearRootArgs() {
        for (uint32_t i = 0; i < maxArg; ++i) {
            args[i].kind      = RootArgKind::None;
            args[i].constMask = 0;
        }
        maxArg = 0;
    }
    void Clear() {
        computeRootSig = nullptr;
        pipelineKind   = PipelineKind::None;
        pso            = nullptr;
        stateObject    = nullptr;
        ClearRootArgs();
    }
};

namespace {

using Microsoft::WRL::ComPtr;

// ID3D12GraphicsCommandList vtable layout (IUnknown 0-2, ID3D12Object 3-6, DeviceChild 7,
// CommandList 8, GraphicsCommandList 9-59, ...List1 60-65, List2 66, List3 67, List4 68-76).
constexpr size_t kSlotGetType                   = 8;
constexpr size_t kSlotClose                     = 9;
constexpr size_t kSlotReset                     = 10;
constexpr size_t kSlotClearState                = 11;
constexpr size_t kSlotSetPipelineState          = 25;
constexpr size_t kSlotSetComputeRootSignature   = 29;
constexpr size_t kSlotSetComputeRootDescTable   = 31;
constexpr size_t kSlotSetComputeRoot32BitConst  = 33;
constexpr size_t kSlotSetComputeRoot32BitConsts = 35;
constexpr size_t kSlotSetComputeRootCbv         = 37;
constexpr size_t kSlotSetComputeRootSrv         = 39;
constexpr size_t kSlotSetComputeRootUav         = 41;
constexpr size_t kSlotSetPipelineState1         = 75;
constexpr size_t kDeviceSlotCreateCommandList   = 12;

using PFN_GetType    = D3D12_COMMAND_LIST_TYPE(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Close      = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*);
using PFN_Reset      = HRESULT(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
using PFN_ClearState = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_SetPSO     = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using PFN_SetRS      = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using PFN_SetTable   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
using PFN_SetConst   = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
using PFN_SetConsts  = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
using PFN_SetView    = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
using PFN_SetPSO1    = void(STDMETHODCALLTYPE*)(ID3D12GraphicsCommandList*, ID3D12StateObject*);
using PFN_CreateCommandList = HRESULT(STDMETHODCALLTYPE*)(ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE,
                                                          ID3D12CommandAllocator*, ID3D12PipelineState*, REFIID, void**);

// One entry per distinct set of original functions (≈ one per list type per driver). Hook
// functions are templated on the set index, so the original is found without a lookup.
struct OrigSet {
    PFN_GetType    getType    = nullptr;
    PFN_Close      close      = nullptr;
    PFN_ClearState clearState = nullptr;
    PFN_SetPSO     setPso     = nullptr;
    PFN_SetRS      setRs      = nullptr;
    PFN_SetTable   setTable   = nullptr;
    PFN_SetConst   setConst   = nullptr;
    PFN_SetConsts  setConsts  = nullptr;
    PFN_SetView    setCbv     = nullptr;
    PFN_SetView    setSrv     = nullptr;
    PFN_SetView    setUav     = nullptr;
    PFN_SetPSO1    setPso1    = nullptr;
};

constexpr int kMaxSets = 8;
OrigSet g_sets[kMaxSets];
std::atomic<int> g_setCount{0};
std::mutex g_patchMutex;

std::shared_mutex g_stateMutex;
std::unordered_map<const void*, std::unique_ptr<CmdListComputeState>> g_states;

thread_local const void*          t_cachedList  = nullptr;
thread_local CmdListComputeState* t_cachedState = nullptr;
thread_local int                  t_suppress    = 0;   // >0 while the wrapper records commands
thread_local const void*          t_probeResult = nullptr;
thread_local bool                 t_probing     = false;

CmdListComputeState* GetState(const void* list, bool create) {
    if (t_cachedList == list && t_cachedState) {
        return t_cachedState;
    }
    {
        std::shared_lock<std::shared_mutex> lock(g_stateMutex);
        auto it = g_states.find(list);
        if (it != g_states.end()) {
            t_cachedList  = list;
            t_cachedState = it->second.get();
            return t_cachedState;
        }
    }
    if (!create) {
        return nullptr;
    }
    std::unique_lock<std::shared_mutex> lock(g_stateMutex);
    auto& slot = g_states[list];
    if (!slot) {
        slot = std::make_unique<CmdListComputeState>(); // entries are never freed: pointers stay valid
    }
    t_cachedList  = list;
    t_cachedState = slot.get();
    return t_cachedState;
}

RootArg* TouchArg(CmdListComputeState* s, UINT index) {
    if (!s || index >= kMaxRootParams) {
        return nullptr;
    }
    if (index + 1 > s->maxArg) {
        s->maxArg = index + 1;
    }
    return &s->args[index];
}

void ResetState(const void* list, ID3D12PipelineState* initial) {
    if (CmdListComputeState* s = GetState(list, true)) {
        s->Clear();
        if (initial) {
            s->pipelineKind = PipelineKind::Pso;
            s->pso          = initial;
        }
    }
}

// ---------------------------------------------------------------------------
// Per-list (open state) hooks, templated on the original-function set
// ---------------------------------------------------------------------------
template <int I>
D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE Hook_GetType(ID3D12GraphicsCommandList* self) {
    if (t_probing) {
        t_probeResult = self;
    }
    return g_sets[I].getType(self);
}

template <int I>
HRESULT STDMETHODCALLTYPE Hook_Close(ID3D12GraphicsCommandList* self) {
    if (CmdListComputeState* s = GetState(self, false)) {
        s->Clear();
    }
    return g_sets[I].close(self);
}

template <int I>
void STDMETHODCALLTYPE Hook_ClearState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    g_sets[I].clearState(self, pso);
    ResetState(self, pso);
}

template <int I>
void STDMETHODCALLTYPE Hook_SetPipelineState(ID3D12GraphicsCommandList* self, ID3D12PipelineState* pso) {
    g_sets[I].setPso(self, pso);
    if (t_suppress == 0) {
        if (CmdListComputeState* s = GetState(self, true)) {
            s->pipelineKind = PipelineKind::Pso;
            s->pso          = pso;
            s->stateObject  = nullptr;
        }
    }
}

template <int I>
void STDMETHODCALLTYPE Hook_SetPipelineState1(ID3D12GraphicsCommandList* self, ID3D12StateObject* so) {
    g_sets[I].setPso1(self, so);
    if (t_suppress == 0) {
        if (CmdListComputeState* s = GetState(self, true)) {
            s->pipelineKind = PipelineKind::StateObject;
            s->stateObject  = so;
            s->pso          = nullptr;
        }
    }
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRootSignature(ID3D12GraphicsCommandList* self, ID3D12RootSignature* rs) {
    g_sets[I].setRs(self, rs);
    if (t_suppress == 0) {
        if (CmdListComputeState* s = GetState(self, true)) {
            if (s->computeRootSig != rs) {
                s->ClearRootArgs(); // a signature change invalidates all root arguments
            }
            s->computeRootSig = rs;
        }
    }
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRootDescriptorTable(ID3D12GraphicsCommandList* self, UINT idx, D3D12_GPU_DESCRIPTOR_HANDLE h) {
    g_sets[I].setTable(self, idx, h);
    if (t_suppress == 0) {
        if (RootArg* a = TouchArg(GetState(self, true), idx)) {
            a->kind  = RootArgKind::Table;
            a->value = h.ptr;
        }
    }
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRoot32BitConstant(ID3D12GraphicsCommandList* self, UINT idx, UINT data, UINT offset) {
    g_sets[I].setConst(self, idx, data, offset);
    if (t_suppress == 0 && offset < kMaxRootParams) {
        if (RootArg* a = TouchArg(GetState(self, true), idx)) {
            if (a->kind != RootArgKind::Constants) {
                a->kind      = RootArgKind::Constants;
                a->constMask = 0;
            }
            a->constants[offset] = data;
            a->constMask |= (1ull << offset);
        }
    }
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRoot32BitConstants(ID3D12GraphicsCommandList* self, UINT idx, UINT num, const void* data, UINT offset) {
    g_sets[I].setConsts(self, idx, num, data, offset);
    if (t_suppress == 0 && data) {
        if (RootArg* a = TouchArg(GetState(self, true), idx)) {
            if (a->kind != RootArgKind::Constants) {
                a->kind      = RootArgKind::Constants;
                a->constMask = 0;
            }
            const uint32_t* src = static_cast<const uint32_t*>(data);
            for (UINT i = 0; i < num && offset + i < kMaxRootParams; ++i) {
                a->constants[offset + i] = src[i];
                a->constMask |= (1ull << (offset + i));
            }
        }
    }
}

void RecordView(ID3D12GraphicsCommandList* self, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va, RootArgKind kind) {
    if (t_suppress == 0) {
        if (RootArg* a = TouchArg(GetState(self, true), idx)) {
            a->kind  = kind;
            a->value = va;
        }
    }
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRootCbv(ID3D12GraphicsCommandList* self, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    g_sets[I].setCbv(self, idx, va);
    RecordView(self, idx, va, RootArgKind::Cbv);
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRootSrv(ID3D12GraphicsCommandList* self, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    g_sets[I].setSrv(self, idx, va);
    RecordView(self, idx, va, RootArgKind::Srv);
}

template <int I>
void STDMETHODCALLTYPE Hook_SetComputeRootUav(ID3D12GraphicsCommandList* self, UINT idx, D3D12_GPU_VIRTUAL_ADDRESS va) {
    g_sets[I].setUav(self, idx, va);
    RecordView(self, idx, va, RootArgKind::Uav);
}

struct HookTable {
    void* getType;
    void* close;
    void* clearState;
    void* setPso;
    void* setRs;
    void* setTable;
    void* setConst;
    void* setConsts;
    void* setCbv;
    void* setSrv;
    void* setUav;
    void* setPso1;
};

template <int I>
constexpr HookTable MakeHookTable() {
    return HookTable{
        reinterpret_cast<void*>(&Hook_GetType<I>),
        reinterpret_cast<void*>(&Hook_Close<I>),
        reinterpret_cast<void*>(&Hook_ClearState<I>),
        reinterpret_cast<void*>(&Hook_SetPipelineState<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRootSignature<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRootDescriptorTable<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRoot32BitConstant<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRoot32BitConstants<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRootCbv<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRootSrv<I>),
        reinterpret_cast<void*>(&Hook_SetComputeRootUav<I>),
        reinterpret_cast<void*>(&Hook_SetPipelineState1<I>),
    };
}

const HookTable g_hookTables[kMaxSets] = {
    MakeHookTable<0>(), MakeHookTable<1>(), MakeHookTable<2>(), MakeHookTable<3>(),
    MakeHookTable<4>(), MakeHookTable<5>(), MakeHookTable<6>(), MakeHookTable<7>(),
};

bool IsOurHook(void* fn) {
    for (const HookTable& t : g_hookTables) {
        if (t.setRs == fn) {
            return true;
        }
    }
    return false;
}

bool IsInModuleImage(const void* p) {
    HMODULE mod = nullptr;
    return GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              static_cast<LPCSTR>(p), &mod) && mod;
}

void WriteSlot(void** vt, size_t slot, void* fn) {
    if (vt[slot] == fn) {
        return;
    }
    DWORD oldProt = 0;
    if (VirtualProtect(&vt[slot], sizeof(void*), PAGE_EXECUTE_READWRITE, &oldProt)) {
        vt[slot] = fn;
        VirtualProtect(&vt[slot], sizeof(void*), oldProt, &oldProt);
    }
}

// Patches the vtable the (open) list currently uses. Caller holds g_patchMutex.
bool PatchOpenListVtableLocked(ID3D12GraphicsCommandList* list, bool hasList4) {
    void** vt = *reinterpret_cast<void***>(list);
    if (IsOurHook(vt[kSlotSetComputeRootSignature])) {
        return true;
    }

    OrigSet cur{};
    cur.getType    = reinterpret_cast<PFN_GetType>(vt[kSlotGetType]);
    cur.close      = reinterpret_cast<PFN_Close>(vt[kSlotClose]);
    cur.clearState = reinterpret_cast<PFN_ClearState>(vt[kSlotClearState]);
    cur.setPso     = reinterpret_cast<PFN_SetPSO>(vt[kSlotSetPipelineState]);
    cur.setRs      = reinterpret_cast<PFN_SetRS>(vt[kSlotSetComputeRootSignature]);
    cur.setTable   = reinterpret_cast<PFN_SetTable>(vt[kSlotSetComputeRootDescTable]);
    cur.setConst   = reinterpret_cast<PFN_SetConst>(vt[kSlotSetComputeRoot32BitConst]);
    cur.setConsts  = reinterpret_cast<PFN_SetConsts>(vt[kSlotSetComputeRoot32BitConsts]);
    cur.setCbv     = reinterpret_cast<PFN_SetView>(vt[kSlotSetComputeRootCbv]);
    cur.setSrv     = reinterpret_cast<PFN_SetView>(vt[kSlotSetComputeRootSrv]);
    cur.setUav     = reinterpret_cast<PFN_SetView>(vt[kSlotSetComputeRootUav]);
    cur.setPso1    = hasList4 ? reinterpret_cast<PFN_SetPSO1>(vt[kSlotSetPipelineState1]) : nullptr;

    const int n = g_setCount.load(std::memory_order_relaxed);
    int index = -1;
    for (int i = 0; i < n; ++i) {
        const OrigSet& s = g_sets[i];
        if (s.getType == cur.getType && s.close == cur.close && s.clearState == cur.clearState &&
            s.setPso == cur.setPso && s.setRs == cur.setRs && s.setTable == cur.setTable &&
            s.setConst == cur.setConst && s.setConsts == cur.setConsts && s.setCbv == cur.setCbv &&
            s.setSrv == cur.setSrv && s.setUav == cur.setUav && (!hasList4 || s.setPso1 == cur.setPso1)) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        if (n >= kMaxSets) {
            static bool warned = false;
            if (!warned) {
                Log("[State] WARNING: too many distinct command-list function sets; state shadowing incomplete");
                warned = true;
            }
            return false;
        }
        index = n;
        g_sets[index] = cur; // published before any vtable points at Hook_*<index>
        g_setCount.store(n + 1, std::memory_order_release);
        Log("[State] New command-list function set #%d (type=%d, vtable %s, SetComputeRootSignature=%p)",
            index, static_cast<int>(cur.getType(list)), IsInModuleImage(vt) ? "static" : "per-list",
            reinterpret_cast<void*>(cur.setRs));
    } else if (hasList4 && !g_sets[index].setPso1) {
        g_sets[index].setPso1 = cur.setPso1;
    }

    const HookTable& h = g_hookTables[index];
    WriteSlot(vt, kSlotGetType,                   h.getType);
    WriteSlot(vt, kSlotClose,                     h.close);
    WriteSlot(vt, kSlotClearState,                h.clearState);
    WriteSlot(vt, kSlotSetPipelineState,          h.setPso);
    WriteSlot(vt, kSlotSetComputeRootDescTable,   h.setTable);
    WriteSlot(vt, kSlotSetComputeRoot32BitConst,  h.setConst);
    WriteSlot(vt, kSlotSetComputeRoot32BitConsts, h.setConsts);
    WriteSlot(vt, kSlotSetComputeRootCbv,         h.setCbv);
    WriteSlot(vt, kSlotSetComputeRootSrv,         h.setSrv);
    WriteSlot(vt, kSlotSetComputeRootUav,         h.setUav);
    if (hasList4) {
        WriteSlot(vt, kSlotSetPipelineState1,     h.setPso1);
    }
    WriteSlot(vt, kSlotSetComputeRootSignature,   h.setRs); // last: marks the vtable as patched
    return true;
}

bool PatchOpenList(ID3D12GraphicsCommandList* list) {
    if (!list) {
        return false;
    }
    void** vt = *reinterpret_cast<void***>(list);
    if (IsOurHook(vt[kSlotSetComputeRootSignature])) {
        return true;
    }
    ComPtr<ID3D12GraphicsCommandList4> list4;
    list->QueryInterface(IID_PPV_ARGS(&list4));
    std::lock_guard<std::mutex> lock(g_patchMutex);
    return PatchOpenListVtableLocked(list, list4 != nullptr);
}

// ---------------------------------------------------------------------------
// Closed-state Reset and device CreateCommandList hooks
// ---------------------------------------------------------------------------
struct ClosedResetEntry {
    void**    vtable = nullptr;
    PFN_Reset orig   = nullptr;
};
ClosedResetEntry g_closedResets[4];
std::atomic<int> g_closedResetCount{0};

PFN_Reset FindClosedReset(const void* list) {
    void** vt = *reinterpret_cast<void** const*>(list);
    const int n = g_closedResetCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        if (g_closedResets[i].vtable == vt) {
            return g_closedResets[i].orig;
        }
    }
    return n > 0 ? g_closedResets[0].orig : nullptr;
}

HRESULT STDMETHODCALLTYPE Hook_ClosedReset(ID3D12GraphicsCommandList* self, ID3D12CommandAllocator* alloc, ID3D12PipelineState* initial) {
    PFN_Reset orig = FindClosedReset(self);
    HRESULT hr = orig ? orig(self, alloc, initial) : E_FAIL;
    if (SUCCEEDED(hr)) {
        PatchOpenList(self);
        ResetState(self, initial);
    }
    return hr;
}

PFN_CreateCommandList g_origCreateCommandList = nullptr;

HRESULT STDMETHODCALLTYPE Hook_CreateCommandList(ID3D12Device* dev, UINT mask, D3D12_COMMAND_LIST_TYPE type,
                                                 ID3D12CommandAllocator* alloc, ID3D12PipelineState* initial,
                                                 REFIID riid, void** ppList) {
    HRESULT hr = g_origCreateCommandList(dev, mask, type, alloc, initial, riid, ppList);
    if (SUCCEEDED(hr) && ppList && *ppList) {
        ComPtr<ID3D12GraphicsCommandList> gfx;
        if (SUCCEEDED(static_cast<IUnknown*>(*ppList)->QueryInterface(IID_PPV_ARGS(&gfx))) && gfx) {
            PatchOpenList(gfx.Get()); // created in the recording state
            ResetState(gfx.Get(), initial);
        }
    }
    return hr;
}

void HookClosedReset(ID3D12GraphicsCommandList* closedList) {
    void** vt = *reinterpret_cast<void***>(closedList);
    if (vt[kSlotReset] == reinterpret_cast<void*>(&Hook_ClosedReset)) {
        return;
    }
    const int n = g_closedResetCount.load(std::memory_order_relaxed);
    for (int i = 0; i < n; ++i) {
        if (g_closedResets[i].vtable == vt) {
            return;
        }
    }
    if (n >= 4) {
        return;
    }
    g_closedResets[n].vtable = vt;
    g_closedResets[n].orig   = reinterpret_cast<PFN_Reset>(vt[kSlotReset]);
    g_closedResetCount.store(n + 1, std::memory_order_release);
    WriteSlot(vt, kSlotReset, reinterpret_cast<void*>(&Hook_ClosedReset));
    Log("[State] Hooked closed-state Reset (vtable=%p, %s)", vt, IsInModuleImage(vt) ? "static" : "per-list");
}

void Log_NoShadowOnce() {
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        Log("[State] No compute state recorded for a command list (opened before the hooks): nothing to restore");
    }
}

} // namespace

bool HookCommandListForStateShadow(ID3D12GraphicsCommandList* pList) {
    return PatchOpenList(pList);
}

void HookDeviceCommandListsForStateShadow(ID3D12Device* pDevice) {
    if (!pDevice) {
        return;
    }
    {
        // AMD_MG_STATE_SHADOW=0: diagnostic switch, no command-list hooks at all.
        char value[8] = {};
        const DWORD len = GetEnvironmentVariableA("AMD_MG_STATE_SHADOW", value, sizeof(value));
        if (len > 0 && len < sizeof(value) && std::atoi(value) == 0) {
            static std::atomic<bool> logged{false};
            if (!logged.exchange(true)) {
                Log("[State] AMD_MG_STATE_SHADOW=0: command-list state shadowing disabled");
            }
            return;
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_patchMutex);
        void** dvt = *reinterpret_cast<void***>(pDevice);
        if (dvt[kDeviceSlotCreateCommandList] != reinterpret_cast<void*>(&Hook_CreateCommandList)) {
            if (!g_origCreateCommandList) {
                g_origCreateCommandList = reinterpret_cast<PFN_CreateCommandList>(dvt[kDeviceSlotCreateCommandList]);
            }
            WriteSlot(dvt, kDeviceSlotCreateCommandList, reinterpret_cast<void*>(&Hook_CreateCommandList));
            Log("[State] Hooked ID3D12Device::CreateCommandList (vtable=%p)", dvt);
        }
    }

    const D3D12_COMMAND_LIST_TYPE types[] = { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_LIST_TYPE_COMPUTE };
    for (D3D12_COMMAND_LIST_TYPE type : types) {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(pDevice->CreateCommandAllocator(type, IID_PPV_ARGS(&alloc))) ||
            FAILED(pDevice->CreateCommandList(0, type, alloc.Get(), nullptr, IID_PPV_ARGS(&list)))) {
            Log("[State] Could not create a probe command list (type=%d)", static_cast<int>(type));
            continue;
        }
        list->Close();
        std::lock_guard<std::mutex> lock(g_patchMutex);
        HookClosedReset(list.Get());
    }
}

ComputeStateScope::ComputeStateScope(ID3D12GraphicsCommandList* pList) : m_list(pList) {
    if (!pList) {
        return;
    }
    // The game may hand us a wrapper (Streamline/OptiScaler proxies). Find the runtime list by
    // letting a forwarded GetType() reach our per-list hook.
    t_probing     = true;
    t_probeResult = nullptr;
    pList->GetType();
    t_probing = false;
    m_key = t_probeResult ? t_probeResult : static_cast<const void*>(pList);

    if (!t_probeResult) {
        // List opened before our hooks were in place: patch now; the state bound so far in
        // this recording is unknown, so nothing can be restored for this call.
        PatchOpenList(pList);
    }
    if (CmdListComputeState* s = GetState(m_key, false); s && (s->computeRootSig || s->pipelineKind != PipelineKind::None)) {
        m_snapshot = std::make_unique<CmdListComputeState>(*s);
    } else {
        Log_NoShadowOnce();
    }
    ++t_suppress;
}

ComputeStateScope::~ComputeStateScope() {
    if (!m_list) {
        return;
    }
    if (m_snapshot) {
        const CmdListComputeState& s = *m_snapshot;
        if (s.computeRootSig) {
            m_list->SetComputeRootSignature(s.computeRootSig);
            for (uint32_t i = 0; i < s.maxArg; ++i) {
                const RootArg& a = s.args[i];
                switch (a.kind) {
                case RootArgKind::Table: {
                    D3D12_GPU_DESCRIPTOR_HANDLE h{};
                    h.ptr = a.value;
                    m_list->SetComputeRootDescriptorTable(i, h);
                    break;
                }
                case RootArgKind::Constants:
                    for (uint32_t c = 0; c < kMaxRootParams; ++c) {
                        if (a.constMask & (1ull << c)) {
                            m_list->SetComputeRoot32BitConstant(i, a.constants[c], c);
                        }
                    }
                    break;
                case RootArgKind::Cbv: m_list->SetComputeRootConstantBufferView(i, a.value); break;
                case RootArgKind::Srv: m_list->SetComputeRootShaderResourceView(i, a.value); break;
                case RootArgKind::Uav: m_list->SetComputeRootUnorderedAccessView(i, a.value); break;
                default: break;
                }
            }
        }
        if (s.pipelineKind == PipelineKind::Pso && s.pso) {
            m_list->SetPipelineState(s.pso);
        } else if (s.pipelineKind == PipelineKind::StateObject && s.stateObject) {
            ComPtr<ID3D12GraphicsCommandList4> list4;
            if (SUCCEEDED(m_list->QueryInterface(IID_PPV_ARGS(&list4))) && list4) {
                list4->SetPipelineState1(s.stateObject);
            }
        }
    }
    --t_suppress;
}

} // namespace amd_mg
