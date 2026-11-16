# Driver issue: amdxc64.dll crashes (access violation) inside CreateStateObject on a valid,
# validator-signed SM 6.10 raytracing library.
#
#   powershell -ExecutionPolicy Bypass -File .\scripts\repro_compiler_crash.ps1 [-RtxmgBin <dir>]
#
# Input: the path tracer DXIL library shipped with the NVIDIA RTXMG 2.2.0 sample. It is NVIDIA code
# and is not in this repository: download RTXMG yourself (README, "Dependencies"). -RtxmgBin is its
# bin\ folder; default sdk\rtxmg_bin\rtxmg.2.2.0.win-x64-arm64\bin.
#
# Steps (all on the CPU, no GPU work is submitted):
#   1. Extract the first DXIL container of shaders\rtxmg_demo\dxil\shaders\rtxmg_demo_path_tracer.bin
#      (the NvAPI variant of the path tracer) to build\repro\path_tracer_nv.dxil.
#   2. patch_dxil.exe translates its NvAPI intrinsics to SM 6.10, twice:
#        control.dxil      NvGetSpecial(GLOBAL_TIMER) -> WaveGetLaneIndex() (non-constant placeholder)
#        timer_const.dxil  NvGetSpecial(GLOBAL_TIMER) -> constant 0 (AMD_MG_TIMER_CONST=1)
#      Both pass the DXIL validator (dxil.dll) and are signed.
#   3. repro_rtpso.exe compiles each into a raytracing pipeline with the driver.
#      Expected on 26.20.12.01: control compiles (hr=0x00000000); timer_const crashes
#      ("CRASH exception 0xC0000005 inside CreateStateObject"), also with only the RayGen and
#      ClusterLodClosestHit exports. The constant timer value only feeds the raygen "timeview"
#      output, so the shader is legal; the compiler should not crash on it.
#
# Verified on RX 9070 XT, AMD Software: Agility SDK Developer Preview Edition 26.20.12.01
# (32.0.32012.1013), 2026-11-04. Not re-verified on later drivers: Adrenalin 26.11.1 does not
# expose clustered geometry and rejects these libraries with E_INVALIDARG.
# Run build.ps1 first.

param([string]$RtxmgBin, [int]$TimeoutSec = 180)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Out = Join-Path $Build "repro"
if (-not $RtxmgBin) { $RtxmgBin = Join-Path $Root "sdk\rtxmg_bin\rtxmg.2.2.0.win-x64-arm64\bin" }
foreach ($exe in "patch_dxil.exe", "repro_rtpso.exe", "dxcompiler.dll", "dxil.dll") {
    if (-not (Test-Path (Join-Path $Build $exe))) { throw "build\$exe missing: run build.ps1 first" }
}
$Bin = Join-Path $RtxmgBin "shaders\rtxmg_demo\dxil\shaders\rtxmg_demo_path_tracer.bin"
if (-not (Test-Path $Bin)) { throw "$Bin not found: pass -RtxmgBin <RTXMG 2.2.0 bin folder>" }
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# 1. First DXBC container in the ShaderMake blob: "DXBC", 16-byte hash, version, total size (u32).
$bytes = [IO.File]::ReadAllBytes($Bin)
$start = -1
for ($i = 0; $i -le $bytes.Length - 32; $i++) {
    if ($bytes[$i] -eq 0x44 -and $bytes[$i + 1] -eq 0x58 -and $bytes[$i + 2] -eq 0x42 -and $bytes[$i + 3] -eq 0x43) { $start = $i; break }
}
if ($start -lt 0) { throw "no DXBC container in $Bin" }
$size = [BitConverter]::ToUInt32($bytes, $start + 24)
if ($start + $size -gt $bytes.Length) { throw "truncated DXBC container in $Bin" }
$Library = Join-Path $Out "path_tracer_nv.dxil"
$lib = New-Object byte[] $size
[Array]::Copy($bytes, $start, $lib, 0, $size)
[IO.File]::WriteAllBytes($Library, $lib)
Write-Host "=== 1. extracted $size bytes at offset 0x$($start.ToString('X')) -> build\repro\path_tracer_nv.dxil" -ForegroundColor Cyan
if ($size -ne 1103224) { Write-Host "    note: RTXMG 2.2.0 gives 1103224 bytes; other versions may not reproduce" -ForegroundColor Yellow }

function Run([string]$Exe, [string[]]$Arguments) {
    Write-Host "    $Exe $($Arguments -join ' ')"
    $tmp = Join-Path $env:TEMP ("amd_mg_repro_" + [guid]::NewGuid().ToString("N") + ".txt")
    $p = Start-Process -FilePath (Join-Path $Build $Exe) -ArgumentList $Arguments -WorkingDirectory $Build -NoNewWindow -PassThru `
                       -RedirectStandardOutput $tmp -RedirectStandardError "$tmp.err"
    $null = $p.Handle
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Host "    (timeout)" }
    $text = (Get-Content $tmp -Raw -ErrorAction SilentlyContinue) + (Get-Content "$tmp.err" -Raw -ErrorAction SilentlyContinue)
    Remove-Item $tmp, "$tmp.err" -ErrorAction SilentlyContinue
    $result = ($text -split "`r?`n" | Where-Object { $_ -match "^RESULT|AMD_MG_TIMER_CONST|Validator" }) -join "`n    "
    if (-not $result) { $result = "(no RESULT line, exit code $($p.ExitCode))" }
    Write-Host "    $result"
    return $text
}

# 2. Patch twice. Only AMD_MG_TIMER_CONST differs between the two outputs.
Get-ChildItem Env: | Where-Object Name -like "AMD_MG_*" | ForEach-Object { Remove-Item "Env:$($_.Name)" }
Write-Host "=== 2a. patch with the non-constant timer placeholder (control)" -ForegroundColor Cyan
$t = Run "patch_dxil.exe" @("repro\path_tracer_nv.dxil", "repro\control.dxil")
if ($t -notmatch "RESULT: patched") { Write-Host "patch failed, stopping" -ForegroundColor Red; exit 1 }
Write-Host "=== 2b. patch with AMD_MG_TIMER_CONST=1" -ForegroundColor Cyan
$env:AMD_MG_TIMER_CONST = "1"
$t = Run "patch_dxil.exe" @("repro\path_tracer_nv.dxil", "repro\timer_const.dxil")
Remove-Item Env:AMD_MG_TIMER_CONST
if ($t -notmatch "RESULT: patched") { Write-Host "patch failed, stopping" -ForegroundColor Red; exit 1 }

# 3. Compile with the driver.
Write-Host "=== 3a. CreateStateObject, control" -ForegroundColor Cyan
$c = Run "repro_rtpso.exe" @("repro\control.dxil")
Write-Host "=== 3b. CreateStateObject, constant timer" -ForegroundColor Cyan
$k = Run "repro_rtpso.exe" @("repro\timer_const.dxil")
Write-Host "=== 3c. CreateStateObject, constant timer, RayGen + ClusterLodClosestHit only" -ForegroundColor Cyan
$m = Run "repro_rtpso.exe" @("repro\timer_const.dxil", "--only=RayGen,ClusterLodClosestHit")

if ($c -match "hr=0x80070057") {
    Write-Host "The driver rejects the control library (E_INVALIDARG): it does not support SM 6.10 clustered geometry. Use the 26.20.12.01 preview driver." -ForegroundColor Yellow
} elseif ($c -notmatch "hr=0x00000000") {
    Write-Host "Control did not compile: the setup is broken, the result below is not meaningful." -ForegroundColor Yellow
} elseif ($k -match "CRASH" -or $m -match "CRASH") {
    Write-Host "Reproduced: the control compiles, the constant-timer variant crashes the driver compiler." -ForegroundColor Red
} else {
    Write-Host "Not reproduced on this driver: both variants compile." -ForegroundColor Green
}
