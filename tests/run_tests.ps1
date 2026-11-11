# Runs the wrapper test suite in order and stops at the first failure.
#
#   powershell -ExecutionPolicy Bypass -File .\tests\run_tests.ps1 [-SkipGpuRepros]
#
# 1. test_wrapper.exe   NvAPI proxy, DXIL patcher, GPU argument converter, state restore, DRED.
# 2. repro_templates    cluster templates/instances/BLAS through build\nvapi64.dll, synthetic data
#                       shaped like Alan Wake 2 (see the header of tests\repro_templates.cpp).
# 3. capture + replay   AMD_MG_CAPTURE_REAL on a synthetic run, then replay_capture.exe on the file.
#
# WARNING: these tests run real GPU work on a preview driver. A driver fault can end in a TDR
# (black screen for a few seconds) and sometimes needs a reboot before D3D12 works again.
# Run build.ps1 first. Logs: build\amd_megageometry.log (overwritten by every step).

param([switch]$SkipGpuRepros, [int]$TimeoutSec = 120)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Build = Join-Path $Root "build"
$Log = Join-Path $Build "amd_megageometry.log"

foreach ($exe in "nvapi64.dll", "test_wrapper.exe", "repro_templates.exe", "replay_capture.exe") {
    if (-not (Test-Path (Join-Path $Build $exe))) { throw "build\$exe missing: run build.ps1 first" }
}

$dev = Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -Name AllowDevelopmentWithoutDevLicense -ErrorAction SilentlyContinue
if (-not $dev -or $dev.AllowDevelopmentWithoutDevLicense -ne 1) {
    throw "Windows Developer Mode is off: the Agility SDK 1.722 preview will not load (Settings > System > For developers)"
}

# Start from a clean configuration: no AMD_MG_* variable inherited from the user environment.
Get-ChildItem Env: | Where-Object Name -like "AMD_MG_*" | ForEach-Object { Remove-Item "Env:$($_.Name)" }

function Invoke-Test([string]$Label, [string]$Exe, [string[]]$Arguments, [string]$WorkDir, [string]$Expect) {
    Write-Host "=== $Label" -ForegroundColor Cyan
    Write-Host "    $Exe $($Arguments -join ' ')"
    Remove-Item $Log -ErrorAction SilentlyContinue
    $out = Join-Path $env:TEMP ("amd_mg_test_" + [guid]::NewGuid().ToString("N") + ".txt")
    $sp = @{ FilePath = (Join-Path $Build $Exe); WorkingDirectory = $WorkDir; NoNewWindow = $true; PassThru = $true
             RedirectStandardOutput = $out; RedirectStandardError = "$out.err" }
    if ($Arguments.Count -gt 0) { $sp.ArgumentList = $Arguments }
    $p = Start-Process @sp
    $null = $p.Handle  # keeps the exit code readable after the process ends
    $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
    if ($timedOut) { $p.Kill() }
    $text = (Get-Content $out -Raw -ErrorAction SilentlyContinue) + (Get-Content "$out.err" -Raw -ErrorAction SilentlyContinue)
    Remove-Item $out, "$out.err" -ErrorAction SilentlyContinue
    $text -split "`r?`n" | Where-Object { $_ -match "RESULT|Summary|FAILED|overflow|CLAS:|BLAS:|\[Real\]" -and $_ -notmatch "^\[cp\]" } |
        ForEach-Object { Write-Host "    $_" }
    $ok = -not $timedOut -and $text -match [regex]::Escape($Expect)
    if (-not $ok) {
        Write-Host "!!! $Label failed$(if ($timedOut) { ' (timeout)' }): stopping" -ForegroundColor Red
        if (Test-Path $Log) {
            Select-String -Path $Log -Pattern "Device removed|Page fault|\[Split\] last chunk|FAILED" |
                Select-Object -First 20 | ForEach-Object { Write-Host "    log: $($_.Line)" }
        }
        exit 1
    }
}

Invoke-Test "1 test_wrapper" "test_wrapper.exe" @() $Root " 0 FAILED"
if ($SkipGpuRepros) { Write-Host "GPU repros skipped." -ForegroundColor Yellow; exit 0 }

Invoke-Test "2a repro: RTXMG-like baseline"         "repro_templates.exe" @() $Build "RESULT: OK"
Invoke-Test "2b repro: all Alan Wake 2 traits"      "repro_templates.exe" @("--aw2", "--n=64", "--inst=256") $Build "RESULT: OK"
Invoke-Test "2c repro: AW2 traits + BLAS of 10 CLAS" "repro_templates.exe" @("--aw2", "--n=64", "--inst=256", "--blas=10") $Build "RESULT: OK"
Invoke-Test "2d repro: AW2 memory layout (--pack)"  "repro_templates.exe" @("--aw2", "--n=64", "--inst=256", "--blas=4", "--pack") $Build "RESULT: OK"

$capture = Join-Path $Build "amd_megageometry_capture.bin"
Remove-Item $capture -ErrorAction SilentlyContinue
$env:AMD_MG_CAPTURE_REAL = "4"
Invoke-Test "3a capture: AMD_MG_CAPTURE_REAL=4"     "repro_templates.exe" @("--aw2", "--n=64", "--inst=256", "--blas=10") $Build "RESULT: OK"
Remove-Item Env:AMD_MG_CAPTURE_REAL
if (-not (Test-Path $capture)) { Write-Host "!!! no capture file written: stopping" -ForegroundColor Red; exit 1 }
Invoke-Test "3b replay of the capture"              "replay_capture.exe" @("amd_megageometry_capture.bin") $Build "RESULT: OK"

Write-Host "All tests passed." -ForegroundColor Green
