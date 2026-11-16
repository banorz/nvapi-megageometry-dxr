# amd-megageometry — Agent Guide

Wrapper C++20 (`nvapi64.dll` proxy + D3D12 hook + DXIL patcher SM 6.10 + GPU argument converter) che traduce le chiamate proprietarie NVIDIA **RTX Mega Geometry** (CLAS, Cluster Templates, Cluster BLAS, Partitioned TLAS e shader intrinsics `nvHLSLExtns`) nelle API native **DirectX Raytracing 1.722 Preview** (`ID3D12DeviceRaytracing2` / `ID3D12CommandListRaytracing2`) su GPU AMD RDNA 4 (Radeon RX 9070 XT / `gfx1201`).

## Indice documentazione `.agent/`
- [architecture.md](architecture.md) — Stack, layout repo, flusso dati e file chiave.
- [deploy.md](deploy.md) — Requisiti sistema (Windows Developer Mode, Agility SDK 1.722, DXC), compilazione e deploy nella cartella del gioco.
- [translation.md](translation.md) — Tabelle di traduzione binaria host/GPU structs (NvAPI ↔ D3D12 1.722) e mapping opcode DXIL (`g_NvidiaExt` ↔ SM 6.10).

## Stato e ripresa (in pausa dal 2026-11-11)
- **Funziona:** demo NVIDIA RTXMG 2.2.0 con `--cluster-api nvapi` (tradotto dal wrapper), anche con DLSS-RR via AMD-NR; tutti i test di [tests/run_tests.ps1](../tests/run_tests.ps1).
- **Non funziona:** Alan Wake 2, crash GPU dopo "Continua" in `BUILD_BLAS_FROM_CLAS` costruita sui CLAS veri del gioco (con un CLAS finto le 501 BLAS passano). RGD indica un fault dentro il driver senza risorsa associata. Dettagli in [architecture.md](architecture.md) "Diagnostica cluster".
- **Perché in pausa:** probabile bug del driver AMD *Agility SDK Developer Preview 26.20.12.01* (DXR 1.722 è ancora preview). Si riprova con il primo driver che supporta DXR 1.722 fuori preview.
- **Passi alla ripresa:**
  1. Installa il nuovo driver; aggiorna se serve Agility SDK e DXC in `sdk/` (versioni nel [README](../README.md#dependencies)) e ricompila con `build.ps1`.
  2. `powershell -ExecutionPolicy Bypass -File .\tests\run_tests.ps1` (chiedere prima all'utente: può dare TDR).
  3. Demo RTXMG: `rtxmg_demo.exe ... --cluster-api nvapi` e `--cluster-api dxr` (comandi sotto).
  4. AW2: reinstalla AMD-NR, deploy come plugin ASI ([deploy.md](deploy.md)) con `amd_megageometry.ini` = `AMD_MG_INDEX_FORMAT=16` + `AMD_MG_CAPTURE_REAL=16` e **senza** gli A/B (scratch privato, CLAS finto): prima si verifica se il driver nuovo basta da solo. Avvio, "Continua".
  5. Se crasha ancora: copia `OptiScaler\plugins\amd_megageometry_capture.bin` in `build\` e `build\replay_capture.exe amd_megageometry_capture.bin` (poi `--only=B` sulla BLAS fallita). Se il replay crasha → bisezione offline dei cluster; se passa → il problema è nello stato del gioco, segnalare ad AMD con dump RGD.
- Il file di cattura contiene geometria di Alan Wake 2: non va pubblicato né messo nel repo (`*.bin` è in `.gitignore`).

## Quick Facts
- **Target Hardware**: AMD Radeon RX 9000 series (RDNA 4, testato su RX 9070 XT con driver **AMD Software: Agility SDK Developer Preview Edition 26.20.12.01** / `32.0.32012.1013`).
- **Dipendenze SDK esterne (in `sdk/`, git-ignored)**:
  - `sdk/agility/extracted` — `Microsoft.Direct3D.D3D12.1.722.0-preview` (`d3d12.h`, `D3D12Core.dll`, `d3d12SDKLayers.dll`, `D3D12StateObjectCompiler.dll`)
  - `sdk/dxc/extracted` — DirectXShaderCompiler `v1.10.2605.37` (SM 6.10 `lib_6_10`, `dxcompiler.dll`, `dxil.dll`)
  - `sdk/nvapi/repo` — `NVIDIA/nvapi` headers (`nvapi.h`, `nvapi_interface.h`, `nvHLSLExtnsInternal.h`, `nvShaderExtnEnums.h`)
  - `sdk/rtxmg` & `sdk/nvrhi` — Riferimenti ufficiali per il confronto side-by-side `NVAPI_CLUSTERS` vs `DXRCLUSTER`.
- **Variabili d'ambiente runtime**:
  - `AMD_MG_LOG=1` (attivo di default) — abilita il log dettagliato su `amd_megageometry.log` nella cartella della DLL/ASI.
  - `AMD_MG_DUMP_DXIL=1` — salva l'ultimo blob patchato in `patched_cluster_shader.dxil` (cartella di lavoro del processo).
  - `AMD_MG_FORCE_CAPS=1` — forza il report di supporto Cluster Operations / Partitioned TLAS in `NvAPI_D3D12_GetRaytracingCaps`.
  - `AMD_MG_TRACE_CXX=1` — vectored exception handler diagnostico: logga ogni eccezione C++ first-chance del processo (max 20) con `what()` e lo stack (modulo+offset).
  - `AMD_MG_DRED=0` — disattiva DRED, che è **attivo di default**: breadcrumb automatici e page fault abilitati prima di `D3D12CreateDevice`. Quando la GPU viene rimossa, il log riporta `[DRED]` con le liste non finite (operazioni intorno a `==>`), il VA del page fault e le allocazioni vicine ([src/dred.cpp](../src/dred.cpp)). Costa un po' di prestazioni: per i benchmark usare `AMD_MG_DRED=0`.
  - `AMD_MG_DUMP_ARGS=0` — disattiva il dump degli argomenti, che è **attivo di default**: per le prime 2 operazioni cluster di ogni tipo, una copia GPU dei primi 64 argomenti, dell'array di destinazione e del conteggio finisce in un buffer readback. Nel log compare come `[Dump]`, decodificato con conteggi, formati e intervalli di indirizzi. Viene stampato anche dentro il dump DRED se la GPU va in crash subito dopo ([src/gpu_arg_converter.cpp](../src/gpu_arg_converter.cpp)).
  - `AMD_MG_INDEX_FORMAT=16|32` — formato indici dichiarato a DXR per template/CLAS da triangoli (default `8` = trucco UINT8, vedi [architecture.md](architecture.md) "IndexFormat"). Alan Wake 2 usa indici a 16 bit assoluti nella mesh (≥ vertexCount, anche > 255): con `UINT8` quelli > 255 vengono troncati (geometria sbagliata, nessun crash). Con `16`, argomenti a 8 bit verrebbero letti male.
  - `AMD_MG_SPLIT_OPS=N` — diagnostica: esegue le operazioni cluster explicit a blocchi di N argomenti (default 256, `0` disattiva) con marker GPU per blocco; al crash il log indica il blocco in volo e ne stampa gli argomenti ([architecture.md](architecture.md) "Diagnostica cluster").
  - **`amd_megageometry.ini`** accanto alla DLL/ASI: righe `AMD_MG_*=valore` (commenti `#`/`;`), lette al caricamento e applicate all'ambiente del processo se la variabile non è già impostata; nel log `[Config]`. Serve per i giochi avviati da launcher (Epic) senza variabili di sistema. In AW2 c'è sia in `OptiScaler\plugins\` sia nella cartella del gioco.
  - `AMD_MG_BLAS_PRIVATE_SCRATCH=1` — A/B: le operazioni BLAS_FROM_CLAS usano uno scratch del wrapper (dimensione da `GetRTASOperationPrebuildInfo`, solo crescente) invece del `batchScratchData` del gioco. Log `[A/B] private BLAS scratch`.
  - `AMD_MG_BLAS_FAKE_CLAS=1` — A/B: il wrapper costruisce una volta un CLAS da 1 triangolo (float3, UINT16) e ogni BLAS non vuota viene costruita su quello (`ClasAddressCount=1`, modo 3 dello shader con `g_Extra` = array indirizzi). Geometria sbagliata per definizione; serve a capire se il crash dipende dai CLAS del gioco. Log `[A/B] fake CLAS`.
  - `AMD_MG_FORCE_GEOM0=1` — A/B: nei modi 0/1 dello shader (CLAS/template da triangoli) azzera l'indice di geometria in `BaseGeometryIndexAndFlags` mantenendo i flag (bit 24..31). `MaxGeometryIndexValue` resta quello del gioco.
  - `AMD_MG_TEMPLATE_NO_HINT=1` — A/B: nel modo 1 (template) azzera il `VertexBuffer` del template (in NvAPI è un suggerimento opzionale; le posizioni vere arrivano dall'istanziazione).
  - `AMD_MG_CAPTURE_REAL=N` — cattura dei dati reali: alla prima operazione BLAS copia sulla GPU (ExecuteIndirect con root SRV/UAV per comando, indirizzi seguiti dai modi 13/14 dello shader) indici e finestre di vertici (istanza e hint) di ogni CLAS delle prime N BLAS (max 8192 CLAS, 160 MB) e scrive `amd_megageometry_capture.bin` accanto alla DLL (al device removal o alla chiusura). Log `[Real]`. Richiede la layout capture (`AMD_MG_DUMP_ARGS`, attiva di default). Replay offline: `build\replay_capture.exe <file> [--only=B] [--nocount]`.
  - `AMD_MG_SPLIT_BLAS=N` — esegue `BUILD_BLAS_FROM_CLAS` a blocchi di N BLAS (default 1, `0` = un'unica operazione come la chiede il gioco). Tiene `maxArgCount × maxClasPerArg` alla dimensione di una sola BLAS e al crash indica la BLAS in volo.
  - `AMD_MG_SANITIZE_DST=0` — disattiva il fix del crash GPU di Alan Wake 2: di default le destinazioni nulle dell'array explicit vengono redirette su un buffer sink, perché il driver AMD ci scrive comunque e manda in hang la GPU (corretto per la spec DXR: un argomento vuoto produce un oggetto vuoto, la destinazione nulla è comportamento indefinito) ([architecture.md](architecture.md) "Diagnostica cluster").
  - `AMD_MG_TIMER_CONST=1` — solo repro: `NvGetSpecial(GLOBAL_TIMER_LO/HI)` diventa la costante `0` invece del segnaposto `WaveGetLaneIndex()`, il caso che fa crashare `amdxc64.dll` 32.0.32012.1013 in `CreateStateObject` ([translation.md](translation.md) §3). Usata da [scripts/repro_compiler_crash.ps1](../scripts/repro_compiler_crash.ps1).
  - `AMD_MG_STATE_SHADOW=0` — diagnostico: niente hook sulle command list e niente ripristino dello stato compute ([src/cmdlist_state.cpp](../src/cmdlist_state.cpp)).
  - `AMD_MG_FORCE_DXR_CAPS=1` — forza `ClustersAndPTLASSupported = TRUE` in `CheckFeatureSupport(OPTIONS_RAYTRACING2)` se `ID3D12DeviceRaytracing2` esiste.
  - `AMD_MG_DISABLE=1` — A/B senza Mega Geometry. Il gioco vede Cluster Operations e Partitioned TLAS come non supportati: caps NvAPI `NONE`, opcode shader cluster `false`, `OPTIONS_RAYTRACING2.ClustersAndPTLASSupported = FALSE`. Agility 1.722, chaining NvAPI, Streamline e AMD-NR restano attivi. Viene letta dall'ambiente Win32 del processo a ogni query. RTXMG non ha un percorso senza cluster ed esce con `FATAL ERROR: The graphics device does not support Clusters`; il confronto vero si fa in Alan Wake 2 (vedi sotto).

## Comandi più frequenti

```powershell
# Build completa (nvapi64.dll + test_wrapper.exe)
powershell -ExecutionPolicy Bypass -File .\build.ps1

# Esecuzione suite di test end-to-end (NvAPI proxy + DXIL patcher + GPU struct layout)
.\build\test_wrapper.exe

# Tutti i test in sequenza (test_wrapper, repro sintetici, cattura+replay); si ferma al primo errore
powershell -ExecutionPolicy Bypass -File .\tests\run_tests.ps1     # -SkipGpuRepros = solo test_wrapper

# Ispezione di un blob DXIL patchato
.\sdk\dxc\extracted\bin\x64\dxc.exe -dumpbin .\build\patched_test.dxil

# Repro offline di crash del driver in CreateStateObject (da build\, carica .\D3D12\ 1.722)
cd build
.\repro_rtpso.exe patched_native_nv.dxil                                  # libreria RTXMG intera
.\repro_rtpso.exe patched_native_nv.dxil --only=RayGen,ClusterLodClosestHit # bisezione per export
# Bisezione sull'IR: dumpbin -> modifica .ll -> riassembla+valida -> repro
..\sdk\dxc\extracted\bin\x64\dxc.exe -dumpbin patched_native_nv.dxil -Fc x.ll
.\dxil_asm.exe x.ll x.dxil; .\repro_rtpso.exe x.dxil
.\patch_dxil.exe native_nv.dxil out.dxil          # patcher del wrapper su un file (rispetta AMD_MG_TIMER_CONST)
cd ..

# Repro pubblicabili dei bug del driver (README "Driver issues and repro scripts"); verificati solo sul preview 26.20.12.01
powershell -ExecutionPolicy Bypass -File .\scripts\repro_compiler_crash.ps1   # estrae la libreria dalla copia di RTXMG, solo CPU
powershell -ExecutionPolicy Bypass -File .\scripts\repro_null_dst_hang.ps1    # TDR voluto: chiedere prima all'utente

# Demo RTXMG sul percorso NvAPI (cartella bin della demo)
.\rtxmg_demo.exe -mf cluster_lod/ABeautifulGame/glTF/ABeautifulGame.gltf --cluster-api nvapi   # oppure dxr; --dlssMode off; --sllog
```
### Benchmark
- **Costo del wrapper (demo RTXMG):** stessa scena con percorso ripetibile, confrontando `--cluster-api nvapi` (tradotto) e `--cluster-api dxr` (DXR nativo; vedi l'avviso sull'hang al punto 4 del troubleshooting):
  `rtxmg_demo.exe -mf cluster_lod/ABeautifulGame/glTF/ABeautifulGame.gltf --cluster-api nvapi --dolly-frames 240 -nf 3000 --dump-stats stats_nvapi.json`
- **Con e senza Mega Geometry (Alan Wake 2):** avvia il gioco con e senza `AMD_MG_DISABLE=1` (variabile di sistema/utente o launcher che la imposta), dallo stesso salvataggio e con lo stesso percorso. Registra 60–90 s con PresentMon o CapFrameX (FPS medi, 1% low, frame time) e annota la VRAM. Nel log verifica `GetRaytracingCaps(CLUSTER_OPERATIONS) -> NONE ... disabled=1`.

`build\native_nv.dxil` / `native_dxr.dxil` sono le librerie path-tracing reali della demo (variante NvAPI e DXR nativa): il primo container DXBC (offset `0x32`) di `bin\shaders\rtxmg_demo\dxil\shaders\rtxmg_demo_path_tracer.bin` / `rtxmg_demo_path_tracer_dxr.bin`. Sono codice NVIDIA: mai committarle; `test_wrapper.exe` rigenera `build\patched_native_nv.dxil`.

## Troubleshooting Flowchart

1. **Quando l'utente dice: "`D3D12CreateDevice` fallisce con `0x887E0003` (`DXGI_ERROR_SDK_COMPONENT_MISSING`) o `D3D12EnableExperimentalFeatures` restituisce `E_NOINTERFACE` (`0x80004002`)"**
   - Controlla se **Windows Developer Mode** è attivo:
     `Get-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\AppModelUnlock" -Name AllowDevelopmentWithoutDevLicense`
   - Se vale `0`, abilitalo da terminale amministratore o da `Impostazioni -> Sistema -> Per sviluppatori -> Modalità sviluppatore`. Senza Developer Mode, Windows blocca il caricamento di qualsiasi Agility SDK Preview (`1.722.0-preview`) e di `D3D12RaytracingPreview` / `D3D12ExperimentalShaderModels`.
2. **Quando l'utente dice: "Il gioco non vede il supporto RTX Mega Geometry nel menu grafico"**
   - Verifica in `amd_megageometry.log` che `nvapi_QueryInterface` abbia intercettato `0x85a6c2a0` (`NvAPI_D3D12_GetRaytracingCaps`) con `type = 3` (Cluster Operations) o `type = 4` (Partitioned TLAS).
   - Se il gioco chiama `NvAPI_GPU_GetArchInfo` o controlla VendorID NVIDIA prima di `GetRaytracingCaps`, assicurati che `nvapi64.dll` sia nella stessa cartella dell'eseguibile del gioco (o caricato via OptiScaler/ASI loader).
3. **Quando l'utente dice: "`CreateStateObject` fallisce / il gioco crasha in `amdxc64.dll` (`0xc0000005`) quando compila le pipeline raytracing"**
   - Nel log: se dopo `[DXIL] Validator succeeded` manca la riga `CreateStateObject patched ... -> hr=` il crash è nel compilatore del driver.
   - Cerca `[DXIL] WARNING: unsupported NvAPI shader extension opcode(s)`: un opcode NV neutralizzato a costante può innescare bug del compilatore AMD (caso noto: timer `NvGetSpecial` in RTXMG, vedi [translation.md](translation.md) §3).
   - Estrai il blob (`AMD_MG_DUMP_DXIL=1`) e riproduci offline con `repro_rtpso.exe`; bisezione con `--only=` e `dxil_asm.exe`.
   - Attiva `AMD_MG_DUMP_DXIL=1` e controlla l'output di `dxil_patcher.cpp`.
   - Verifica che `NvAPI_D3D12_SetNvShaderExtnSlotSpace` sia stata chiamata prima di `CreateStateObject` e che `dxcompiler.dll` + `dxil.dll` (versione SM 6.10 `v1.10.2605+`) siano accanto a `nvapi64.dll`.
4. **Quando l'utente dice: "RTXMG: `Failed to create texture Accum ... 0x80070057`, poi `Failed to grow a descriptor heap!` e crash"**
   - Non è Mega Geometry: succede anche con `--cluster-api dxr`. Streamline fallisce in `slSetD3DDevice` (`eErrorExceptionHandler`) e `QueryDLSSOptimalSettings` restituisce una render size spazzatura (es. `-793575424,556`). Con DLSS attivo le texture hanno dimensioni invalide; con `--dlssMode off` la demo ricrea i sampler a ogni frame (LOD bias che non converge) e al frame ~2047 (circa 60 s) esaurisce il descriptor heap, poi va in abort o in AV dentro `amdxc64.dll`.
   - Attenzione a usare `--cluster-api dxr` come riferimento: anche quella libreria usa `NvGetSpecial` (2 timer, quindi viene patchata), e in un run (04/11, sotto Frida) è andata in `DXGI_ERROR_DEVICE_HUNG` al primo frame. Va ancora verificato.
   - Per testare a lungo il percorso cluster nella demo: spegnere **Enable DLSS-RR** nella GUI (sezione *Denoiser and Upscaling*).
   - Il minidump di Streamline (`C:\ProgramData\NVIDIA\Streamline\rtxmg_demo\*\sl-sha-*.dmp`) contiene un `std::system_error` "device or resource busy" da `msvcp140.dll`: un `std::mutex` ribloccato dallo stesso thread. Il wrapper usa il CRT statico, quindi il mutex non è suo: è in Streamline, OptiScaler/AMD-NR (`dxgi.dll`) o nella demo. È **intermittente**: un solo dump (04/11 10:06); nei run successivi Streamline si inizializza (`DLSS is supported on this system`). Per ritrovarlo: `AMD_MG_TRACE_CXX=1` logga ogni eccezione C++ con `what()` e lo stack (modulo+offset). Le eccezioni `what="?"` da `dxcore.dll` sono normali.
   - **Causa sistematica:** `DLSS_RR is NOT supported on this adapter` perché `OptiScaler.ini` ha `[FSR-RR] RrBackend=off`. Con `off`, OptiScaler dice al gioco che RR non esiste, anche se il log di AMD-NR (`AMDNR-NeuralHub-080\dlssnr_on_amd.log`) riporta `DLSS-RR AVAILABLE (preset F)`. La demo resta però con DLSS-RR attivo, riceve una render size 0 o spazzatura e finisce nella perdita di sampler descritta sopra. `off` era stato messo il 31/10 come workaround (backup: `OptiScaler.ini.bak-rrbackend-auto`), prima del fix del crash `amdxc64` in `CreateStateObject` (vedi punto 3). Il valore originale è `auto`.
   - **Il 04/11 è stato riportato a `RrBackend=auto`** (lo stato precedente è in `OptiScaler.ini.bak-rrbackend-off`). Risultato con `--cluster-api nvapi` e DLSS-RR attivo: AMD-NR esegue `FSRDFeatureDx12::DispatchDenoiser` (`dispatch stability milestone: ... 0 failures`), oltre 14.000 operazioni cluster senza crash, ben oltre il vecchio limite di ~2047 frame. Quando modifichi `OptiScaler.ini` mantieni i CRLF: il `sed -i` di Git Bash li converte in LF.
5. **Quando l'utente dice: "Le geometrie clusterizzate sono corrotte o spariscono (`ExecuteIndirectRTASOperations`)"**
   - Controlla la conversione del buffer indiretto in [gpu_arg_converter.cpp](../src/gpu_arg_converter.cpp): ricorda che NvAPI usa argomenti indiretti da **64 byte** per `BUILD_CLAS_FROM_TRIANGLES` (contro **80 byte** di DXR 1.722) e scrive `ResultSizeArray` a **32 bit** (contro **64 bit** di DXR 1.722).
