# NvAPI/DXR difference: a null address in the explicit destination array of a cluster operation
# hangs the GPU on AMD. Not a driver bug: in DXR an empty argument still produces an empty object
# that is written to its destination, so a null destination is undefined behavior.
#
#   powershell -ExecutionPolicy Bypass -File .\scripts\repro_null_dst_hang.ps1 [-Yes]
#
# Builds 64 cluster templates through the wrapper (build\nvapi64.dll -> DXR 1.722
# ExecuteIndirectRTASOperations, explicit destinations); the last 16 arguments are empty (0 triangles).
#   A. empty arguments with valid destinations   -> expected "RESULT: OK"
#   B. same, but their destination address is 0  -> on driver 26.20.12.01 the GPU hangs:
#      "RESULT: DEVICE LOST", removed=0x887A0006 (DXGI_ERROR_DEVICE_HUNG)
# AMD_MG_SANITIZE_DST=0 turns off the wrapper's workaround (null destinations redirected to a sink
# buffer), so the null addresses reach the driver as-is. NVIDIA's NvAPI accepts them. The DXR spec
# (Raytracing2, D3D12_RTAS_BATCHED_OPERATION_DATA / *_ARGS) allows a NULL destination array only in
# GET_SIZES mode and says TriangleCount 0 "just produces an empty CLAS", so the wrapper's redirect
# is the correct translation, not just a workaround.
#
# Verified on RX 9070 XT, AMD Software: Agility SDK Developer Preview Edition 26.20.12.01
# (32.0.32012.1013), 2026-11-06. Not re-verified on later drivers.
#
# WARNING: step B causes a TDR (black screen for a few seconds). D3D12 may not work again until reboot.
# Run build.ps1 first. Wrapper log: build\amd_megageometry.log.

param([switch]$Yes, [int]$TimeoutSec = 60)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Exe = Join-Path $Build "repro_templates.exe"
if (-not (Test-Path $Exe) -or -not (Test-Path (Join-Path $Build "nvapi64.dll"))) { throw "build\repro_templates.exe or build\nvapi64.dll missing: run build.ps1 first" }

if (-not $Yes) {
    Write-Host "Step B hangs the GPU on purpose (TDR, maybe a reboot). Save your work first." -ForegroundColor Yellow
    if ((Read-Host "Continue? [y/N]") -notmatch "^[yY]") { exit 0 }
}

Get-ChildItem Env: | Where-Object Name -like "AMD_MG_*" | ForEach-Object { Remove-Item "Env:$($_.Name)" }
$env:AMD_MG_SANITIZE_DST = "0"

function Run([string]$Label, [string[]]$Arguments) {
    Write-Host "=== $Label" -ForegroundColor Cyan
    Write-Host "    repro_templates.exe $($Arguments -join ' ')"
    $out = Join-Path $env:TEMP ("amd_mg_repro_" + [guid]::NewGuid().ToString("N") + ".txt")
    $p = Start-Process -FilePath $Exe -ArgumentList $Arguments -WorkingDirectory $Build -NoNewWindow -PassThru `
                       -RedirectStandardOutput $out -RedirectStandardError "$out.err"
    $null = $p.Handle
    if (-not $p.WaitForExit($TimeoutSec * 1000)) { $p.Kill(); Write-Host "    (timeout)" }
    $text = (Get-Content $out -Raw -ErrorAction SilentlyContinue) + (Get-Content "$out.err" -Raw -ErrorAction SilentlyContinue)
    Remove-Item $out, "$out.err" -ErrorAction SilentlyContinue
    $result = ($text -split "`r?`n" | Where-Object { $_ -match "^RESULT" }) -join " "
    if (-not $result) { $result = "(no RESULT line)" }
    Write-Host "    $result"
    return $result
}

$a = Run "A: 16 empty arguments, valid destinations" @("--n=64", "--empty=16")
if ($a -notmatch "RESULT: OK") {
    Write-Host "Control run A did not pass: the setup is broken (driver without clustered geometry? see README), stopping." -ForegroundColor Red
    exit 1
}
$b = Run "B: 16 empty arguments, null destinations" @("--n=64", "--empty=16", "--emptydst")
if ($b -match "DEVICE LOST|timeout|no RESULT") {
    Write-Host "Reproduced: null destinations for empty arguments hang the GPU (A passes, B does not)." -ForegroundColor Red
} else {
    Write-Host "Not reproduced on this driver: B passes too." -ForegroundColor Green
}
