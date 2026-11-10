#import "MetalPointCloudRenderer.h"
#include <algorithm>
#include <cmath>
#include <mutex>
#include <simd/simd.h>

namespace {

#pragma pack(push, 1)
struct GpuPointVertex {
    float x, y, z;
    uint8_t r, g, b, a;
    float intensity;
};
#pragma pack(pop)

struct GpuSceneUniforms {
    simd_float4x4 viewProjectionMatrix;
    float pointSize;
    int colorMode;
    float minZ;
    float maxZ;
};

static NSString *const kEmbeddedMetalShaderSource = @R"(
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
    int colorMode;
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
)";

simd_float4x4 makePerspectiveMatrix(float fovYRad, float aspect, float nearZ, float farZ) {
    float ys = 1.0f / std::tan(fovYRad * 0.5f);
    float xs = ys / std::max(0.1f, aspect);
    float zs = farZ / (nearZ - farZ);
    simd_float4 c0 = {xs, 0.0f, 0.0f, 0.0f};
    simd_float4 c1 = {0.0f, ys, 0.0f, 0.0f};
    simd_float4 c2 = {0.0f, 0.0f, zs, -1.0f};
    simd_float4 c3 = {0.0f, 0.0f, nearZ * zs, 0.0f};
    return simd_matrix(c0, c1, c2, c3);
}

simd_float4x4 makeLookAtMatrix(simd_float3 eye, simd_float3 target, simd_float3 up) {
    simd_float3 z = simd_normalize(eye - target);
    simd_float3 x = simd_normalize(simd_cross(up, z));
    simd_float3 y = simd_cross(z, x);

    simd_float4 c0 = {x.x, y.x, z.x, 0.0f};
    simd_float4 c1 = {x.y, y.y, z.y, 0.0f};
    simd_float4 c2 = {x.z, y.z, z.z, 0.0f};
    simd_float4 c3 = {-simd_dot(x, eye), -simd_dot(y, eye), -simd_dot(z, eye), 1.0f};
    return simd_matrix(c0, c1, c2, c3);
}

}  // namespace

@implementation MetalPointCloudRenderer {
    id<MTLDevice> _device;
    id<MTLCommandQueue> _commandQueue;
    id<MTLRenderPipelineState> _pointPipeline;
    id<MTLDepthStencilState> _depthState;
    id<MTLBuffer> _vertexBuffer;
    size_t _vertexCount;
    size_t _bufferCapacity;
    float _minZ;
    float _maxZ;
    simd_float3 _cameraTarget;
    std::mutex _bufferMutex;
}

- (instancetype)initWithMetalKitView:(MTKView *)mtkView {
    self = [super init];
    if (self) {
        _device = mtkView.device ?: MTLCreateSystemDefaultDevice();
        mtkView.device = _device;
        mtkView.colorPixelFormat = MTLPixelFormatBGRA8Unorm;
        mtkView.depthStencilPixelFormat = MTLPixelFormatDepth32Float;
        mtkView.clearColor = MTLClearColorMake(0.06, 0.08, 0.11, 1.0);
        mtkView.preferredFramesPerSecond = 30;

        _commandQueue = [_device newCommandQueue];
        _colorMode = 0;
        _pointSize = 4.0f;
        [self resetCamera];

        NSError *error = nil;
        id<MTLLibrary> library = [_device newDefaultLibrary];
        if (!library) {
            // Runtime compilation fallback for Windows TheosWin cross-compiled builds
            MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
            library = [_device newLibraryWithSource:kEmbeddedMetalShaderSource options:opts error:&error];
        }

        if (library) {
            id<MTLFunction> vertFunc = [library newFunctionWithName:@"pointCloudVertex"];
            id<MTLFunction> fragFunc = [library newFunctionWithName:@"pointCloudFragment"];
            MTLRenderPipelineDescriptor *pipeDesc = [[MTLRenderPipelineDescriptor alloc] init];
            pipeDesc.vertexFunction = vertFunc;
            pipeDesc.fragmentFunction = fragFunc;
            pipeDesc.colorAttachments[0].pixelFormat = mtkView.colorPixelFormat;
            pipeDesc.depthAttachmentPixelFormat = mtkView.depthStencilPixelFormat;
            _pointPipeline = [_device newRenderPipelineStateWithDescriptor:pipeDesc error:&error];
        }

        MTLDepthStencilDescriptor *depthDesc = [[MTLDepthStencilDescriptor alloc] init];
        depthDesc.depthCompareFunction = MTLCompareFunctionLess;
        depthDesc.depthWriteEnabled = YES;
        _depthState = [_device newDepthStencilStateWithDescriptor:depthDesc];

        _bufferCapacity = 250000;
        _vertexBuffer = [_device newBufferWithLength:_bufferCapacity * sizeof(GpuPointVertex)
                                             options:MTLResourceStorageModeShared];
        _vertexCount = 0;
        _minZ = -1.5f;
        _maxZ = 3.0f;
    }
    return self;
}

- (void)resetCamera {
    _cameraDistance = 15.0f;
    _cameraYawRad = -0.78f;
    _cameraPitchRad = 0.65f;
    _cameraTarget = simd_make_float3(0.0f, 0.0f, 0.0f);
}

- (void)panByDeltaX:(float)dx deltaY:(float)dy {
    float scale = _cameraDistance * 0.002f;
    _cameraTarget.x -= dx * scale * std::sin(_cameraYawRad);
    _cameraTarget.y += dx * scale * std::cos(_cameraYawRad);
    _cameraTarget.z += dy * scale;
}

- (void)orbitByDeltaYaw:(float)dyaw deltaPitch:(float)dpitch {
    _cameraYawRad += dyaw;
    _cameraPitchRad = std::clamp(_cameraPitchRad + dpitch, -1.50f, 1.50f);
}

- (void)zoomByScale:(float)scale {
    if (scale > 0.01f) {
        _cameraDistance = std::clamp(_cameraDistance / scale, 1.0f, 150.0f);
    }
}

- (void)updatePointCloud:(const std::vector<avscan::core::LidarPointXYZIRT> &)points
              trajectory:(const std::vector<avscan::core::SlamState> &)trajectory {
    std::lock_guard<std::mutex> lock(_bufferMutex);
    if (!_device) return;

    // Adaptive point density downsampling to maintain 30 fps on A14 Bionic
    size_t maxRenderPoints = _bufferCapacity - std::min<size_t>(trajectory.size() * 2, 10000);
    size_t stride = 1;
    if (points.size() > maxRenderPoints && maxRenderPoints > 0) {
        stride = (points.size() + maxRenderPoints - 1) / maxRenderPoints;
    }

    GpuPointVertex *dst = reinterpret_cast<GpuPointVertex *>([_vertexBuffer contents]);
    size_t count = 0;
    float min_z = 1e6f, max_z = -1e6f;

    for (size_t i = 0; i < points.size() && count < _bufferCapacity; i += stride) {
        const auto &pt = points[i];
        GpuPointVertex v;
        v.x = static_cast<float>(pt.x);
        v.y = static_cast<float>(pt.y);
        v.z = static_cast<float>(pt.z);
        v.r = pt.r;
        v.g = pt.g;
        v.b = pt.b;
        v.a = 255;
        v.intensity = pt.intensity;
        dst[count++] = v;
        min_z = std::min(min_z, v.z);
        max_z = std::max(max_z, v.z);
    }

    // Append bright cyan trajectory markers
    for (const auto &st : trajectory) {
        if (count >= _bufferCapacity) break;
        GpuPointVertex tv;
        tv.x = static_cast<float>(st.T_world_body.t.x);
        tv.y = static_cast<float>(st.T_world_body.t.y);
        tv.z = static_cast<float>(st.T_world_body.t.z);
        tv.r = 0;
        tv.g = 255;
        tv.b = 220;
        tv.a = 255;
        tv.intensity = 255.0f;
        dst[count++] = tv;
    }

    if (!trajectory.empty()) {
        const auto &last = trajectory.back().T_world_body.t;
        _cameraTarget = simd_make_float3(static_cast<float>(last.x),
                                         static_cast<float>(last.y),
                                         static_cast<float>(last.z));
    }

    _vertexCount = count;
    if (min_z < max_z) {
        _minZ = min_z;
        _maxZ = max_z;
    }
}

- (void)mtkView:(MTKView *)view drawableSizeWillChange:(CGSize)size {
    (void)view;
    (void)size;
}

- (void)drawInMTKView:(MTKView *)view {
    if (!_pointPipeline || !_commandQueue) return;

    MTLRenderPassDescriptor *passDesc = view.currentRenderPassDescriptor;
    id<CAMetalDrawable> drawable = view.currentDrawable;
    if (!passDesc || !drawable) return;

    id<MTLCommandBuffer> cmdBuf = [_commandQueue commandBuffer];
    id<MTLRenderCommandEncoder> enc = [cmdBuf renderCommandEncoderWithDescriptor:passDesc];
    [enc setRenderPipelineState:_pointPipeline];
    [enc setDepthStencilState:_depthState];

    CGSize sz = view.drawableSize;
    float aspect = (sz.height > 1.0) ? static_cast<float>(sz.width / sz.height) : 1.333f;

    float cx = _cameraTarget.x + _cameraDistance * std::cos(_cameraPitchRad) * std::cos(_cameraYawRad);
    float cy = _cameraTarget.y + _cameraDistance * std::cos(_cameraPitchRad) * std::sin(_cameraYawRad);
    float cz = _cameraTarget.z + _cameraDistance * std::sin(_cameraPitchRad);

    simd_float4x4 proj = makePerspectiveMatrix(60.0f * (3.14159265f / 180.0f), aspect, 0.1f, 300.0f);
    simd_float4x4 viewMat = makeLookAtMatrix(simd_make_float3(cx, cy, cz),
                                             _cameraTarget,
                                             simd_make_float3(0.0f, 0.0f, 1.0f));

    std::lock_guard<std::mutex> lock(_bufferMutex);
    if (_vertexCount > 0) {
        GpuSceneUniforms uniforms{};
        uniforms.viewProjectionMatrix = simd_mul(proj, viewMat);
        uniforms.pointSize = _pointSize;
        uniforms.colorMode = _colorMode;
        uniforms.minZ = _minZ;
        uniforms.maxZ = _maxZ;

        [enc setVertexBuffer:_vertexBuffer offset:0 atIndex:0];
        [enc setVertexBytes:&uniforms length:sizeof(uniforms) atIndex:1];
        [enc drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:_vertexCount];
    }

    [enc endEncoding];
    [cmdBuf presentDrawable:drawable];
    [cmdBuf commit];
}

@end
