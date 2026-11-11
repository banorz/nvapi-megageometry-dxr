// dxil_asm.exe <in.ll> <out.dxil>
//
// Assembles DXIL LLVM IR text (e.g. the output of `dxc -dumpbin`) into a validated, signed
// DXIL container with the same IDxcAssembler + IDxcValidator path used by the wrapper's
// DXIL patcher. Used to bisect driver crashes by hand-editing the patched IR.
// Run from build\ (loads dxcompiler.dll and dxil.dll from the current directory).

#include <windows.h>
#include <wrl/client.h>
#include <dxcapi.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: dxil_asm <in.ll> <out.dxil>\n");
        return 2;
    }
    std::ifstream ifs(argv[1], std::ios::binary);
    if (!ifs) {
        std::printf("cannot open %s\n", argv[1]);
        return 2;
    }
    std::stringstream ss;
    ss << ifs.rdbuf();
    std::string ir = ss.str();

    HMODULE hDxc = LoadLibraryW(L"dxcompiler.dll");
    HMODULE hDxil = LoadLibraryW(L"dxil.dll");
    auto createDxc = reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(hDxc, "DxcCreateInstance"));
    auto createDxil = hDxil ? reinterpret_cast<DxcCreateInstanceProc>(GetProcAddress(hDxil, "DxcCreateInstance")) : nullptr;
    if (!createDxc) {
        std::printf("dxcompiler.dll not found\n");
        return 2;
    }

    ComPtr<IDxcUtils> utils;
    ComPtr<IDxcAssembler> assembler;
    createDxc(CLSID_DxcUtils, IID_PPV_ARGS(&utils));
    createDxc(CLSID_DxcAssembler, IID_PPV_ARGS(&assembler));

    ComPtr<IDxcBlobEncoding> irBlob;
    utils->CreateBlobFromPinned(ir.data(), static_cast<UINT32>(ir.size()), DXC_CP_UTF8, &irBlob);
    ComPtr<IDxcOperationResult> asmRes;
    assembler->AssembleToContainer(irBlob.Get(), &asmRes);
    HRESULT st = E_FAIL;
    if (asmRes) {
        asmRes->GetStatus(&st);
    }
    if (FAILED(st)) {
        ComPtr<IDxcBlobEncoding> err;
        if (asmRes && SUCCEEDED(asmRes->GetErrorBuffer(&err)) && err) {
            std::printf("assemble error:\n%.*s\n", static_cast<int>(err->GetBufferSize()), static_cast<const char*>(err->GetBufferPointer()));
        }
        return 1;
    }
    ComPtr<IDxcBlob> container;
    asmRes->GetResult(&container);
    std::vector<char> out(static_cast<const char*>(container->GetBufferPointer()),
                          static_cast<const char*>(container->GetBufferPointer()) + container->GetBufferSize());

    if (createDxil) {
        ComPtr<IDxcValidator> validator;
        createDxil(CLSID_DxcValidator, IID_PPV_ARGS(&validator));
        ComPtr<IDxcBlobEncoding> inPlace;
        utils->CreateBlobFromPinned(out.data(), static_cast<UINT32>(out.size()), DXC_CP_ACP, &inPlace);
        ComPtr<IDxcOperationResult> valRes;
        validator->Validate(inPlace.Get(), DxcValidatorFlags_InPlaceEdit, &valRes);
        HRESULT vst = E_FAIL;
        if (valRes) {
            valRes->GetStatus(&vst);
        }
        if (FAILED(vst)) {
            ComPtr<IDxcBlobEncoding> err;
            if (valRes && SUCCEEDED(valRes->GetErrorBuffer(&err)) && err) {
                std::printf("validation error:\n%.*s\n", static_cast<int>(err->GetBufferSize()), static_cast<const char*>(err->GetBufferPointer()));
            }
            return 1;
        }
    }

    std::ofstream ofs(argv[2], std::ios::binary);
    ofs.write(out.data(), out.size());
    std::printf("wrote %s (%zu bytes, validated)\n", argv[2], out.size());
    return 0;
}
