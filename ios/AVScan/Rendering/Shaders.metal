#include <metal_stdlib>
using namespace metal;

struct PointVertexIn {
    packed_float3 position;
    packed_uchar4 color;
    float intensity;
};

struct SceneUniforms {
    float4x4 viewProjectionMatrix;
    float pointSize;
    int colorMode; // 0 = RGB, 1 = Intensity, 2 = Elevation Z, 3 = Quality
    float minZ;
    float maxZ;
};

struct PointVertexOut {
    float4 position [[position]];
    float4 color;
    float pointSize [[point_size]];
};

float3 jetColormap(float t) {
    float x = clamp(t, 0.0f, 1.0f);
    float r = clamp(1.5f - abs(4.0f * x - 3.0f), 0.0f, 1.0f);
    float g = clamp(1.5f - abs(4.0f * x - 2.0f), 0.0f, 1.0f);
    float b = clamp(1.5f - abs(4.0f * x - 1.0f), 0.0f, 1.0f);
    return float3(r, g, b);
}

vertex PointVertexOut pointCloudVertex(
    uint vertexID [[vertex_id]],
    constant PointVertexIn* vertices [[buffer(0)]],
    constant SceneUniforms& uniforms [[buffer(1)]])
{
    PointVertexIn in = vertices[vertexID];
    PointVertexOut out;
    float3 pos = float3(in.position);
    out.position = uniforms.viewProjectionMatrix * float4(pos, 1.0f);
    out.pointSize = uniforms.pointSize;

    if (uniforms.colorMode == 1) {
        float normI = clamp(in.intensity / 255.0f, 0.0f, 1.0f);
        out.color = float4(jetColormap(normI), 1.0f);
    } else if (uniforms.colorMode == 2) {
        float span = max(0.5f, uniforms.maxZ - uniforms.minZ);
        float normZ = clamp((pos.z - uniforms.minZ) / span, 0.0f, 1.0f);
        out.color = float4(jetColormap(normZ), 1.0f);
    } else {
        out.color = float4(float(in.color.r) / 255.0f,
                           float(in.color.g) / 255.0f,
                           float(in.color.b) / 255.0f,
                           1.0f);
    }
    return out;
}

fragment float4 pointCloudFragment(
    PointVertexOut in [[stage_in]],
    float2 pointCoord [[point_coord]])
{
    float2 centered = pointCoord * 2.0f - 1.0f;
    if (dot(centered, centered) > 1.0f) {
        discard_fragment();
    }
    return in.color;
}
