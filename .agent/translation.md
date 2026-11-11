# Specifica di Traduzione NvAPI Mega Geometry ↔ DXR 1.722 (`translation.md`)

Questo documento raccoglie le differenze binarie esatte tra l'implementazione proprietaria **NVIDIA NvAPI RTX Mega Geometry** e lo standard **Microsoft DirectX Raytracing 1.722 (Shader Model 6.10)** verificate sui sorgenti ufficiali (`sdk/nvrhi/include/nvrhi/nvrhiHLSL.h`, `sdk/nvapi/repo/nvapi.h`, `sdk/agility/extracted/build/native/include/d3d12.idl`).

---

## 1. Tabella ID Funzioni `nvapi_QueryInterface`

| Funzione NvAPI | ID Esadecimale | Traduzione in `amd-megageometry` |
|---|---|---|
| `NvAPI_Initialize` | `0x0150e828` | Abilita `D3D12RaytracingPreview` + `D3D12ExperimentalShaderModels` e inizializza DXC |
| `NvAPI_Unload` | `0xd22bdd7e` | Rilascia risorse cache pipeline compute e DXC |
| `NvAPI_GetErrorMessage` | `0x6c2d048c` | Restituisce stringa descrittiva `NvAPI_Status` |
| `NvAPI_GetInterfaceVersionString` | `0x01053fa5` | Restituisce `"NVidia Complete Version 570.00 (amd-megageometry)"` |
| `NvAPI_D3D12_GetRaytracingCaps` | `0x85a6c2a0` | Interroga `ID3D12DeviceRaytracing2` e restituisce `CLUSTER_OPERATIONS_CAP_STANDARD` / `PARTITIONED_TLAS_CAP_STANDARD` |
| `NvAPI_D3D12_IsNvShaderExtnOpCodeSupported` | `0x3dfacec8` | Restituisce `true` per gli opcode cluster `86` e `93..99` |
| `NvAPI_D3D12_SetCreatePipelineStateOptions` | `0x5c607a27` | Abilita il tracciamento delle pipeline RT per l'iniezione di `ALLOW_CLUSTERED_GEOMETRY` |
| `NvAPI_D3D12_SetNvShaderExtnSlotSpace` | `0xac2dfeb5` | Salva `(uavSlot, uavSpace)` per il device e installa l'hook su `ID3D12Device5::CreateStateObject` |
| `NvAPI_D3D12_SetNvShaderExtnSlotSpaceLocalThread` | `0x43d867c0` | Salva `(uavSlot, uavSpace)` in `thread_local` e installa l'hook su `ID3D12Device5::CreateStateObject` |
| `NvAPI_D3D12_GetRaytracingMultiIndirectClusterOperationRequirementsInfo` | `0x5c9163f4` | Converte `NVAPI_D3D12_RAYTRACING_MULTI_INDIRECT_CLUSTER_OPERATION_INPUTS` -> `D3D12_RTAS_OPERATION_INPUTS` e chiama `ID3D12DeviceRaytracing2::GetRTASOperationPrebuildInfo` |
| `NvAPI_D3D12_RaytracingExecuteMultiIndirectClusterOperation` | `0x67c798af` | Converte il buffer indiretto GPU con compute shader, chiama `ID3D12CommandListRaytracing2::ExecuteIndirectRTASOperations` e converte `ResultSizeArray` da 64-bit a 32-bit |
| `NvAPI_D3D12_GetRaytracingPartitionedTlasIndirectPrebuildInfo` | `0xcdfdc5f2` | Traduce in `ID3D12DeviceRaytracing2::GetRTASOperationPrebuildInfo` con `D3D12_RTAS_OPERATION_TYPE_PARTITIONED_TLAS` (`0x8`) |
| `NvAPI_D3D12_BuildRaytracingPartitionedTlasIndirect` | `0x7cfc6fc3` | Converte `WriteInstanceArgs` (104B) se necessario e chiama `ID3D12CommandListRaytracing2::ExecuteIndirectRTASOperations` |

---

## 2. Confronto Layout Strutture Indirette GPU (`nvrhiHLSL.h` vs `d3d12.idl`)

### A. `MOVE_CLUSTER_OBJECT` (8 byte — Identico)
- **NvAPI (`MoveObjectsInput`)**: `{ uint64_t sourceEmptyOrClasOrClusterBlasAddress; }` (8 byte)
- **DXR 1.722 (`D3D12_RTAS_CLUSTER_MOVE_OBJECTS_ARGS`)**: `{ D3D12_GPU_VIRTUAL_ADDRESS SourceObject; }` (8 byte)
- **Azione**: Nessuna conversione GPU necessaria (zero-copy).

### B. `BUILD_BLAS_FROM_CLAS` (16 byte — Compatibile al 100%)
- **NvAPI (`ClasToBlasInput`)**:
  - `0x00`: `uint32_t clasCount`
  - `0x04`: `uint32_t reserved` (`0` per specifica NvAPI)
  - `0x08`: `uint64_t clasAddresses` (puntatore a array di `uint64_t` GPU VA)
- **DXR 1.722 (`D3D12_RTAS_CLUSTER_BUILD_BLAS_ARGS`)**:
  - `0x00`: `uint32_t ClasCount`
  - `0x04`: `uint32_t ClasAddressStride` (`0` in D3D12 equivale allo stride naturale `8`, oppure `8` esplicito)
  - `0x08`: `uint64_t ClasAddressArray`
- **Azione**: Compatibile direttamente (il nostro compute converter imposta `ClasAddressStride = 8` se `clasCount > 0`).

### C. `INSTANTIATE_CLUSTER_TEMPLATES` (32 byte — Primi 2 `uint32` invertiti!)
| Offset | NvAPI (`InstantiateTemplatesInput`) | DXR 1.722 (`D3D12_RTAS_CLUSTER_INSTANTIATE_TEMPLATE_ARGS`) |
|---|---|---|
| `0x00` | `uint32_t clusterIdOffset` | `int32_t GeometryIndexOffset` |
| `0x04` | `uint32_t geometryIndexOffset` | `uint32_t ClusterIdOffset` |
| `0x08` | `uint64_t clusterTemplate` | `uint64_t ClusterTemplate` |
| `0x10` | `uint64_t vertexBuffer.StartAddress` | `uint64_t VertexBuffer.StartAddress` |
| `0x18` | `uint64_t vertexBuffer.StrideInBytes` | `uint64_t VertexBuffer.StrideInBytes` |
- **Azione**: Il compute shader `CS_ConvertInstantiateTemplates` scambia le prime due `dword` (`offset 0` e `offset 4`).

### D. `BUILD_CLAS_FROM_TRIANGLES` (64 byte in NvAPI vs 80 byte in DXR 1.722)
| Offset | NvAPI (`TrianglesToClasInput`, 64 byte) | DXR 1.722 (`D3D12_RTAS_CLUSTER_TRIANGLES_ARGS`, 80 byte) |
|---|---|---|
| `0x00` | `uint32_t clusterId` | `uint32_t ClusterId` |
| `0x04` | `uint32_t clusterFlags` | `uint32_t ClusterFlags` |
| `0x08` | Bitfield `uint32_t`: `triangleCount:9`, `vertexCount:9`, `positionTruncateBitCount:6`, `indexFormat:4`, `ommIndexFormat:4` | `uint16_t TriangleCount`, `uint16_t UniqueVertexIndexCount` |
| `0x0C` | `uint32_t baseGeometryIndexAndFlags` | `uint32_t BaseGeometryIndexAndFlags` |
| `0x10` | `uint16_t indexBufferStride`, `uint16_t vertexBufferStride` | `uint32_t OpacityMicromapBaseLocation` (`0`) |
| `0x14` | `uint16_t geometryIndexAndFlagsBufferStride`, `uint16_t ommIndexBufferStride` | `uint16_t VertexBufferStride`, `uint16_t IndexBufferStride` |
| `0x18` | `uint64_t indexBuffer` | `uint16_t OmmIndexBufferStride`, `uint16_t GeomIndexAndFlagsArrayStride` |
| `0x1C` | *(parte di `indexBuffer`)* | `uint16_t PositionTruncateBitCount`, `uint16_t ReservedPadding` |
| `0x20` | `uint64_t vertexBuffer` | `uint64_t VertexBuffer` |
| `0x28` | `uint64_t geometryIndexAndFlagsBuffer` | `uint64_t IndexBuffer` |
| `0x30` | `uint64_t opacityMicromapArray` | `uint64_t GeometryIndexAndFlagsArray` |
| `0x38` | `uint64_t opacityMicromapIndexBuffer` | `uint64_t GeometryIndexAndFlagsIndexBuffer` (`0`) |
| `0x40` | *(fine struct NvAPI: 64B)* | `uint64_t OpacityMicromapArray` |
| `0x48` | — | `uint64_t OpacityMicromapIndexBuffer` (fine struct DXR: 80B) |

### E. `BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES` (72 byte in NvAPI vs 96 byte in DXR 1.722)
- Stessa struttura dei primi 64B di `TrianglesToClasInput` + `uint64_t instantiationBoundingBoxLimit` a offset `0x40` (totale 72B).
- In DXR 1.722 (`D3D12_RTAS_CLUSTER_BUILD_TEMPLATE_ARGS`, 96B): i primi 80B seguono `D3D12_RTAS_CLUSTER_TRIANGLES_ARGS`, seguiti da `uint64_t InstantiationBoundingBoxLimit` a offset `0x50` e `uint64_t Compressed1` (`0`) a offset `0x58`.

### F. `ResultSizeArray` (32-bit in NvAPI vs 64-bit in DXR 1.722)
- In NvAPI (`rt::cluster::nvapi::OperationSize`), ogni elemento di `resultSizeArray` è `uint32_t` (stride tipico 4 byte).
- In DXR 1.722 (`rt::cluster::dxr::OperationSize`), `ExecuteIndirectRTASOperations` scrive elementi `uint64_t` (stride minimo 8 byte).
- Il wrapper alloca uno scratch buffer a 64-bit per `ExecuteIndirectRTASOperations` e lancia `CS_DowncastResultSizes` subito dopo per copiare i `uint32_t` nel buffer del gioco.

### G. Partitioned TLAS `WRITE_INSTANCE_ARGS` (104 byte — Campi riordinati da offset `0x30`)
| Offset | NvAPI (`PartitionedTlasWriteInstanceInput`, 104B) | DXR 1.722 (`D3D12_RTAS_PARTITIONED_TLAS_WRITE_INSTANCE_ARGS`, 104B) |
|---|---|---|
| `0x00..0x2F` | `transform[3][4]` (`48B`, identico) | `Transform[3][4]` (`48B`, identico) |
| `0x30..0x47` | `float explicitAABB[6]` (`24B`) | `uint32 InstanceID:24/Mask:8`, `uint32 Contribution:24/Flags:8`, `uint32 InstanceFlags`, `uint32 InstanceIndex`, `uint64 AccelerationStructure` (`24B`) |
| `0x48..0x5F` | `instanceID`, `instanceMask`, `instanceContributionToHitGroupIndex`, `instanceFlags`, `instanceIndex`, `partitionIndex`, `accelerationStructure` (`32B`) | `uint32 PartitionIndex`, `uint32 Padding`, `float ExplicitAABB[6]` (`32B`) |

---

## 3. Traduzione Bytecode DXIL (`nvHLSLExtns.h` ↔ Shader Model 6.10)

Quando un gioco compila gli shader con `nvHLSLExtns.h`, ogni chiamata a `NvRtGetClusterID()` o `NvRtTriangleObjectPositions()` emette una sequenza atomica sulla UAV fittizia `g_NvidiaExt` (registrata tramite `NvAPI_D3D12_SetNvShaderExtnSlotSpace`):
1. `call void @dx.op.rawBufferStore.i32(i32 140, %dx.types.Handle %uav, i32 %index, i32 0, i32 <NV_EXTN_OP>, ...)` (scrive l'opcode in offset `0` della struttura `NvShaderExtnStruct`).
2. `call i32 @dx.op.bufferUpdateCounter(i32 70, %dx.types.Handle %uav, i8 1)` (1 chiamata per `ClusterID`, oppure 9 chiamate consecutive + `bitcast i32 to float` per le 3 posizioni dei vertici `v0, v1, v2`).

Il modulo [src/dxil_patcher.cpp](../src/dxil_patcher.cpp) intercetta questi pattern nell'IR DXIL e li sostituisce con le intrinseche native SM 6.10:

| Costante `nvShaderExtnEnums.h` | Valore | Intrinsica DXIL SM 6.10 Sostitutiva | DXIL OpCode (`i32`) |
|---|---|---|---|
| `NV_EXTN_OP_RT_GET_CLUSTER_ID` | `93` | `call i32 @dx.op.clusterID(i32 -2147483645)` | `0x80000003` (`-2147483645`) |
| `NV_EXTN_OP_RT_TRIANGLE_OBJECT_POSITIONS` | `86` | `call <9 x float> @dx.op.triangleObjectPosition.f32(i32 -2147483641)` + `extractelement` | `0x80000007` (`-2147483641`) |
| `NV_EXTN_OP_RT_GET_CANDIDATE_CLUSTER_ID` | `94` | `call i32 @dx.op.rayQuery_StateScalar.i32(i32 -2147483644, i32 %rq)` | `0x80000004` (`-2147483644`) |
| `NV_EXTN_OP_RT_GET_COMMITTED_CLUSTER_ID` | `95` | `call i32 @dx.op.rayQuery_StateScalar.i32(i32 -2147483643, i32 %rq)` | `0x80000005` (`-2147483643`) |
| `NV_EXTN_OP_HIT_OBJECT_GET_CLUSTER_ID` | `96` | `call i32 @dx.op.hitObject_StateScalar.i32(i32 -2147483642, %dx.types.HitObject %ho)` | `0x80000006` (`-2147483642`) |
| `NV_EXTN_OP_RT_CANDIDATE_TRIANGLE_OBJECT_POSITIONS` | `97` | `call <9 x float> @dx.op.rayQuery_CandidateTriangleObjectPosition.f32(i32 -2147483640, i32 %rq)` | `0x80000008` (`-2147483640`) |
| `NV_EXTN_OP_RT_COMMITTED_TRIANGLE_OBJECT_POSITIONS` | `98` | `call <9 x float> @dx.op.rayQuery_CommittedTriangleObjectPosition.f32(i32 -2147483639, i32 %rq)` | `0x80000009` (`-2147483639`) |
| `NV_EXTN_OP_HIT_OBJECT_GET_TRIANGLE_OBJECT_POSITIONS` | `99` | `call <9 x float> @dx.op.hitObject_TriangleObjectPosition.f32(i32 -2147483638, %dx.types.HitObject %ho)` | `0x8000000A` (`-2147483638`) |
| `NV_EXTN_OP_GET_SPECIAL` + `NV_SPECIALOP_THREADLTMASK` | `19` / sub-op `4` | `(1 << (WaveGetLaneIndex() & 31)) - 1` | `111` (`waveGetLaneIndex`) |
| `NV_EXTN_OP_GET_SPECIAL` + `NV_SPECIALOP_GLOBAL_TIMER_LO/HI` | `19` / sub-op `9`/`10` | `call i32 @dx.op.waveGetLaneIndex(i32 111)` (segnaposto **non costante**, DXIL non ha un clock) | `111` |

### Note su `NV_EXTN_OP_GET_SPECIAL` (opcode 19)
- Il sub-opcode arriva dalla store a offset `76` (`src0u`) sulla stessa slot del counter, come per RayQuery/HitObject.
- RTXMG (`RayGen` della libreria path-tracing) legge `NvGetSpecial(NV_SPECIALOP_GLOBAL_TIMER_LO)` due volte per la "timeview". Se il timer diventa una **costante** (`0`, sia come `add i32 0, 0` sia come letterale), `amdxc64.dll` 32.0.32012.1013 va in access violation dentro `CreateStateObject` (riprodotto offline con `repro_rtpso.exe`, bisezione: RayGen + qualunque closest-hit che usa `RayPayload`). Con un valore non costante (`WaveGetLaneIndex`, `DispatchRaysIndex`) compila. Per questo il timer è un segnaposto non costante: la timeview mostra 0, ma la pipeline compila.
- `waveGetLaneIndex` richiede il bit WaveOps (`0x80000`) nei flag del modulo (`!{i32 0, i64 <flags>}` nelle proprietà dell'entry `null` di `!dx.entryPoints`), altrimenti il validatore fallisce con "Flags must match usage": il patcher lo aggiunge.
- Gli opcode NV non gestiti vengono comunque neutralizzati a `0` (la UAV fittizia non esiste su AMD), ma ora compaiono nel log come `[DXIL] WARNING: unsupported NvAPI shader extension opcode(s) neutralized to 0: ...`.
- Se il validatore rifiuta il container patchato, `PatchDxilClusterShader` restituisce `false` e il driver riceve lo shader originale (prima il container non valido veniva usato lo stesso).
