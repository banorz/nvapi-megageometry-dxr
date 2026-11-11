#include "amd_mg.h"
#include "dxcapi.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <set>
#include <fstream>
#include <regex>
#include <sstream>

namespace amd_mg {

namespace {

HMODULE g_hDxcompiler = nullptr;
HMODULE g_hDxil       = nullptr;
DxcCreateInstanceProc g_pfnDxcCreateInstance  = nullptr;
DxcCreateInstanceProc g_pfnDxilCreateInstance = nullptr;
std::mutex g_dxcMutex;

HMODULE LoadDxcModule(const wchar_t* dllName) {
    // 1. Check already loaded
    if (HMODULE h = GetModuleHandleW(dllName)) {
        return h;
    }
    // 2. Check next to our own DLL / EXE
    wchar_t path[MAX_PATH]{};
    HMODULE hSelf = nullptr;
    GetModuleHandleExW(
        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&LoadDxcModule),
        &hSelf);
    if (GetModuleFileNameW(hSelf, path, MAX_PATH)) {
        wchar_t* lastSlash = wcsrchr(path, L'\\');
        if (lastSlash) {
            *(lastSlash + 1) = L'\0';
            std::wstring candidate = std::wstring(path) + dllName;
            if (HMODULE h = LoadLibraryW(candidate.c_str())) {
                return h;
            }
            // Also check ../sdk/dxc/extracted/bin/x64/ for local workspace builds
            std::wstring sdkCandidate = std::wstring(path) + L"..\\sdk\\dxc\\extracted\\bin\\x64\\" + dllName;
            if (HMODULE h = LoadLibraryW(sdkCandidate.c_str())) {
                return h;
            }
            std::wstring sdkCandidate2 = std::wstring(path) + L"sdk\\dxc\\extracted\\bin\\x64\\" + dllName;
            if (HMODULE h = LoadLibraryW(sdkCandidate2.c_str())) {
                return h;
            }
        }
    }
    return LoadLibraryW(dllName);
}

} // namespace

bool InitializeDxilPatcher() {
    std::lock_guard<std::mutex> lock(g_dxcMutex);
    if (g_pfnDxcCreateInstance) {
        return true;
    }
    g_hDxil = LoadDxcModule(L"dxil.dll");
    if (g_hDxil) {
        g_pfnDxilCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(
            GetProcAddress(g_hDxil, "DxcCreateInstance"));
    }
    g_hDxcompiler = LoadDxcModule(L"dxcompiler.dll");
    if (!g_hDxcompiler) {
        Log("[DXIL] Failed to load dxcompiler.dll");
        return false;
    }
    g_pfnDxcCreateInstance = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(g_hDxcompiler, "DxcCreateInstance"));
    if (!g_pfnDxcCreateInstance) {
        Log("[DXIL] dxcompiler.dll missing DxcCreateInstance");
        return false;
    }
    Log("[DXIL] Loaded dxcompiler.dll (%p) and dxil.dll (%p)", g_hDxcompiler, g_hDxil);
    return true;
}

void ShutdownDxilPatcher() {
    std::lock_guard<std::mutex> lock(g_dxcMutex);
    g_pfnDxcCreateInstance  = nullptr;
    g_pfnDxilCreateInstance = nullptr;
}

// ============================================================================
// TransformDxilIrText: Rewrites NvAPI fake-UAV cluster intrinsics into SM 6.10
// ============================================================================
bool TransformDxilIrText(
    const std::string& irText,
    uint32_t uavSlot,
    uint32_t uavSpace,
    std::string& outIrText,
    uint32_t& outPatchCount)
{
    outPatchCount = 0;

    // Quick check: does the shader reference rawBufferStore or bufferUpdateCounter?
    if (irText.find("bufferUpdateCounter") == std::string::npos ||
        irText.find("rawBufferStore") == std::string::npos) {
        return false;
    }

    std::istringstream iss(irText);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    // 1. Identify the NVIDIA extension UAV global symbol name (e.g., @"\01?g_NvidiaExt...")
    // Either by matching `NvShaderExtnStruct` or the UAV binding with `(uavSpace, uavSlot)`.
    std::string nvGlobalName;
    for (const auto& l : lines) {
        if (l.find("NvShaderExtnStruct") != std::string::npos && l.find("@\"") != std::string::npos &&
            l.find("external constant %dx.types.Handle") != std::string::npos) {
            size_t pos = l.find("@\"");
            size_t end = l.find("\"", pos + 2);
            if (pos != std::string::npos && end != std::string::npos) {
                nvGlobalName = l.substr(pos, end - pos + 1);
                break;
            }
        }
    }
    if (nvGlobalName.empty()) {
        // Fallback: search metadata line with uavSpace, uavSlot and stride 256
        std::string bindPattern = "i32 " + std::to_string(uavSpace) + ", i32 " + std::to_string(uavSlot) + ", i32 1, i32 12";
        for (const auto& l : lines) {
            if (l.find(bindPattern) != std::string::npos && l.find("@") != std::string::npos) {
                size_t pos = l.find('@');
                size_t end = l.find(" to ", pos);
                if (pos != std::string::npos && end != std::string::npos) {
                    nvGlobalName = l.substr(pos, end - pos);
                    break;
                }
            }
        }
    }
    if (nvGlobalName.empty()) {
        return false;
    }

    bool needClusterIdDecl         = false;
    bool needTriPosDecl            = false;
    bool needRqStateScalarDecl     = false;
    bool needRqCandidateTriPosDecl = false;
    bool needRqCommittedTriPosDecl = false;
    bool needHoStateScalarDecl     = false;
    bool needHoTriPosDecl          = false;
    bool needWaveLaneIndexDecl     = false;
    std::set<std::string> unsupportedOps;

    // 2. Scan inside functions for NvAPI extension sequences:
    // Each sequence starts with a `bufferUpdateCounter` that allocates the slot index `%idx`,
    // followed by `rawBufferStore.i32(..., i32 %idx, i32 0, i32 <opcode>, ...)` (and possibly
    // a preceding `rawBufferStore.i32` at offset 76 `src0u` for RayQuery / HitObject handle),
    // followed by 1 `bufferUpdateCounter` (for ClusterID opcodes 93, 94, 95, 96) or
    // 9 `bufferUpdateCounter`s (for TriangleObjectPositions opcodes 86, 97, 98, 99).
    std::regex reSsaAssign(R"(^\s*(%[0-9A-Za-z_\.]+)\s*=\s*(.*)$)");
    std::regex reHandleOperand(R"(%dx\.types\.Handle\s+(%[0-9A-Za-z_\.]+))");
    std::regex reCounter(R"(^\s*(%[0-9A-Za-z_\.]+)\s*=\s*call i32 @dx\.op\.bufferUpdateCounter\(i32 70,\s*%dx\.types\.Handle\s+(%[0-9A-Za-z_\.]+),\s*i8 1\))");
    std::regex reStoreOp(R"(^\s*call void @dx\.op\.rawBufferStore\.i32\(i32 140,\s*%dx\.types\.Handle\s+(%[0-9A-Za-z_\.]+),\s*i32\s+(%[0-9A-Za-z_\.]+),\s*i32\s+([0-9]+),\s*i32\s+([0-9A-Za-z_%]+))");

    std::vector<bool> lineIsNvHandleSetup(lines.size(), false);

    // Process each function body independently (`define ... {` .. `}`) so SSA names (%1, %2, ...)
    // never collide across different entry points in a DXIL library!
    size_t fnStart = 0;
    while (fnStart < lines.size()) {
        if (lines[fnStart].rfind("define ", 0) != 0) {
            ++fnStart;
            continue;
        }
        size_t fnEnd = fnStart + 1;
        while (fnEnd < lines.size() && lines[fnEnd] != "}") {
            ++fnEnd;
        }

        std::unordered_map<std::string, bool> isNvHandleVar;
        std::unordered_map<std::string, std::string> indexToSrc0Arg;
        std::unordered_map<std::string, size_t> counterDefLine;

        // Pass 2a: Track which SSA handles in this function come from nvGlobalName
        for (size_t i = fnStart; i < fnEnd; ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, reSsaAssign)) {
                std::string lhs = m[1].str();
                std::string rhs = m[2].str();
                if (rhs.find(nvGlobalName) != std::string::npos && rhs.find("load %dx.types.Handle") != std::string::npos) {
                    isNvHandleVar[lhs] = true;
                    lineIsNvHandleSetup[i] = true;
                } else if (rhs.find("@dx.op.createHandleForLib") != std::string::npos ||
                           rhs.find("@dx.op.annotateHandle") != std::string::npos ||
                           rhs.find("@dx.op.createHandle") != std::string::npos) {
                    std::smatch mh;
                    if (std::regex_search(rhs, mh, reHandleOperand)) {
                        std::string srcHandle = mh[1].str();
                        if (isNvHandleVar[srcHandle]) {
                            isNvHandleVar[lhs] = true;
                            lineIsNvHandleSetup[i] = true;
                        }
                    }
                }
            }
        }

        if (isNvHandleVar.empty()) {
            fnStart = fnEnd + 1;
            continue;
        }

        // Pass 2b: Record any offset 76 (src0u) stores for RayQuery/HitObject in this function
        for (size_t i = fnStart; i < fnEnd; ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, reStoreOp)) {
                std::string handleVar = m[1].str();
                std::string idxVar    = m[2].str();
                int elemOffset        = std::atoi(m[3].str().c_str());
                std::string val0      = m[4].str();
                if (isNvHandleVar[handleVar] && elemOffset == 76) {
                    indexToSrc0Arg[idxVar] = val0;
                    lines[i] = "  ; [amd-mg] removed src0u store for " + idxVar;
                }
            }
        }

        // Pass 2c: Map from counter index SSA var (e.g. "%4") to the line where it was defined in this function
        for (size_t i = fnStart; i < fnEnd; ++i) {
            std::smatch m;
            if (std::regex_search(lines[i], m, reCounter)) {
                std::string lhs       = m[1].str();
                std::string handleVar = m[2].str();
                if (isNvHandleVar[handleVar]) {
                    counterDefLine[lhs] = i;
                }
            }
        }

        // Pass 2d: Find rawBufferStore with offset 0 (opcode store) in this function
        for (size_t i = fnStart; i < fnEnd; ++i) {
            std::smatch m;
            if (!std::regex_search(lines[i], m, reStoreOp)) {
                continue;
            }
            std::string handleVar = m[1].str();
            std::string idxVar    = m[2].str();
            int elemOffset        = std::atoi(m[3].str().c_str());
            int opcode            = std::atoi(m[4].str().c_str());

            if (!isNvHandleVar[handleVar] || elemOffset != 0) {
                continue;
            }

            // Neutralize the initial counter allocation line (`%4 = call i32 @dx.op.bufferUpdateCounter...`)
            auto itDef = counterDefLine.find(idxVar);
            if (itDef != counterDefLine.end()) {
                lines[itDef->second] = "  " + idxVar + " = add i32 0, 0 ; [amd-mg] neutralized counter alloc";
            }

            if (opcode == 93 || opcode == 94 || opcode == 95 || opcode == 96) {
                // ClusterID family: find the next bufferUpdateCounter on nvHandle
                for (size_t j = i + 1; j < fnEnd; ++j) {
                    std::smatch mc;
                    if (std::regex_search(lines[j], mc, reCounter) && isNvHandleVar[mc[2].str()]) {
                        std::string outVar = mc[1].str();
                        lines[i] = "  ; [amd-mg] removed rawBufferStore opcode " + std::to_string(opcode);
                        if (opcode == 93) {
                            lines[j] = "  " + outVar + " = call i32 @dx.op.clusterID(i32 -2147483645) ; [amd-mg] ClusterID()";
                            needClusterIdDecl = true;
                        } else if (opcode == 94 || opcode == 95) {
                            std::string rqArg = indexToSrc0Arg.count(idxVar) ? indexToSrc0Arg[idxVar] : "0";
                            int dxOp = (opcode == 94) ? -2147483644 : -2147483643;
                            lines[j] = "  " + outVar + " = call i32 @dx.op.rayQuery_StateScalar.i32(i32 " +
                                       std::to_string(dxOp) + ", i32 " + rqArg + ") ; [amd-mg] RayQuery::ClusterID()";
                            needRqStateScalarDecl = true;
                        } else if (opcode == 96) {
                            std::string hoArg = indexToSrc0Arg.count(idxVar) ? indexToSrc0Arg[idxVar] : "undef";
                            lines[j] = "  " + outVar + " = call i32 @dx.op.hitObject_StateScalar.i32(i32 -2147483642, %dx.types.HitObject " +
                                       hoArg + ") ; [amd-mg] HitObject::ClusterID()";
                            needHoStateScalarDecl = true;
                        }
                        outPatchCount++;
                        break;
                    }
                }
            } else if (opcode == 86 || opcode == 97 || opcode == 98 || opcode == 99) {
                // TriangleObjectPositions family: emits <9 x float> and feeds the next 9 bufferUpdateCounter calls
                std::string vecName = "%amd_mg_tri_pos_" + std::to_string(i);
                if (opcode == 86) {
                    lines[i] = "  " + vecName + " = call <9 x float> @dx.op.triangleObjectPosition.f32(i32 -2147483641) ; [amd-mg] TriangleObjectPositions()";
                    needTriPosDecl = true;
                } else if (opcode == 97 || opcode == 98) {
                    std::string rqArg = indexToSrc0Arg.count(idxVar) ? indexToSrc0Arg[idxVar] : "0";
                    if (opcode == 97) {
                        lines[i] = "  " + vecName + " = call <9 x float> @dx.op.rayQuery_CandidateTriangleObjectPosition.f32(i32 -2147483640, i32 " + rqArg + ")";
                        needRqCandidateTriPosDecl = true;
                    } else {
                        lines[i] = "  " + vecName + " = call <9 x float> @dx.op.rayQuery_CommittedTriangleObjectPosition.f32(i32 -2147483639, i32 " + rqArg + ")";
                        needRqCommittedTriPosDecl = true;
                    }
                } else if (opcode == 99) {
                    std::string hoArg = indexToSrc0Arg.count(idxVar) ? indexToSrc0Arg[idxVar] : "undef";
                    lines[i] = "  " + vecName + " = call <9 x float> @dx.op.hitObject_TriangleObjectPosition.f32(i32 -2147483638, %dx.types.HitObject " + hoArg + ")";
                    needHoTriPosDecl = true;
                }

                int compIdx = 0;
                for (size_t j = i + 1; j < fnEnd && compIdx < 9; ++j) {
                    std::smatch mc;
                    if (std::regex_search(lines[j], mc, reCounter) && isNvHandleVar[mc[2].str()]) {
                        std::string outVar = mc[1].str();
                        std::string fltVar = "%amd_mg_flt_" + std::to_string(i) + "_" + std::to_string(compIdx);
                        lines[j] = "  " + fltVar + " = extractelement <9 x float> " + vecName + ", i32 " + std::to_string(compIdx) + "\n" +
                                   "  " + outVar + " = bitcast float " + fltVar + " to i32";
                        compIdx++;
                    }
                }
                outPatchCount++;
            } else if (opcode == 19) {
                // NV_EXTN_OP_GET_SPECIAL: sub-opcode comes from the src0u store (offset 76), result
                // is the next bufferUpdateCounter on the extension UAV.
                const std::string subOpText = indexToSrc0Arg.count(idxVar) ? indexToSrc0Arg[idxVar] : "";
                const int subOp = subOpText.empty() || subOpText[0] == '%' ? -1 : std::atoi(subOpText.c_str());
                for (size_t j = i + 1; j < fnEnd; ++j) {
                    std::smatch mc;
                    if (!std::regex_search(lines[j], mc, reCounter) || !isNvHandleVar[mc[2].str()]) {
                        continue;
                    }
                    const std::string outVar = mc[1].str();
                    const std::string lane   = "%amd_mg_lane_" + std::to_string(i);
                    lines[i] = "  ; [amd-mg] removed rawBufferStore opcode 19 (GET_SPECIAL " + subOpText + ")";
                    if (subOp == 4) {
                        // NV_SPECIALOP_THREADLTMASK: bits of all lanes below the current one.
                        lines[j] = "  " + lane + " = call i32 @dx.op.waveGetLaneIndex(i32 111) ; [amd-mg] WaveGetLaneIndex()\n" +
                                   "  " + lane + "_m = and i32 " + lane + ", 31\n" +
                                   "  " + lane + "_b = shl i32 1, " + lane + "_m\n" +
                                   "  " + outVar + " = add i32 " + lane + "_b, -1 ; [amd-mg] NvGetSpecial(THREADLTMASK)";
                        needWaveLaneIndexDecl = true;
                    } else if (subOp == 9 || subOp == 10) {
                        // NV_SPECIALOP_GLOBAL_TIMER_LO/HI: DXIL has no clock op. The value must NOT be a
                        // constant: amdxc64.dll 32.0.32012.1013 crashes (AV in CreateStateObject) on the
                        // RTXMG raygen "timeview" code once both timer reads fold to a constant.
                        lines[j] = "  " + outVar + " = call i32 @dx.op.waveGetLaneIndex(i32 111) ; [amd-mg] NvGetSpecial(GLOBAL_TIMER) placeholder";
                        needWaveLaneIndexDecl = true;
                    } else {
                        lines[j] = "  " + outVar + " = add i32 0, 0 ; [amd-mg] unsupported NvGetSpecial sub-op " + subOpText;
                        unsupportedOps.insert("19 (GET_SPECIAL sub-op " + subOpText + ")");
                    }
                    outPatchCount++;
                    break;
                }
            } else {
                unsupportedOps.insert(std::to_string(opcode));
            }
        }

        // Pass 2e: Safety sweep in case any stray bufferUpdateCounter or rawBufferStore on nvHandle remains
        for (size_t i = fnStart; i < fnEnd; ++i) {
            std::smatch mc;
            if (std::regex_search(lines[i], mc, reCounter) && isNvHandleVar[mc[2].str()]) {
                lines[i] = "  " + mc[1].str() + " = add i32 0, 0 ; [amd-mg] neutralized stray nvHandle counter";
            } else if (std::regex_search(lines[i], mc, reStoreOp) && isNvHandleVar[mc[1].str()]) {
                lines[i] = "  ; [amd-mg] removed stray nvHandle store";
            }
        }

        fnStart = fnEnd + 1;
    }

    if (!unsupportedOps.empty()) {
        std::string list;
        for (const auto& op : unsupportedOps) {
            list += (list.empty() ? "" : ", ") + op;
        }
        Log("[DXIL] WARNING: unsupported NvAPI shader extension opcode(s) neutralized to 0: %s", list.c_str());
    }

    if (outPatchCount == 0) {
        return false;
    }

    // Module-level shader flags live in the properties of the null entry point
    // (`!dx.entryPoints = !{!E, ...}`, `!E = !{null, !"", null, !R, !P}`, `!P = !{i32 0, i64 <flags>, ...}`).
    // New wave intrinsics require the WaveOps bit there, or validation fails with "Flags must match usage".
    if (needWaveLaneIndexDecl) {
        constexpr uint64_t kShaderFlagWaveOps = 0x80000;
        std::string entryNode;
        for (const auto& l : lines) {
            if (l.rfind("!dx.entryPoints = !{", 0) == 0) {
                size_t b = l.find('!', 19);
                size_t e = l.find_first_of(",}", b);
                if (b != std::string::npos && e != std::string::npos) {
                    entryNode = l.substr(b, e - b);
                }
                break;
            }
        }
        std::string propsNode;
        if (!entryNode.empty()) {
            const std::string prefix = entryNode + " = !{null, ";
            for (const auto& l : lines) {
                if (l.rfind(prefix, 0) == 0) {
                    size_t b = l.rfind('!');
                    size_t e = l.find('}', b);
                    if (b != std::string::npos && e != std::string::npos && b > prefix.size() &&
                        l.compare(l.size() - 5, 5, "null}") != 0) {
                        propsNode = l.substr(b, e - b);
                    }
                    break;
                }
            }
        }
        bool flagsUpdated = false;
        if (!propsNode.empty()) {
            const std::string prefix = propsNode + " = !{i32 0, i64 ";
            for (auto& l : lines) {
                if (l.rfind(prefix, 0) == 0) {
                    size_t numEnd = l.find_first_of(",}", prefix.size());
                    uint64_t flags = std::strtoull(l.substr(prefix.size(), numEnd - prefix.size()).c_str(), nullptr, 10);
                    l = prefix + std::to_string(flags | kShaderFlagWaveOps) + l.substr(numEnd);
                    flagsUpdated = true;
                    break;
                }
            }
        }
        if (!flagsUpdated) {
            Log("[DXIL] Could not locate module shader flags to add WaveOps; validation may fail");
        }
    }

    // 3. Neutralize all now-unused handle setup lines (`%1 = load ...`, `%2 = createHandleForLib`, `%3 = annotateHandle`)
    // preserving sequential SSA numbering (`%N = add i32 0, 0`) so LLVM IR parser stays 100% happy!
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lineIsNvHandleSetup[i]) {
            std::smatch m;
            if (std::regex_search(lines[i], m, reSsaAssign)) {
                lines[i] = "  " + m[1].str() + " = add i32 0, 0 ; [amd-mg] neutralized g_NvidiaExt handle";
            }
        }
    }

    // Check if any legitimate bufferUpdateCounter, rawBufferStore, annotateHandle, or createHandleForLib calls remain
    bool anyRemainingCounterCalls    = false;
    bool anyRemainingStoreCalls      = false;
    bool anyRemainingAnnotateCalls   = false;
    bool anyRemainingCreateLibCalls  = false;
    std::string dxVersionNode        = "!1";
    for (const auto& l : lines) {
        if (l.find("call i32 @dx.op.bufferUpdateCounter(") != std::string::npos) {
            anyRemainingCounterCalls = true;
        }
        if (l.find("call void @dx.op.rawBufferStore.i32(") != std::string::npos) {
            anyRemainingStoreCalls = true;
        }
        if (l.find("call %dx.types.Handle @dx.op.annotateHandle(") != std::string::npos) {
            anyRemainingAnnotateCalls = true;
        }
        if (l.find("call %dx.types.Handle @dx.op.createHandleForLib.") != std::string::npos) {
            anyRemainingCreateLibCalls = true;
        }
        if (l.rfind("!dx.version = !{!", 0) == 0) {
            size_t b = l.find("!{!");
            size_t e = l.find("}", b);
            if (b != std::string::npos && e != std::string::npos) {
                dxVersionNode = l.substr(b + 2, e - (b + 2));
            }
        }
    }

    // 4. Insert SM 6.10 intrinsic declarations and upgrade shaderModel / version metadata to 6.10 (1.10)
    std::ostringstream oss;
    bool insertedDecls = false;
    std::string dxVerPrefix = dxVersionNode + " = !{i32 1, i32 ";
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string& l = lines[i];

        // Comment out unused declarations of bufferUpdateCounter / rawBufferStore / handle ops if no longer called
        if (!anyRemainingCounterCalls && l.find("declare i32 @dx.op.bufferUpdateCounter(") != std::string::npos) {
            oss << "; [amd-mg] " << l << "\n";
            continue;
        }
        if (!anyRemainingStoreCalls && l.find("declare void @dx.op.rawBufferStore.i32(") != std::string::npos) {
            oss << "; [amd-mg] " << l << "\n";
            continue;
        }
        if (!anyRemainingAnnotateCalls && l.find("declare %dx.types.Handle @dx.op.annotateHandle(") != std::string::npos) {
            oss << "; [amd-mg] " << l << "\n";
            continue;
        }
        if (!anyRemainingCreateLibCalls && l.find("declare %dx.types.Handle @dx.op.createHandleForLib.") != std::string::npos) {
            oss << "; [amd-mg] " << l << "\n";
            continue;
        }

        if (!insertedDecls && l.find("attributes #0") == 0) {
            if (needClusterIdDecl) {
                oss << "declare i32 @dx.op.clusterID(i32) nounwind readnone\n";
            }
            if (needTriPosDecl) {
                oss << "declare <9 x float> @dx.op.triangleObjectPosition.f32(i32) nounwind readnone\n";
            }
            if (needRqStateScalarDecl) {
                oss << "declare i32 @dx.op.rayQuery_StateScalar.i32(i32, i32) nounwind readonly\n";
            }
            if (needRqCandidateTriPosDecl) {
                oss << "declare <9 x float> @dx.op.rayQuery_CandidateTriangleObjectPosition.f32(i32, i32) nounwind readonly\n";
            }
            if (needRqCommittedTriPosDecl) {
                oss << "declare <9 x float> @dx.op.rayQuery_CommittedTriangleObjectPosition.f32(i32, i32) nounwind readonly\n";
            }
            if (needHoStateScalarDecl) {
                oss << "declare i32 @dx.op.hitObject_StateScalar.i32(i32, %dx.types.HitObject) nounwind readnone\n";
            }
            if (needHoTriPosDecl) {
                oss << "declare <9 x float> @dx.op.hitObject_TriangleObjectPosition.f32(i32, %dx.types.HitObject) nounwind readnone\n";
            }
            if (needWaveLaneIndexDecl && irText.find("declare i32 @dx.op.waveGetLaneIndex(") == std::string::npos) {
                oss << "declare i32 @dx.op.waveGetLaneIndex(i32) nounwind readonly\n";
            }
            oss << "\n";
            insertedDecls = true;
        }

        // Upgrade ONLY the !dx.version metadata node (e.g. !1 = !{i32 1, i32 6} -> !1 = !{i32 1, i32 10})
        // Never touch StructuredBuffer stride nodes (which also have the form !N = !{i32 1, i32 <stride>})!
        if (l.rfind(dxVerPrefix, 0) == 0) {
            l = dxVersionNode + " = !{i32 1, i32 10}";
        }
        // Upgrade !dx.shaderModel = !{!"lib", i32 6, i32 X} -> !{!"lib", i32 6, i32 10}
        if (l.find("= !{!\"") != std::string::npos && l.find(", i32 6, i32 ") != std::string::npos) {
            size_t smPos = l.find(", i32 6, i32 ");
            l = l.substr(0, smPos) + ", i32 6, i32 10}";
        }

        oss << l << "\n";
    }

    outIrText = oss.str();
    return true;
}

// ============================================================================
// PatchDxilClusterShader: Disassembles DXIL -> Transforms IR -> Reassembles & Signs
// ============================================================================
bool PatchDxilClusterShader(
    const void* pBytecode,
    size_t bytecodeLength,
    uint32_t uavSlot,
    uint32_t uavSpace,
    std::vector<uint8_t>& outPatchedBytecode)
{
    if (!pBytecode || bytecodeLength < 32) {
        return false;
    }
    if (!InitializeDxilPatcher()) {
        return false;
    }

    ComPtr<IDxcCompiler3> pCompiler;
    ComPtr<IDxcUtils>     pUtils;
    ComPtr<IDxcAssembler> pAssembler;

    if (FAILED(g_pfnDxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&pCompiler))) ||
        FAILED(g_pfnDxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&pUtils))) ||
        FAILED(g_pfnDxcCreateInstance(CLSID_DxcAssembler, IID_PPV_ARGS(&pAssembler)))) {
        Log("[DXIL] Failed to create DXC interfaces");
        return false;
    }

    // If the DXBC container includes an "ILDB" (debug DXIL) chunk, IDxcCompiler3::Disassemble
    // will disassemble the unstripped debug IR (containing @llvm.dbg.value / !DICompileUnit)
    // instead of the runtime "DXIL" chunk. Rename "ILDB" -> "ILDX" in a temporary copy first.
    std::vector<uint8_t> cleanContainer;
    const uint8_t* bytes = static_cast<const uint8_t*>(pBytecode);
    if (bytecodeLength >= 36 &&
        bytes[0] == 'D' && bytes[1] == 'X' && bytes[2] == 'B' && bytes[3] == 'C') {
        uint32_t numChunks = 0;
        std::memcpy(&numChunks, bytes + 28, sizeof(uint32_t));
        if (numChunks > 0 && numChunks < 64 && 32 + numChunks * 4 <= bytecodeLength) {
            for (uint32_t c = 0; c < numChunks; ++c) {
                uint32_t off = 0;
                std::memcpy(&off, bytes + 32 + c * 4, sizeof(uint32_t));
                if (off + 8 <= bytecodeLength &&
                    bytes[off] == 'I' && bytes[off + 1] == 'L' &&
                    bytes[off + 2] == 'D' && bytes[off + 3] == 'B') {
                    if (cleanContainer.empty()) {
                        cleanContainer.assign(bytes, bytes + bytecodeLength);
                    }
                    cleanContainer[off + 3] = 'X';
                }
            }
        }
    }

    DxcBuffer srcBuf{};
    srcBuf.Ptr      = cleanContainer.empty() ? pBytecode : cleanContainer.data();
    srcBuf.Size     = bytecodeLength;
    srcBuf.Encoding = 0;

    ComPtr<IDxcResult> pDisasmResult;
    if (FAILED(pCompiler->Disassemble(&srcBuf, IID_PPV_ARGS(&pDisasmResult))) || !pDisasmResult) {
        return false;
    }

    ComPtr<IDxcBlobEncoding> pDisasmBlob;
    if (FAILED(pDisasmResult->GetOutput(DXC_OUT_DISASSEMBLY, IID_PPV_ARGS(&pDisasmBlob), nullptr)) || !pDisasmBlob) {
        return false;
    }

    std::string irText(
         static_cast<const char*>(pDisasmBlob->GetBufferPointer()),
        pDisasmBlob->GetBufferSize());
    while (!irText.empty() && irText.back() == '\0') {
        irText.pop_back();
    }

    std::string patchedIr;
    uint32_t patchCount = 0;
    if (!TransformDxilIrText(irText, uavSlot, uavSpace, patchedIr, patchCount)) {
        return false;
    }

    Log("[DXIL] Patched %u NvAPI cluster extension call(s) (u%u, space%u) -> SM 6.10 DXIL",
        patchCount, uavSlot, uavSpace);

    ComPtr<IDxcBlobEncoding> pPatchedIrBlob;
    if (FAILED(pUtils->CreateBlobFromPinned(
            patchedIr.data(),
            static_cast<uint32_t>(patchedIr.size()),
            DXC_CP_UTF8,
            &pPatchedIrBlob))) {
        return false;
    }

    ComPtr<IDxcOperationResult> pAsmResult;
    if (FAILED(pAssembler->AssembleToContainer(pPatchedIrBlob.Get(), &pAsmResult)) || !pAsmResult) {
        Log("[DXIL] AssembleToContainer call failed");
        return false;
    }

    HRESULT hrStatus = S_OK;
    pAsmResult->GetStatus(&hrStatus);
    if (FAILED(hrStatus)) {
        ComPtr<IDxcBlobEncoding> pErr;
        if (SUCCEEDED(pAsmResult->GetErrorBuffer(&pErr)) && pErr && pErr->GetBufferSize() > 0) {
            Log("[DXIL] AssembleToContainer error: %.*s",
                static_cast<int>(pErr->GetBufferSize()),
                static_cast<const char*>(pErr->GetBufferPointer()));
        }
        return false;
    }

    ComPtr<IDxcBlob> pContainerBlob;
    if (FAILED(pAsmResult->GetResult(&pContainerBlob)) || !pContainerBlob || pContainerBlob->GetBufferSize() == 0) {
        return false;
    }

    outPatchedBytecode.resize(pContainerBlob->GetBufferSize());
    std::memcpy(outPatchedBytecode.data(), pContainerBlob->GetBufferPointer(), pContainerBlob->GetBufferSize());

    // Optional validation / signing via dxil.dll
    if (g_pfnDxilCreateInstance) {
        ComPtr<IDxcValidator> pValidator;
        if (SUCCEEDED(g_pfnDxilCreateInstance(CLSID_DxcValidator, IID_PPV_ARGS(&pValidator))) && pValidator) {
            ComPtr<IDxcBlobEncoding> pInPlaceBlob;
            if (SUCCEEDED(pUtils->CreateBlobFromPinned(
                    outPatchedBytecode.data(),
                    static_cast<uint32_t>(outPatchedBytecode.size()),
                    DXC_CP_ACP,
                    &pInPlaceBlob))) {
                ComPtr<IDxcOperationResult> pValRes;
                pValidator->Validate(pInPlaceBlob.Get(), DxcValidatorFlags_InPlaceEdit, &pValRes);
                if (pValRes) {
                    HRESULT hrVal = S_OK;
                    pValRes->GetStatus(&hrVal);
                    if (FAILED(hrVal)) {
                        ComPtr<IDxcBlobEncoding> pValErr;
                        if (SUCCEEDED(pValRes->GetErrorBuffer(&pValErr)) && pValErr && pValErr->GetBufferSize() > 0) {
                            Log("[DXIL] Validator status=0x%08X: %.*s",
                                static_cast<unsigned>(hrVal),
                                static_cast<int>(pValErr->GetBufferSize()),
                                static_cast<const char*>(pValErr->GetBufferPointer()));
                        }
                        // An invalid container must never reach the driver: keep the original shader.
                        outPatchedBytecode.clear();
                        return false;
                    } else {
                        Log("[DXIL] Validator succeeded (0x%08X)", static_cast<unsigned>(hrVal));
                    }
                }
            }
        }
    }

    if (const char* dumpEnv = std::getenv("AMD_MG_DUMP_DXIL")) {
        if (std::atoi(dumpEnv) != 0) {
            std::ofstream ofs("patched_cluster_shader.dxil", std::ios::binary);
            ofs.write(reinterpret_cast<const char*>(outPatchedBytecode.data()), outPatchedBytecode.size());
        }
    }

    return true;
}

} // namespace amd_mg
