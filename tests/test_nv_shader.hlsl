#define NV_SHADER_EXTN_SLOT u63
#define NV_SHADER_EXTN_REGISTER_SPACE space0
#include "nvHLSLExtns.h"

struct [raypayload] Payload {
    uint   clusterId : write(closesthit) : read(caller);
    float3 posSum    : write(closesthit) : read(caller);
};

[shader("closesthit")]
void ClosestHitNV(inout Payload p, in BuiltInTriangleIntersectionAttributes attr) {
    uint cid = NvRtGetClusterID();
    float3x3 pos = NvRtTriangleObjectPositions();
    p.clusterId = cid;
    p.posSum    = pos[0] + pos[1] + pos[2];
}
