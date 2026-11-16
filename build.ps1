$ErrorActionPreference = "Stop"

$Root = $PSScriptRoot
$BuildDir = Join-Path $Root "build"
$D3D12Dir = Join-Path $BuildDir "D3D12"
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
New-Item -ItemType Directory -Force -Path $D3D12Dir | Out-Null

$DxcExe = Join-Path $Root "sdk\dxc\extracted\bin\x64\dxc.exe"
$AgilityInc = Join-Path $Root "sdk\agility\extracted\build\native\include"
$DxcInc = Join-Path $Root "sdk\dxc\extracted\inc"
$NvapiInc = Join-Path $Root "sdk\nvapi\repo"
foreach ($p in @($DxcExe, $AgilityInc, $DxcInc, (Join-Path $NvapiInc "nvapi.h"))) {
    if (-not (Test-Path $p)) { throw "Missing dependency: $p (see README, 'Dependencies')" }
}

Write-Host "[1/4] Compiling GPU Indirect Argument Converter Compute Shader..." -ForegroundColor Cyan
& $DxcExe -T cs_6_0 -E CS_ConvertArgs -Vn g_CS_ConvertArgs -Fh "$BuildDir\arg_converter_cs.h" "$Root\shaders\arg_converter.hlsl"
if ($LASTEXITCODE -ne 0) { throw "Failed to compile arg_converter.hlsl" }

Write-Host "[2/4] Compiling NvAPI Test Raytracing Library Shader..." -ForegroundColor Cyan
& $DxcExe -T lib_6_6 -I "$NvapiInc" -Fo "$BuildDir\test_nv_shader.dxil" "$Root\tests\test_nv_shader.hlsl"
if ($LASTEXITCODE -ne 0) { throw "Failed to compile test_nv_shader.hlsl" }

Write-Host "[3/4] Copying Agility SDK 1.722 & DXC runtime DLLs to build/..." -ForegroundColor Cyan
Copy-Item "$Root\sdk\agility\extracted\build\native\bin\x64\*.dll" "$D3D12Dir\" -Force
Copy-Item "$Root\sdk\dxc\extracted\bin\x64\dxcompiler.dll" "$BuildDir\" -Force
Copy-Item "$Root\sdk\dxc\extracted\bin\x64\dxil.dll" "$BuildDir\" -Force

Write-Host "[4/4] Building nvapi64.dll and test_wrapper.exe with MSVC..." -ForegroundColor Cyan
# Visual Studio with the C++ x64 tools: $env:VSDEVCMD overrides, otherwise the newest install found by vswhere.
$VsDevCmd = $env:VSDEVCMD
if (-not $VsDevCmd) {
    $VsWhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $VsWhere) {
        $VsPath = & $VsWhere -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1
        if ($VsPath) { $VsDevCmd = Join-Path $VsPath "Common7\Tools\VsDevCmd.bat" }
    }
}
if (-not $VsDevCmd -or -not (Test-Path $VsDevCmd)) { throw "VsDevCmd.bat not found: install Visual Studio with 'Desktop development with C++' or set VSDEVCMD" }
$ClFlags = "/nologo /std:c++20 /EHsc /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /I`"$AgilityInc`" /I`"$DxcInc`" /I`"$NvapiInc`" /I`"$Root\include`" /I`"$BuildDir`""

$BuildCmd = "`"$VsDevCmd`" -arch=x64 -no_logo && " +
    "cl $ClFlags /LD `"$Root\src\dllmain.cpp`" `"$Root\src\nvapi_proxy.cpp`" `"$Root\src\d3d12_hook.cpp`" `"$Root\src\dxil_patcher.cpp`" `"$Root\src\gpu_arg_converter.cpp`" `"$Root\src\cmdlist_state.cpp`" `"$Root\src\dred.cpp`" /Fe:`"$BuildDir\nvapi64.dll`" /Fo:`"$BuildDir\\`" d3d12.lib dxgi.lib dxguid.lib advapi32.lib && " +
    "cl $ClFlags `"$Root\tests\test_wrapper.cpp`" `"$Root\src\d3d12_hook.cpp`" `"$Root\src\dxil_patcher.cpp`" `"$Root\src\gpu_arg_converter.cpp`" `"$Root\src\cmdlist_state.cpp`" `"$Root\src\dred.cpp`" /Fe:`"$BuildDir\test_wrapper.exe`" /Fo:`"$BuildDir\\`" d3d12.lib dxgi.lib dxguid.lib advapi32.lib && " +
    "cl $ClFlags `"$Root\tests\repro_templates.cpp`" /Fe:`"$BuildDir\repro_templates.exe`" /Fo:`"$BuildDir\\`" d3d12.lib dxgi.lib dxguid.lib && " +
    "cl $ClFlags `"$Root\tests\replay_capture.cpp`" /Fe:`"$BuildDir\replay_capture.exe`" /Fo:`"$BuildDir\\`" d3d12.lib dxgi.lib dxguid.lib && " +
    "cl $ClFlags `"$Root\tests\repro_rtpso.cpp`" /Fe:`"$BuildDir\repro_rtpso.exe`" /Fo:`"$BuildDir\\`" d3d12.lib dxgi.lib dxguid.lib && " +
    "cl $ClFlags `"$Root\tests\dxil_asm.cpp`" /Fe:`"$BuildDir\dxil_asm.exe`" /Fo:`"$BuildDir\\`" && " +
    "cl $ClFlags `"$Root\tests\patch_dxil.cpp`" `"$Root\src\dxil_patcher.cpp`" /Fe:`"$BuildDir\patch_dxil.exe`" /Fo:`"$BuildDir\\`""

cmd /c $BuildCmd
if ($LASTEXITCODE -ne 0) { throw "MSVC compilation failed" }

Write-Host "Build completed successfully! Outputs in $BuildDir" -ForegroundColor Green
