// ============================================================================
// amd-megageometry: DRED (Device Removed Extended Data) diagnostics
//
// Enabled before device creation (default on; AMD_MG_DRED=0 disables it). When the device is
// removed, the auto-breadcrumbs of every unfinished command list and the page-fault VA with
// the allocations around it are written to amd_megageometry.log.
// ============================================================================
#include "amd_mg.h"

#include <atomic>
#include <cstdlib>

namespace amd_mg {

namespace {

using Microsoft::WRL::ComPtr;

#define AMD_MG_BREADCRUMB_OPS(X) \
    X(SETMARKER) X(BEGINEVENT) X(ENDEVENT) X(DRAWINSTANCED) X(DRAWINDEXEDINSTANCED) X(EXECUTEINDIRECT) \
    X(DISPATCH) X(COPYBUFFERREGION) X(COPYTEXTUREREGION) X(COPYRESOURCE) X(COPYTILES) X(RESOLVESUBRESOURCE) \
    X(CLEARRENDERTARGETVIEW) X(CLEARUNORDEREDACCESSVIEW) X(CLEARDEPTHSTENCILVIEW) X(RESOURCEBARRIER) \
    X(EXECUTEBUNDLE) X(PRESENT) X(RESOLVEQUERYDATA) X(BEGINSUBMISSION) X(ENDSUBMISSION) X(DECODEFRAME) \
    X(PROCESSFRAMES) X(ATOMICCOPYBUFFERUINT) X(ATOMICCOPYBUFFERUINT64) X(RESOLVESUBRESOURCEREGION) \
    X(WRITEBUFFERIMMEDIATE) X(DECODEFRAME1) X(SETPROTECTEDRESOURCESESSION) X(DECODEFRAME2) X(PROCESSFRAMES1) \
    X(BUILDRAYTRACINGACCELERATIONSTRUCTURE) X(EMITRAYTRACINGACCELERATIONSTRUCTUREPOSTBUILDINFO) \
    X(COPYRAYTRACINGACCELERATIONSTRUCTURE) X(DISPATCHRAYS) X(INITIALIZEMETACOMMAND) X(EXECUTEMETACOMMAND) \
    X(ESTIMATEMOTION) X(RESOLVEMOTIONVECTORHEAP) X(SETPIPELINESTATE1) X(INITIALIZEEXTENSIONCOMMAND) \
    X(EXECUTEEXTENSIONCOMMAND) X(DISPATCHMESH) X(ENCODEFRAME) X(RESOLVEENCODEROUTPUTMETADATA) X(BARRIER) \
    X(BEGIN_COMMAND_LIST) X(DISPATCHGRAPH) X(SETPROGRAM)

const char* OpName(D3D12_AUTO_BREADCRUMB_OP op) {
    switch (op) {
#define AMD_MG_OP_CASE(n) case D3D12_AUTO_BREADCRUMB_OP_##n: return #n;
    AMD_MG_BREADCRUMB_OPS(AMD_MG_OP_CASE)
#undef AMD_MG_OP_CASE
    default: return nullptr;
    }
}

bool DredRequested() {
    char value[8] = {};
    const DWORD len = GetEnvironmentVariableA("AMD_MG_DRED", value, sizeof(value));
    return !(len > 0 && len < sizeof(value) && std::atoi(value) == 0);
}

std::string Narrow(const wchar_t* w) {
    if (!w) {
        return {};
    }
    char buf[256];
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, sizeof(buf), nullptr, nullptr);
    return n > 0 ? std::string(buf) : std::string();
}

std::string NodeName(const char* a, const wchar_t* w) {
    if (a && *a) {
        return a;
    }
    std::string s = Narrow(w);
    return s.empty() ? std::string("<unnamed>") : s;
}

void LogAllocations(const char* label, const D3D12_DRED_ALLOCATION_NODE1* node) {
    int count = 0;
    for (; node && count < 24; node = node->pNext, ++count) {
        Log("[DRED]   %s: %s (type=%d, object=%p)", label,
            NodeName(node->ObjectNameA, node->ObjectNameW).c_str(),
            static_cast<int>(node->AllocationType), node->pObject);
    }
    if (count == 0) {
        Log("[DRED]   %s: none", label);
    }
}

struct WatchContext {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence>  fence;
    HANDLE               event = nullptr;
    HANDLE               wait  = nullptr;
};

std::atomic<bool> g_dumped{false};

void DumpDred(ID3D12Device* device) {
    if (g_dumped.exchange(true)) {
        return;
    }
    const HRESULT reason = device->GetDeviceRemovedReason();
    Log("[DRED] ===== Device removed: reason=0x%08X =====", reason);

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred1;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred1))) || !dred1) {
        Log("[DRED] ID3D12DeviceRemovedExtendedData1 not available (DRED not enabled?)");
        return;
    }

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
    if (SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&crumbs))) {
        int nodeIndex = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE1* n = crumbs.pHeadAutoBreadcrumbNode; n; n = n->pNext, ++nodeIndex) {
            const UINT last  = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            const UINT total = n->BreadcrumbCount;
            if (last >= total) {
                continue; // command list completed
            }
            Log("[DRED] Unfinished list #%d: list=%s queue=%s completed %u/%u ops",
                nodeIndex, NodeName(n->pCommandListDebugNameA, n->pCommandListDebugNameW).c_str(),
                NodeName(n->pCommandQueueDebugNameA, n->pCommandQueueDebugNameW).c_str(), last, total);
            const UINT from = last > 24 ? last - 24 : 0;
            const UINT to   = (last + 6 < total) ? last + 6 : total;
            for (UINT i = from; i < to; ++i) {
                const D3D12_AUTO_BREADCRUMB_OP op = n->pCommandHistory[i];
                const char* name = OpName(op);
                std::string ctx;
                for (UINT c = 0; c < n->BreadcrumbContextsCount; ++c) {
                    if (n->pBreadcrumbContexts[c].BreadcrumbIndex == i) {
                        ctx = Narrow(n->pBreadcrumbContexts[c].pContextString);
                        break;
                    }
                }
                char opbuf[32];
                if (!name) {
                    std::snprintf(opbuf, sizeof(opbuf), "OP_%d", static_cast<int>(op));
                    name = opbuf;
                }
                Log("[DRED]   %s [%u] %s%s%s", i == last ? "==>" : "   ", i, name,
                    ctx.empty() ? "" : "  ctx=", ctx.c_str());
            }
        }
    } else {
        Log("[DRED] GetAutoBreadcrumbsOutput1 failed");
    }

    ComPtr<ID3D12DeviceRemovedExtendedData2> dred2;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dred2))) && dred2) {
        D3D12_DRED_PAGE_FAULT_OUTPUT2 pf{};
        if (SUCCEEDED(dred2->GetPageFaultAllocationOutput2(&pf))) {
            Log("[DRED] Page fault VA=0x%llx (device state=%d)",
                static_cast<unsigned long long>(pf.PageFaultVA), static_cast<int>(dred2->GetDeviceState()));
            LogAllocations("existing", pf.pHeadExistingAllocationNode);
            LogAllocations("recently freed", pf.pHeadRecentFreedAllocationNode);
            DescribeConverterAllocationsNear(pf.PageFaultVA);
        }
    } else {
        D3D12_DRED_PAGE_FAULT_OUTPUT1 pf{};
        if (SUCCEEDED(dred1->GetPageFaultAllocationOutput1(&pf))) {
            Log("[DRED] Page fault VA=0x%llx", static_cast<unsigned long long>(pf.PageFaultVA));
            LogAllocations("existing", pf.pHeadExistingAllocationNode);
            LogAllocations("recently freed", pf.pHeadRecentFreedAllocationNode);
        }
    }
    FlushArgDumpsForDiagnostics();
    Log("[DRED] ===== end =====");
}

void CALLBACK OnDeviceRemoved(PVOID ctx, BOOLEAN) {
    auto* w = static_cast<WatchContext*>(ctx);
    DumpDred(w->device.Get());
}

} // namespace

void EnableDredBeforeDeviceCreation() {
    static std::atomic<bool> done{false};
    if (done.exchange(true) || !DredRequested()) {
        return;
    }
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> settings;
    HRESULT hr = D3D12GetDebugInterface(IID_PPV_ARGS(&settings));
    if (FAILED(hr) || !settings) {
        Log("[DRED] D3D12GetDebugInterface(DRED settings) failed: 0x%08X", hr);
        return;
    }
    settings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    settings->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
    Log("[DRED] Auto-breadcrumbs + page-fault reporting enabled (AMD_MG_DRED=0 to disable)");
}

void WatchDeviceRemoval(ID3D12Device* pDevice) {
    if (!pDevice || !DredRequested()) {
        return;
    }
    static std::mutex s_mutex;
    static std::vector<ID3D12Device*> s_watched;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (ID3D12Device* d : s_watched) {
        if (d == pDevice) {
            return;
        }
    }
    auto* w = new WatchContext(); // lives for the process lifetime
    w->device = pDevice;
    w->event  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!w->event || FAILED(pDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&w->fence))) ||
        FAILED(w->fence->SetEventOnCompletion(UINT64_MAX, w->event)) ||
        !RegisterWaitForSingleObject(&w->wait, w->event, &OnDeviceRemoved, w, INFINITE, WT_EXECUTEONLYONCE)) {
        Log("[DRED] Could not register the device-removal watch");
        return;
    }
    s_watched.push_back(pDevice);
    Log("[DRED] Watching device %p for removal", pDevice);
}

} // namespace amd_mg
