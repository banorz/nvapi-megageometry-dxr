# Build, Ambiente Locale & Deploy (`amd-megageometry`)

## 1. Prerequisiti su Windows 11 (RDNA 4 / RX 9070 XT)

### A. Windows Developer Mode (Obbligatorio per Agility SDK 1.722 Preview)
Poiché Clustered Geometry e Shader Model 6.10 fanno parte dell'Agility SDK Preview (`D3D12_PREVIEW_SDK_VERSION = 722`), il runtime D3D12 di Windows rifiuta di attivare `D3D12RaytracingPreview` e `D3D12ExperimentalShaderModels` se la Modalità Sviluppatore di Windows è disattivata.

Per verificare lo stato attuale:
```powershell
Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -Name AllowDevelopmentWithoutDevLicense
```
Per abilitarla (da PowerShell **come Amministratore** o da `Impostazioni -> Sistema -> Per sviluppatori`):
```powershell
reg add "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" /t REG_DWORD /f /v "AllowDevelopmentWithoutDevLicense" /d "1"
```

### B. Driver AMD Richiesto: Agility SDK Developer Preview Edition 26.20.12.01
- **`amd-megageometry`** gira interamente su DirectX 12 nativo: **non richiede ROCm/HIP** a runtime. Usa il driver grafico AMD Adrenalin D3D12 (`amdxc64.dll`).
- **Driver obbligatorio per `Clusters=1` (`ClustersAndPTLASSupported = TRUE`)**:
  - [**AMD Software: Agility SDK Developer Preview Edition 26.20.12.01**](https://www.amd.com/en/resources/support-articles/release-notes/RN-RAD-MS-AGILITY-SDK-26-20-12-01.html) (`RN-RAD-MS-AGILITY-SDK-26-20-12-01`).
  - Sul driver Adrenalin standard `32.0.32015.7029` (18/10/2026), `CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS_RAYTRACING2)` riporta `ClustersAndPTLASSupported = FALSE` (`Clusters=0`), quindi `ExecuteIndirectRTASOperations` non costruisce i nodi CLAS in VRAM (la scena 3D di `rtxmg_demo.exe` resta vuota). Installando il driver **26.20.12.01**, `ClustersAndPTLASSupported` diventa `TRUE` in hardware sulla RX 9070 XT.
  - **Adrenalin 26.11.1** (`32.0.32015.16001`, 11/11/2026, primo driver non beta dopo la preview): di nuovo `Clusters=0`. `ID3D12DeviceRaytracing2` esiste e il prebuild info dei CLAS risponde, ma `CreateStateObject` con gli intrinsics SM 6.10 (`clusterID`, `triangleObjectPosition`) restituisce `E_INVALIDARG`: `test_wrapper` 27/28 (verificato il 13/11/2026). Per lavorare serve ancora il driver preview.
- Per il progetto parallelo `amd-nr` (DLSS 5 Neural Rendering via HIP), sulla macchina è installato ROCm 7.1 (`C:\Program Files\AMD\ROCm\7.1`), mentre online sono disponibili gli aggiornamenti **ROCm / HIP SDK 7.2.x** per Windows con miglioramenti per `gfx1201`.

## 2. Compilazione Locale

Lo script [build.ps1](../build.ps1) trova Visual Studio con `vswhere` (o usa `$env:VSDEVCMD`), controlla che le dipendenze in `sdk/` esistano (come scaricarle: [README](../README.md#dependencies)), configura l'ambiente MSVC, compila i compute shader di conversione (`shaders/arg_converter.hlsl`) in header C++ tramite DXC (`sdk/dxc/extracted/bin/x64/dxc.exe`), e genera:
- `build/nvapi64.dll` — Il wrapper completo pronto da copiare nei giochi.
- `build/test_wrapper.exe` — La suite di test end-to-end.

```powershell
powershell -ExecutionPolicy Bypass -File .\build.ps1
```

## 3. Deploy in un Gioco o Demo con OptiScaler / AMD-NR

Poiché `nvapi64.lib` di NVIDIA carica `nvapi64.dll` usando `LOAD_LIBRARY_SEARCH_SYSTEM32` (`C:\Windows\System32\nvapi64.dll`) e i titoli RTX Mega Geometry usano Streamline (`sl.interposer.dll` / `sl.common.dll`), il deploy raccomandato si appoggia a **OptiScaler / AMD-NR** (oppure al proxy `winmm.dll` incluso nel nostro binario):

1. Copia nella cartella dell'eseguibile del gioco (es. `C:\Program Files\Epic Games\AlanWake2\` o `sdk\rtxmg_bin\rtxmg.2.2.0.win-x64-arm64\bin\`):
   - `build\nvapi64.dll` (e opzionalmente una copia chiamata `winmm.dll` per intercettare `LoadLibraryExW` al boot del processo)
   - `build\dxcompiler.dll` e `build\dxil.dll`
2. Aggiorna l'Agility SDK del gioco alla versione `1.722.0-preview` copiando nella sottocartella `D3D12\` (e nella root se il gioco ha `D3D12Core.dll` nella root, come Alan Wake 2):
   - `build\D3D12\D3D12Core.dll`
   - `build\D3D12\D3D12StateObjectCompiler.dll`
   - `build\D3D12\d3d12SDKLayers.dll`
3. **Integrazione con OptiScaler / AMD-NR (`dxgi.dll`)**:
   Poiché `AMD-NR` / `OptiScaler` intercetta a livello `ntdll` il caricamento di `nvapi64.dll` e restituisce il proprio modulo `dxgi.dll` (che contiene `fakenvapi::queryInterface` compilato internamente), il modo più pulito per caricare il nostro wrapper senza toccare `UseFakenvapi` è come plugin ASI:
   - Copia `build\nvapi64.dll` in `OptiScaler\plugins\amd_megageometry.asi`
   - Imposta in `OptiScaler.ini`:
     ```ini
     [Plugins]
     LoadAsiPlugins = true
     ```
   All'avvio, `amd_megageometry.asi` esegue `PatchModuleEatForNvapi` su `dxgi.dll` (iniettando uno stub non-RIP-relative compatibile con Microsoft Detours) e `PatchModuleIat` sull'eseguibile principale: tutte le 14 funzioni RTX Mega Geometry vengono servite dal nostro wrapper e tutte le altre chiamate NvAPI vengono inoltrate in cascata al `fakenvapi` interno di `dxgi.dll`.

## 4. Gotchas & Note Operative
- **`LOAD_LIBRARY_SEARCH_SYSTEM32` in `nvapi64.lib`**: La libreria statica ufficiale NVIDIA chiama `LoadLibraryExW(L"nvapi64.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32)`, ignorando la cartella dell'eseguibile a meno che OptiScaler (`NvapiPath` / plugin ASI) o l'hook IAT in [src/d3d12_hook.cpp](../src/d3d12_hook.cpp) (`InstallProcessIatHooks`) non intercetti la chiamata.
- **Patching `D3D12SDKVersion` prima della probe D3D12 di OptiScaler**: Giochi come `AlanWake2.exe` esportano `D3D12SDKVersion = 619` e `D3D12SDKPath = ".\\"`, e `OptiScaler` (`dxgi.dll`) crea un device D3D12 di probe (`IdentifyGpu::updateD3d12Capabilities`) pochi millisecondi dopo il caricamento dei plugin ASI, prima che Streamline chiami `NvAPI_Initialize()`. Perciò [src/d3d12_hook.cpp](../src/d3d12_hook.cpp) modifica in memoria l'export `D3D12SDKVersion` dell'eseguibile (`619 -> 722`) già in `InstallProcessIatHooks` e aggancia `D3D12CreateDevice` anche su `dxgi.dll`, assicurando che `D3D12EnableExperimentalFeatures` venga chiamata prima del primo `D3D12CreateDevice`. Inoltre `D3D12Core.dll`, `d3d12SDKLayers.dll` e `D3D12StateObjectCompiler.dll` (`1.722.0-preview`) devono essere presenti sia nella cartella root del gioco sia in `D3D12\`.
- **Stripping chunk `ILDB` e preservazione stride `StructuredBuffer` in [src/dxil_patcher.cpp](../src/dxil_patcher.cpp)**:
  1. Se un container DXBC contiene il chunk di debug `ILDB`, `IDxcCompiler3::Disassemble` disassembla `ILDB` (che contiene intrinsics `@llvm.dbg.*` illegali in DXIL runtime) invece di `DXIL`. Il patcher rinomina `ILDB` in `ILDX` in un buffer temporaneo prima di disassemblare.
  2. L'upgrade di `!dx.version` a `1.10` (`!N = !{i32 1, i32 10}`) deve modificare esclusivamente il nodo referenziato da `!dx.version = !{!N}`, senza toccare i nodi metadata di stride dei `StructuredBuffer` (`!M = !{i32 1, i32 <stride>}`).
- **Firma DXIL (`dxil.dll`)**: Quando `dxcompiler.dll` riassembla il modulo LLVM IR modificato tramite `IDxcAssembler::AssembleToContainer`, il container risultante viene validato e firmato in-place tramite `IDxcValidator::Validate` di `dxil.dll`.
