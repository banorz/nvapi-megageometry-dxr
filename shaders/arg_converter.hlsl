// ============================================================================
// amd-megageometry: GPU Indirect Argument Converter (NvAPI -> DXR 1.722)
// ============================================================================

cbuffer ConverterConstants : register(b0)
{
    uint g_MaxOperations;
    uint g_SrcStride;
    uint g_DstStride;
    uint g_Mode; // 11 = whole-array diagnostic copy (layout capture); 0 = ClasFromTriangles (64->80), 1 = TemplatesFromTriangles (72->96),
                 // 2 = InstantiateTemplates (32->32), 3 = BlasFromClas (16->16),
                 // 4 = PtlasWriteInstance (104->104), 5 = DowncastResultSizes (64b->32b),
                 // 6 = raw dword copy (diagnostic argument dump), 7/8/9 = diagnostic full-array scan,
                 // 10 = destination address array copy with null entries redirected
    uint g_HasCount; // 1 = g_ArgCount holds the GPU-written argument count (NvAPI indirectArgCount)
    uint g_Extra0;   // mode 10: fallback destination address (lo)
    uint g_Extra1;   // mode 10: fallback destination address (hi)
};

ByteAddressBuffer   g_SrcArgs  : register(t0);
ByteAddressBuffer   g_ArgCount : register(t1);
RWByteAddressBuffer g_DstArgs  : register(u0);

// AMD_MG_CAPTURE_REAL ExecuteIndirect command (64 B): constants(7: maxOps,4,4,6,0,0,0),
// root SRV (2), root UAV (2), dispatch (3), pad (2). The runtime requires the constants first.
void WriteCopyCmd(uint cm, uint dw, uint64_t src, uint64_t dst, uint groups)
{
    g_DstArgs.Store4(cm + 0,  uint4(dw, 4u, 4u, 6u));
    g_DstArgs.Store4(cm + 16, uint4(0u, 0u, 0u, uint(src)));
    g_DstArgs.Store4(cm + 32, uint4(uint(src >> 32), uint(dst), uint(dst >> 32), groups));
    g_DstArgs.Store4(cm + 48, uint4(1u, 1u, 0u, 0u));
}

[numthreads(64, 1, 1)]
void CS_ConvertArgs(uint3 dtid : SV_DispatchThreadID)
{
    uint idx = dtid.x;
    if (g_Mode == 13 || g_Mode == 14)
    {
        // AMD_MG_CAPTURE_REAL: GPU-side plan of the real-data capture (one thread per CLAS slot s).
        // t1 = parameter block (dwords, see RecordRealCapture), u0 = plan buffer.
        // Mode 13 (t0 = layout capture buffer): finds the instance and template of CLAS s of the
        //   first BLAS and writes an ExecuteIndirect copy command for the template's indices.
        // Mode 14 (t0 = data buffer): reads the copied indices, finds their range and writes copy
        //   commands for the instance and template-hint vertices of that range.
        // Commands: see WriteCopyCmd (inert = 0 dwords, 0 groups, addresses at the data buffer).
        uint s = idx;
        if (s >= g_MaxOperations)
            return;
        uint nBlas    = g_ArgCount.Load(0 * 4);
        uint maxClas  = g_ArgCount.Load(1 * 4);
        uint dataLo   = g_ArgCount.Load(20 * 4), dataHi = g_ArgCount.Load(21 * 4);
        uint dataCap  = g_ArgCount.Load(22 * 4);
        uint cmdAOff  = g_ArgCount.Load(23 * 4);
        uint cmdBOff  = g_ArgCount.Load(24 * 4);
        uint recOff   = g_ArgCount.Load(25 * 4);
        uint cntOff   = g_ArgCount.Load(26 * 4);
        uint winCap   = g_ArgCount.Load(27 * 4);
        uint rec2Off  = g_ArgCount.Load(28 * 4);
        uint64_t dataVa = (uint64_t(dataHi) << 32) | dataLo;
        uint rec  = recOff + s * 64u;
        uint rec2 = rec2Off + s * 64u;

        if (g_Mode == 13)
        {
            uint cmd = cmdAOff + s * 64u;
            // Inert command until proven otherwise.
            WriteCopyCmd(cmd, 0u, dataVa, dataVa, 0u);
            g_DstArgs.Store4(rec + 0,  uint4(0u, 0u, 0u, 0u));
            uint bArgOff = g_ArgCount.Load(2 * 4), bArgStride = g_ArgCount.Load(3 * 4);
            uint bCountOff = g_ArgCount.Load(4 * 4), bArgCount = g_ArgCount.Load(5 * 4);
            uint extraOff = g_ArgCount.Load(6 * 4);
            uint64_t extraVa = (uint64_t(g_ArgCount.Load(8 * 4)) << 32) | g_ArgCount.Load(7 * 4);
            uint extraBytes = g_ArgCount.Load(9 * 4);
            uint tArgOff = g_ArgCount.Load(10 * 4), tArgStride = g_ArgCount.Load(11 * 4);
            uint tDstOff = g_ArgCount.Load(12 * 4), tDstStride = g_ArgCount.Load(13 * 4);
            uint tCount  = g_ArgCount.Load(14 * 4);
            uint iArgOff = g_ArgCount.Load(16 * 4), iArgStride = g_ArgCount.Load(17 * 4);
            uint iCount  = g_ArgCount.Load(18 * 4), iCountOff = g_ArgCount.Load(19 * 4);
            if (bCountOff != 0xFFFFFFFFu) bArgCount = min(bArgCount, g_SrcArgs.Load(bCountOff));
            if (iCountOff != 0xFFFFFFFFu) iCount = min(iCount, g_SrcArgs.Load(iCountOff));
            // CLAS slot s -> (BLAS b, reference j).
            uint acc = 0u, b = 0xFFFFFFFFu, j = 0u;
            for (uint i = 0u; i < nBlas && i < bArgCount; ++i)
            {
                uint cnt = g_SrcArgs.Load(bArgOff + i * bArgStride);
                if (s < acc + cnt) { b = i; j = s - acc; break; }
                acc += cnt;
            }
            if (b == 0xFFFFFFFFu)
                return;
            uint2 arr2 = g_SrcArgs.Load2(bArgOff + b * bArgStride + 8u);
            uint64_t arr = (uint64_t(arr2.y) << 32) | arr2.x;
            if (arr < extraVa || ((arr - extraVa) & 7u) != 0u)
                return;
            uint64_t kk = (arr - extraVa) / 8u + j;
            if (kk * 8u >= extraBytes || kk >= iCount)
                return;
            uint k = uint(kk);
            // The BLAS reads the instantiate destination array: entry k is CLAS k of the instantiate.
            uint io = iArgOff + k * iArgStride;
            uint2 tp2 = g_SrcArgs.Load2(io + 8u);
            uint2 vb2 = g_SrcArgs.Load2(io + 16u);
            uint  vs  = g_SrcArgs.Load(io + 24u);
            g_DstArgs.Store4(rec + 0,  uint4(1u, b | (j << 8), 0u, k));
            g_DstArgs.Store4(rec2 + 0, uint4(vb2.x, vb2.y, vs, 0u));
            uint t = 0xFFFFFFFFu;
            for (uint q = 0u; q < tCount; ++q)
            {
                uint2 d = g_SrcArgs.Load2(tDstOff + q * tDstStride);
                if (d.x == tp2.x && d.y == tp2.y) { t = q; break; }
            }
            g_DstArgs.Store(rec + 16u, t);
            if (t == 0xFFFFFFFFu)
                return;
            uint to = tArgOff + t * tArgStride;
            uint packed  = g_SrcArgs.Load(to + 8u);
            uint strides = g_SrcArgs.Load(to + 16u);
            uint2 ib2    = g_SrcArgs.Load2(to + 24u);
            uint2 hint2  = g_SrcArgs.Load2(to + 32u);
            uint tri     = packed & 0x1FFu;
            uint fmt     = (packed >> 24) & 0xFu;
            uint isz     = fmt == 2u ? 2u : (fmt == 4u ? 4u : 1u);
            uint istr    = (strides & 0xFFFFu) != 0u ? (strides & 0xFFFFu) : isz;
            uint tvs     = strides >> 16;
            g_DstArgs.Store4(rec2 + 16u, uint4(hint2.x, hint2.y, tvs, 0u));
            uint64_t ib = (uint64_t(ib2.y) << 32) | ib2.x;
            if (tri == 0u || ib == 0u)
            {
                g_DstArgs.Store(rec + 0u, 3u);
                return;
            }
            uint bytes = (tri * 3u - 1u) * istr + isz;
            uint64_t ibStart = ib & ~uint64_t(3);
            uint delta = uint(ib - ibStart);
            uint dw = (delta + bytes + 3u) / 4u + 1u;
            uint off;
            g_DstArgs.InterlockedAdd(cntOff, dw * 4u, off);
            if (off + dw * 4u > dataCap)
            {
                g_DstArgs.Store(rec + 0u, 3u | 64u);
                return;
            }
            WriteCopyCmd(cmd, dw, ibStart, dataVa + off, (dw + 63u) / 64u);
            g_DstArgs.Store4(rec + 0,  uint4(7u, b | (j << 8), tri, k));
            g_DstArgs.Store4(rec + 16, uint4(t, delta | (istr << 8) | (isz << 16), off, dw * 4u));
            return;
        }

        // Mode 14: vertex windows.
        uint cmdB = cmdBOff + s * 64u;
        uint cmdC = cmdBOff + (maxClas + s) * 64u;
        WriteCopyCmd(cmdB, 0u, dataVa, dataVa, 0u);
        WriteCopyCmd(cmdC, 0u, dataVa, dataVa, 0u);
        uint flags = g_DstArgs.Load(rec + 0u);
        if ((flags & 4u) == 0u)
            return;
        uint tri  = g_DstArgs.Load(rec + 8u);
        uint4 r4  = g_DstArgs.Load4(rec + 16u);
        uint delta = r4.y & 0xFFu, istr = (r4.y >> 8) & 0xFFu, isz = (r4.y >> 16) & 0xFFu;
        uint ibOff = r4.z;
        uint lo = 0xFFFFFFFFu, hi = 0u;
        for (uint i = 0u; i < tri * 3u; ++i)
        {
            uint a  = ibOff + delta + i * istr;
            uint al = a & ~3u;
            uint sh = (a & 3u) * 8u;
            uint64_t w = (uint64_t(g_SrcArgs.Load(al + 4u)) << 32) | g_SrcArgs.Load(al);
            uint v = uint(w >> sh);
            v = isz == 1u ? (v & 0xFFu) : (isz == 2u ? (v & 0xFFFFu) : v);
            lo = min(lo, v);
            hi = max(hi, v);
        }
        g_DstArgs.Store2(rec + 32u, uint2(lo, hi));
        uint4 v4 = g_DstArgs.Load4(rec2 + 0u);
        uint4 h4 = g_DstArgs.Load4(rec2 + 16u);
        [unroll] for (uint c2 = 0u; c2 < 2u; ++c2)
        {
            uint64_t base = c2 == 0u ? ((uint64_t(v4.y) << 32) | v4.x) : ((uint64_t(h4.y) << 32) | h4.x);
            uint stride   = c2 == 0u ? v4.z : (h4.z != 0u ? h4.z : v4.z);
            if (base == 0u || stride == 0u)
                continue;
            uint64_t start = base + uint64_t(lo) * stride;
            uint64_t startA = start & ~uint64_t(3);
            uint d = uint(start - startA);
            uint64_t want = uint64_t(hi - lo + 1u) * stride + d;
            uint clamped = want > winCap ? 1u : 0u;
            uint bytes = uint(min(want, uint64_t(winCap)));
            uint dw = (bytes + 3u) / 4u;
            uint off;
            g_DstArgs.InterlockedAdd(cntOff, dw * 4u, off);
            if (off + dw * 4u > dataCap)
            {
                g_DstArgs.InterlockedOr(rec + 0u, 64u);
                continue;
            }
            WriteCopyCmd(c2 == 0u ? cmdB : cmdC, dw, startA, dataVa + off, (dw + 63u) / 64u);
            // rec: [10] vb off [11] vb bytes [12] vb delta | [13] hint off [14] hint bytes [15] hint delta
            g_DstArgs.Store3(rec + (c2 == 0u ? 40u : 52u), uint3(off, bytes, d));
            g_DstArgs.InterlockedOr(rec + 0u, (c2 == 0u ? 8u : 16u) | (clamped != 0u ? 32u : 0u));
        }
        return;
    }
    if (g_Mode == 11)
    {
        // Diagnostic raw copy of a whole strided array (AMD_MG_DUMP_ARGS layout capture):
        // g_MaxOperations dwords, element stride g_SrcStride bytes, limited to the GPU element
        // count when present so arrays sized for that count are never over-read.
        uint elems = g_HasCount != 0u ? g_ArgCount.Load(0) : 0xFFFFFFFFu;
        if (idx < g_MaxOperations && (idx * 4u) / max(g_SrcStride, 4u) < elems)
            g_DstArgs.Store(idx * 4u, g_SrcArgs.Load(idx * 4u));
        return;
    }
    if (g_Mode == 12)
    {
        // Split execution (AMD_MG_SPLIT_OPS): element count of chunk idx, chunk size g_Extra0,
        // total = min(GPU count, g_Extra1 = maxArgCount). g_MaxOperations = number of chunks.
        if (idx < g_MaxOperations)
        {
            uint total = g_Extra1;
            if (g_HasCount != 0u)
                total = min(total, g_ArgCount.Load(0));
            uint first = idx * g_Extra0;
            g_DstArgs.Store(idx * 4u, total > first ? min(total - first, g_Extra0) : 0u);
        }
        return;
    }
    // Never touch more elements than the game actually provided: with a GPU-side count the
    // source/destination arrays may be sized for that count only, not for maxArgCount.
    uint count = g_MaxOperations;
    if (g_HasCount != 0u)
        count = min(count, g_ArgCount.Load(0));
    if (idx >= count)
        return;

    uint srcOff = idx * g_SrcStride;
    uint dstOff = idx * g_DstStride;

    if (g_Mode == 0 || g_Mode == 1)
    {
        // BUILD_CLAS_FROM_TRIANGLES (64B -> 80B) or BUILD_CLUSTER_TEMPLATES_FROM_TRIANGLES (72B -> 96B)
        uint4 w0 = g_SrcArgs.Load4(srcOff + 0x00); // clusterId, clusterFlags, packedBitfield, baseGeomIndexAndFlags
        uint2 w1 = g_SrcArgs.Load2(srcOff + 0x10); // strides0, strides1
        uint4 w2 = g_SrcArgs.Load4(srcOff + 0x18); // indexBuffer (lo,hi), vertexBuffer (lo,hi)
        uint4 w3 = g_SrcArgs.Load4(srcOff + 0x28); // geomIndexBuffer (lo,hi), ommArray (lo,hi)
        uint2 w4 = g_SrcArgs.Load2(srcOff + 0x38); // ommIndexBuffer (lo,hi)

        uint packed = w0.z;
        uint triCount     = (packed >> 0)  & 0x1FFu;
        uint vtxCount     = (packed >> 9)  & 0x1FFu;
        uint posTruncBits = (packed >> 18) & 0x3Fu;

        uint dxrTriAndVtx = (triCount & 0xFFFFu) | ((vtxCount & 0xFFFFu) << 16);

        // NvAPI declares the index format per argument (packed bits 24..27: 1/2/4 bytes), DXR per
        // operation (the wrapper always declares UINT8). Cluster-local indices are < 256, so
        // reading the low byte of each 16/32-bit index with the element stride of the real
        // format yields the right value: make that stride explicit (NvAPI 0 = index size).
        uint idxFmt      = (packed >> 24) & 0xFu;
        uint indexStride = w1.x & 0xFFFFu;
        if (indexStride == 0u)
            indexStride = (idxFmt == 2u || idxFmt == 4u) ? idxFmt : 1u;

        // NvAPI w1.x = indexStride (lo16) | vertexStride (hi16)
        // DXR   0x14 = vertexStride (lo16) | indexStride (hi16)
        uint dxrStrides0 = ((w1.x >> 16) & 0xFFFFu) | (indexStride << 16);

        // NvAPI w1.y = geomStride (lo16) | ommStride (hi16)
        // DXR   0x18 = ommStride (lo16) | geomStride (hi16)
        uint dxrStrides1 = ((w1.y >> 16) & 0xFFFFu) | ((w1.y & 0xFFFFu) << 16);

        uint dxrTruncAndPad = posTruncBits & 0xFFFFu;

        // AMD_MG_FORCE_GEOM0 (g_Extra0 bit 0): geometry index 0, geometry flags kept.
        if ((g_Extra0 & 1u) != 0u)
            w0.w &= 0xFF000000u;
        // AMD_MG_TEMPLATE_NO_HINT (g_Extra0 bit 1): templates get no vertex buffer (the NvAPI
        // template vertexBuffer is an optional hint; instances supply the real positions).
        if (g_Mode == 1 && (g_Extra0 & 2u) != 0u)
            w2.zw = uint2(0u, 0u);

        // 0x00..0x0F: ClusterID, ClusterFlags, TriangleCount|UniqueVertexIndexCount, BaseGeometryIndexAndFlags
        g_DstArgs.Store4(dstOff + 0x00, uint4(w0.x, w0.y, dxrTriAndVtx, w0.w));
        // 0x10..0x1F: OpacityMicromapBaseLocation(0), Vertex|IndexStride, Omm|GeomStride, PositionTruncateBitCount|Pad
        g_DstArgs.Store4(dstOff + 0x10, uint4(0u, dxrStrides0, dxrStrides1, dxrTruncAndPad));
        // 0x20..0x2F: VertexBuffer (w2.zw), IndexBuffer (w2.xy)
        g_DstArgs.Store4(dstOff + 0x20, uint4(w2.z, w2.w, w2.x, w2.y));
        // 0x30..0x3F: GeometryIndexAndFlagsArray (w3.xy), GeometryIndexAndFlagsIndexBuffer (0, 0)
        g_DstArgs.Store4(dstOff + 0x30, uint4(w3.x, w3.y, 0u, 0u));
        // 0x40..0x4F: OpacityMicromapArray (w3.zw), OpacityMicromapIndexBuffer (w4.xy)
        g_DstArgs.Store4(dstOff + 0x40, uint4(w3.z, w3.w, w4.x, w4.y));

        if (g_Mode == 1)
        {
            uint2 bboxLimit = g_SrcArgs.Load2(srcOff + 0x40);
            g_DstArgs.Store4(dstOff + 0x50, uint4(bboxLimit.x, bboxLimit.y, 0u, 0u));
        }
    }
    else if (g_Mode == 2)
    {
        // INSTANTIATE_CLUSTER_TEMPLATES (32B -> 32B, swap first two dwords)
        uint4 w0 = g_SrcArgs.Load4(srcOff + 0x00);
        uint4 w1 = g_SrcArgs.Load4(srcOff + 0x10);
        g_DstArgs.Store4(dstOff + 0x00, uint4(w0.y, w0.x, w0.z, w0.w));
        g_DstArgs.Store4(dstOff + 0x10, w1);
    }
    else if (g_Mode == 3)
    {
        // BUILD_BLAS_FROM_CLAS (16B -> 16B). NvAPI dword 1 is "reserved, must be 0" and the CLAS
        // address array always has an 8-byte stride: never trust the game's dword there.
        uint4 w0 = g_SrcArgs.Load4(srcOff + 0x00);
        if ((g_Extra0 | g_Extra1) != 0u)
        {
            // AMD_MG_BLAS_FAKE_CLAS: every non-empty BLAS is built from the wrapper's single
            // one-triangle CLAS (address array at g_Extra) instead of the game's CLAS.
            g_DstArgs.Store4(dstOff + 0x00, uint4(w0.x != 0u ? 1u : 0u, 8u, g_Extra0, g_Extra1));
        }
        else
        {
            g_DstArgs.Store4(dstOff + 0x00, uint4(w0.x, 8u, w0.z, w0.w));
        }
    }
    else if (g_Mode == 4)
    {
        // D3D12_RTAS_PARTITIONED_TLAS_OPERATION_WRITE_INSTANCE_ARGS (104B -> 104B)
        // NvAPI: transform(48B), userAABB(24B), instanceID(4B), instanceMask(4B), hitGroup(4B), flags(4B), instanceIdx(4B), partitionIdx(4B), accelStruct(8B)
        // DXR:   Transform(48B), InstanceID(4B), InstanceMask(4B), HitGroup(4B), Flags(4B), AccelStruct(8B), InstanceIdx(4B), PartitionIdx(4B), ExplicitAABB(24B)
        uint4 t0 = g_SrcArgs.Load4(srcOff + 0x00);
        uint4 t1 = g_SrcArgs.Load4(srcOff + 0x10);
        uint4 t2 = g_SrcArgs.Load4(srcOff + 0x20);
        uint4 aabb0 = g_SrcArgs.Load4(srcOff + 0x30);
        uint2 aabb1 = g_SrcArgs.Load2(srcOff + 0x40);
        uint4 meta0 = g_SrcArgs.Load4(srcOff + 0x48); // instanceID, instanceMask, hitGroupContrib, instanceFlags
        uint4 meta1 = g_SrcArgs.Load4(srcOff + 0x58); // instanceIndex, partitionIndex, accelStructLo, accelStructHi

        g_DstArgs.Store4(dstOff + 0x00, t0);
        g_DstArgs.Store4(dstOff + 0x10, t1);
        g_DstArgs.Store4(dstOff + 0x20, t2);
        g_DstArgs.Store4(dstOff + 0x30, meta0);
        g_DstArgs.Store4(dstOff + 0x40, uint4(meta1.z, meta1.w, meta1.x, meta1.y));
        g_DstArgs.Store4(dstOff + 0x50, aabb0);
        g_DstArgs.Store2(dstOff + 0x60, aabb1);
    }
    else if (g_Mode == 6)
    {
        // Diagnostic raw copy (AMD_MG_DUMP_ARGS): g_MaxOperations dwords, strides ignored.
        g_DstArgs.Store(idx * 4u, g_SrcArgs.Load(idx * 4u));
    }
    else if (g_Mode == 7)
    {
        // Diagnostic scan of every argument (AMD_MG_DUMP_ARGS). g_DstStride = NvAPI operation type.
        // Statistics (dwords) are accumulated in g_DstArgs, initialised by mode 9:
        //  0 scanned | 1 empty (tri=0 / template=0 / clasCount=0) | 2 vtx=0 | 3 null main pointer
        //  4 null vertex buffer | 5 counts > 256 | 6 bad index format / instantiate stride 0
        //  7 first bad idx | 8 last bad idx | 9/10 min/max hi32 main pointer | 11 max count | 12 sum
        //  13 min alignment bit of main pointer
        uint type = g_DstStride;
        bool bad = false;
        uint2 ptr = uint2(0u, 0u);
        uint mag = 0u;
        if (type == 2u || type == 3u)
        {
            uint packed = g_SrcArgs.Load(srcOff + 8u);
            uint tri = packed & 0x1FFu, vtx = (packed >> 9) & 0x1FFu, fmt = (packed >> 24) & 0xFu;
            ptr = g_SrcArgs.Load2(srcOff + 24u);
            uint2 vb = g_SrcArgs.Load2(srcOff + 32u);
            uint dummy;
            if (tri == 0u) { g_DstArgs.InterlockedAdd(4u, 1u, dummy); bad = true; }
            if (vtx == 0u) { g_DstArgs.InterlockedAdd(8u, 1u, dummy); bad = true; }
            if (ptr.x == 0u && ptr.y == 0u) { g_DstArgs.InterlockedAdd(12u, 1u, dummy); bad = true; }
            if (vb.x == 0u && vb.y == 0u) g_DstArgs.InterlockedAdd(16u, 1u, dummy);
            if (tri > 256u || vtx > 256u) { g_DstArgs.InterlockedAdd(20u, 1u, dummy); bad = true; }
            if (!(fmt == 1u || fmt == 2u || fmt == 4u)) { g_DstArgs.InterlockedAdd(24u, 1u, dummy); bad = true; }
            mag = tri;
        }
        else if (type == 4u)
        {
            ptr = g_SrcArgs.Load2(srcOff + 8u);
            uint2 vb = g_SrcArgs.Load2(srcOff + 16u);
            uint vs = g_SrcArgs.Load(srcOff + 24u);
            uint dummy;
            if (ptr.x == 0u && ptr.y == 0u) { g_DstArgs.InterlockedAdd(4u, 1u, dummy); bad = true; }
            if (vb.x == 0u && vb.y == 0u) { g_DstArgs.InterlockedAdd(16u, 1u, dummy); bad = true; }
            if (vs == 0u) g_DstArgs.InterlockedAdd(24u, 1u, dummy);
        }
        else if (type == 1u)
        {
            uint cnt = g_SrcArgs.Load(srcOff);
            ptr = g_SrcArgs.Load2(srcOff + 8u);
            uint dummy;
            if (cnt == 0u) g_DstArgs.InterlockedAdd(4u, 1u, dummy);
            if (ptr.x == 0u && ptr.y == 0u) { g_DstArgs.InterlockedAdd(12u, 1u, dummy); bad = cnt != 0u; }
            if (cnt > 65536u) { g_DstArgs.InterlockedAdd(20u, 1u, dummy); bad = true; }
            mag = cnt;
        }
        uint d0;
        g_DstArgs.InterlockedAdd(0u, 1u, d0);
        if (bad)
        {
            g_DstArgs.InterlockedMin(28u, idx, d0);
            g_DstArgs.InterlockedMax(32u, idx, d0);
        }
        if (ptr.x != 0u || ptr.y != 0u)
        {
            g_DstArgs.InterlockedMin(36u, ptr.y, d0);
            g_DstArgs.InterlockedMax(40u, ptr.y, d0);
            g_DstArgs.InterlockedMin(52u, ptr.x != 0u ? firstbitlow(ptr.x) : 32u, d0);
        }
        g_DstArgs.InterlockedMax(44u, mag, d0);
        g_DstArgs.InterlockedAdd(48u, mag, d0);
    }
    else if (g_Mode == 8)
    {
        // Diagnostic scan of the destination address array (stride g_SrcStride):
        //  16 null | 17 first null idx | 18/19 min/max hi32 | 20 min alignment bit
        uint2 d = g_SrcArgs.Load2(srcOff);
        uint d0;
        if (d.x == 0u && d.y == 0u)
        {
            g_DstArgs.InterlockedAdd(64u, 1u, d0);
            g_DstArgs.InterlockedMin(68u, idx, d0);
        }
        else
        {
            g_DstArgs.InterlockedMin(72u, d.y, d0);
            g_DstArgs.InterlockedMax(76u, d.y, d0);
            g_DstArgs.InterlockedMin(80u, d.x != 0u ? firstbitlow(d.x) : 32u, d0);
        }
    }
    else if (g_Mode == 10)
    {
        // Explicit destinations: the game may leave unused slots with a null address (NVIDIA
        // tolerates it, the AMD driver writes there anyway and faults the GPU). Copy the array
        // (stride g_SrcStride -> 8) and send null entries to a scratch buffer nobody reads.
        uint2 d = g_SrcArgs.Load2(srcOff);
        if (d.x == 0u && d.y == 0u)
            d = uint2(g_Extra0, g_Extra1);
        g_DstArgs.Store2(idx * 8u, d);
    }
    else if (g_Mode == 9)
    {
        // Initialise the 32 statistic dwords of modes 7/8 (min fields start at ~0).
        if (idx < 32u)
            g_DstArgs.Store(idx * 4u, (idx == 7u || idx == 9u || idx == 13u || idx == 17u || idx == 18u || idx == 20u) ? 0xFFFFFFFFu : 0u);
    }
    else if (g_Mode == 5)
    {
        // Downcast 64-bit DXR OperationSize -> 32-bit NvAPI OperationSize
        uint lo = g_SrcArgs.Load(srcOff);
        if (lo == 0u)
            lo = 4096u;
        g_DstArgs.Store(dstOff, lo);
    }
}
