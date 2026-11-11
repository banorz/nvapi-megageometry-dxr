#include "amd_mg.h"

namespace {

HMODULE GetSystemWinmm() {
    static HMODULE s_winmm = nullptr;
    if (!s_winmm) {
        char sysDir[MAX_PATH] = {};
        if (GetSystemDirectoryA(sysDir, MAX_PATH) > 0) {
            std::string path = std::string(sysDir) + "\\winmm.dll";
            s_winmm = LoadLibraryA(path.c_str());
        }
    }
    return s_winmm;
}

using PFN_WinmmGeneric = uint64_t(__stdcall*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

uint64_t ForwardWinmm(const char* name, uint64_t a = 0, uint64_t b = 0, uint64_t c = 0, uint64_t d = 0, uint64_t e = 0, uint64_t f = 0) {
    if (HMODULE mod = GetSystemWinmm()) {
        if (auto fn = reinterpret_cast<PFN_WinmmGeneric>(GetProcAddress(mod, name))) {
            return fn(a, b, c, d, e, f);
        }
    }
    return 0;
}

// ----------------------------------------------------------------------------
// Diagnostics: AMD_MG_TRACE_CXX=1 logs every first-chance C++ exception (any module) with its
// std::exception::what() and a module+offset call stack. Used to find who throws inside
// third-party code (e.g. Streamline's slSetD3DDevice "device or resource busy").
// ----------------------------------------------------------------------------
constexpr DWORD kMsvcCxxException = 0xE06D7363;
LONG g_cxxTraceCount = 0;

const char* SafeWhat(ULONG_PTR exceptionObject) {
    __try {
        // MSVC std::exception layout: vptr, __std_exception_data { const char* _What; bool _DoFree; }
        const char* what = *reinterpret_cast<const char* const*>(exceptionObject + sizeof(void*));
        if (what && !IsBadStringPtrA(what, 256)) {
            return what;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return "?";
}

void DescribeAddress(void* addr, char* out, size_t outSize) {
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod) && mod) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(mod, path, MAX_PATH);
        const char* name = std::strrchr(path, '\\');
        std::snprintf(out, outSize, "%s+0x%llx", name ? name + 1 : path,
                      static_cast<unsigned long long>(static_cast<uint8_t*>(addr) - reinterpret_cast<uint8_t*>(mod)));
    } else {
        std::snprintf(out, outSize, "%p", addr);
    }
}

LONG CALLBACK CxxTraceHandler(PEXCEPTION_POINTERS info) {
    const EXCEPTION_RECORD* rec = info ? info->ExceptionRecord : nullptr;
    if (!rec) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const bool isCxx = rec->ExceptionCode == kMsvcCxxException && rec->NumberParameters >= 3;
    const bool isAv  = rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION ||
                       rec->ExceptionCode == EXCEPTION_ILLEGAL_INSTRUCTION ||
                       rec->ExceptionCode == STATUS_STACK_BUFFER_OVERRUN;
    if (!isCxx && !isAv) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (InterlockedIncrement(&g_cxxTraceCount) > 20) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    void* frames[48] = {};
    USHORT n = CaptureStackBackTrace(1, 48, frames, nullptr);
    std::string stack;
    char buf[160];
    for (USHORT i = 0; i < n; ++i) {
        DescribeAddress(frames[i], buf, sizeof(buf));
        stack += "\n    #";
        stack += std::to_string(i);
        stack += ' ';
        stack += buf;
    }
    if (isAv) {
        char where[160];
        DescribeAddress(rec->ExceptionAddress, where, sizeof(where));
        const unsigned long long target = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
        const unsigned long long op     = rec->NumberParameters >= 1 ? rec->ExceptionInformation[0] : 0;
        amd_mg::Log("[SEH] tid=%lu exception 0x%08lX at %s (op=%llu addr=0x%llx)%s",
                    GetCurrentThreadId(), rec->ExceptionCode, where, op, target, stack.c_str());
    } else {
        amd_mg::Log("[CXX] tid=%lu C++ exception what=\"%s\"%s",
                    GetCurrentThreadId(), SafeWhat(rec->ExceptionInformation[1]), stack.c_str());
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// amd_megageometry.ini next to this module: one AMD_MG_*=value per line ('#' / ';' comments).
// Lets a game launched from a store client get options without system-wide environment
// variables. Values already present in the process environment win. Returns the log lines.
std::vector<std::string> LoadIniEnvironment(HMODULE self) {
    std::vector<std::string> notes;
    char path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameA(self, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return notes;
    std::string ini(path, n);
    const size_t slash = ini.find_last_of("\\/");
    ini = (slash == std::string::npos ? std::string() : ini.substr(0, slash + 1)) + "amd_megageometry.ini";
    FILE* f = nullptr;
    if (fopen_s(&f, ini.c_str(), "rb") != 0 || !f) return notes;
    notes.push_back("[Config] reading " + ini);
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        std::string l(line);
        auto trim = [](std::string& t) {
            const size_t b = t.find_first_not_of(" \t\r\n");
            const size_t e = t.find_last_not_of(" \t\r\n");
            t = b == std::string::npos ? std::string() : t.substr(b, e - b + 1);
        };
        trim(l);
        if (l.empty() || l[0] == '#' || l[0] == ';' || l[0] == '[') continue;
        const size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string key = l.substr(0, eq), value = l.substr(eq + 1);
        trim(key);
        trim(value);
        if (key.rfind("AMD_MG_", 0) != 0) {
            notes.push_back("[Config]   ignored " + key + " (only AMD_MG_* keys)");
            continue;
        }
        char existing[8];
        if (GetEnvironmentVariableA(key.c_str(), existing, sizeof(existing)) > 0) {
            notes.push_back("[Config]   " + key + " already set in the environment, ini value ignored");
            continue;
        }
        SetEnvironmentVariableA(key.c_str(), value.c_str());
        _putenv_s(key.c_str(), value.c_str());
        notes.push_back("[Config]   " + key + "=" + value);
    }
    std::fclose(f);
    return notes;
}

void InstallCxxTraceIfRequested() {
    const char* env = std::getenv("AMD_MG_TRACE_CXX");
    if (env && std::atoi(env) != 0) {
        AddVectoredExceptionHandler(1, &CxxTraceHandler);
        amd_mg::Log("[DLL] AMD_MG_TRACE_CXX: logging first-chance C++ exceptions");
    }
}

} // namespace

// Export WINMM.dll entrypoints so nvapi64.dll can also be placed as winmm.dll next to an executable
extern "C" {
__declspec(dllexport) uint64_t __stdcall midiOutGetDevCapsA(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f) { return ForwardWinmm("midiOutGetDevCapsA", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiInStart(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)        { return ForwardWinmm("midiInStart", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiInClose(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)        { return ForwardWinmm("midiInClose", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiInOpen(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)         { return ForwardWinmm("midiInOpen", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiOutOpen(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)        { return ForwardWinmm("midiOutOpen", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiOutShortMsg(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)    { return ForwardWinmm("midiOutShortMsg", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiInGetDevCapsA(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)  { return ForwardWinmm("midiInGetDevCapsA", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall midiOutGetNumDevs(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)  { return ForwardWinmm("midiOutGetNumDevs", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall timeBeginPeriod(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)    { return ForwardWinmm("timeBeginPeriod", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall timeEndPeriod(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)      { return ForwardWinmm("timeEndPeriod", a, b, c, d, e, f); }
__declspec(dllexport) uint64_t __stdcall timeGetTime(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f)        { return ForwardWinmm("timeGetTime", a, b, c, d, e, f); }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved) {
    switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        {
            const std::vector<std::string> iniNotes = LoadIniEnvironment(hModule);
            amd_mg::LogInit();
            amd_mg::Log("[DLL] amd-megageometry attached (hModule=%p)", hModule);
            for (const auto& note : iniNotes) amd_mg::Log("%s", note.c_str());
        }
        InstallCxxTraceIfRequested();
        amd_mg::InstallProcessIatHooks(hModule);
        break;
    case DLL_PROCESS_DETACH:
        // lpReserved != nullptr: the process is exiting and other DLLs may already be gone.
        amd_mg::ShutdownGpuConverter(lpReserved != nullptr);
        amd_mg::ShutdownDxilPatcher();
        amd_mg::Log("[DLL] amd-megageometry detached");
        break;
    }
    return TRUE;
}
