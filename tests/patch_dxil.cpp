// ============================================================================
// patch_dxil.exe <in.dxil> <out.dxil> [--slot=N] [--space=N]
//
// Runs the wrapper's DXIL patcher (src\dxil_patcher.cpp) on a DXIL container outside any game:
// NvAPI shader extension intrinsics -> SM 6.10, then reassembles and validates with dxil.dll.
// The result can be compiled by the driver with repro_rtpso.exe. Honors AMD_MG_TIMER_CONST.
// Defaults: --slot=1000 --space=0 (the values test_wrapper uses for the RTXMG path tracer).
// Needs dxcompiler.dll and dxil.dll next to the exe (build.ps1 copies them to build\).
// ============================================================================
#include "amd_mg.h"

#include <cstdarg>
#include <fstream>

namespace amd_mg {

bool PatchDxilClusterShader(const void* pBytecode, size_t bytecodeLength, uint32_t uavSlot,
                            uint32_t uavSpace, std::vector<uint8_t>& outPatchedBytecode);

// The patcher logs through amd_mg::Log; here it goes to the console.
void LogInit() {}
void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vprintf(fmt, args);
    va_end(args);
    std::printf("\n");
}

} // namespace amd_mg

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: patch_dxil <in.dxil> <out.dxil> [--slot=N] [--space=N]\n");
        return 2;
    }
    uint32_t slot = 1000, space = 0;
    for (int a = 3; a < argc; ++a) {
        if (std::strncmp(argv[a], "--slot=", 7) == 0) {
            slot = static_cast<uint32_t>(std::strtoul(argv[a] + 7, nullptr, 10));
        } else if (std::strncmp(argv[a], "--space=", 8) == 0) {
            space = static_cast<uint32_t>(std::strtoul(argv[a] + 8, nullptr, 10));
        } else {
            std::printf("unknown option %s\n", argv[a]);
            return 2;
        }
    }

    std::ifstream in(argv[1], std::ios::binary | std::ios::ate);
    if (!in) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::vector<uint8_t> original(static_cast<size_t>(in.tellg()));
    in.seekg(0);
    in.read(reinterpret_cast<char*>(original.data()), static_cast<std::streamsize>(original.size()));

    std::vector<uint8_t> patched;
    if (!amd_mg::PatchDxilClusterShader(original.data(), original.size(), slot, space, patched) || patched.empty()) {
        std::printf("RESULT: patch FAILED (see the [DXIL] lines above)\n");
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(reinterpret_cast<const char*>(patched.data()), static_cast<std::streamsize>(patched.size()));
    if (!out) {
        std::printf("cannot write %s\n", argv[2]);
        return 2;
    }
    std::printf("RESULT: patched %zu -> %zu bytes, written %s\n", original.size(), patched.size(), argv[2]);
    return 0;
}
