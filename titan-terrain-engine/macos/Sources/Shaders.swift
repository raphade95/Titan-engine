// Metal shaders, compiled at runtime with device.makeLibrary(source:) —
// no build-time .metal toolchain dependency, and the source ships readable.
// The splat logic mirrors the web lab's fragment shader (temperate biome).

let titanShaderSource = """
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float4 color; // R rock, G height, B flow, A sediment
};

struct Uniforms {
    float4x4 mvp;
    float4 sunDirAndIntensity;  // xyz dir, w intensity
    float4 cameraPosAndFog;     // xyz pos, w fog density
    float4 skyColorAndTime;     // xyz sky, w time
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float4 splat;
    float3 worldPos;
};

vertex VertexOut terrain_vertex(uint vid [[vertex_id]],
                                device const VertexIn* vertices [[buffer(0)]],
                                constant Uniforms& u [[buffer(1)]]) {
    VertexOut out;
    float3 p = float3(vertices[vid].position);
    out.position = u.mvp * float4(p, 1.0);
    out.normal = float3(vertices[vid].normal);
    out.splat = float4(vertices[vid].color);
    out.worldPos = p;
    return out;
}

float hash21(float2 p) {
    return fract(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123);
}

float vnoise(float2 p) {
    float2 i = floor(p);
    float2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash21(i), hash21(i + float2(1, 0)), f.x),
               mix(hash21(i + float2(0, 1)), hash21(i + float2(1, 1)), f.x), f.y);
}

fragment float4 terrain_fragment(VertexOut in [[stage_in]],
                                 constant Uniforms& u [[buffer(1)]]) {
    float3 lightDir = normalize(u.sunDirAndIntensity.xyz);
    float3 n = normalize(in.normal);
    float diff = max(dot(n, lightDir), 0.0) * u.sunDirAndIntensity.w;

    float rockMask = in.splat.r;
    float height = in.splat.g;
    float flowMask = in.splat.b;
    float sedimentMask = in.splat.a;

    float3 grassColor = float3(0.12, 0.22, 0.08);
    float3 rockColor = float3(0.25, 0.24, 0.22);
    float3 snowColor = float3(0.95, 0.98, 1.0);
    float3 sedimentColor = float3(0.22, 0.16, 0.10);
    float3 waterColor = float3(0.02, 0.08, 0.18);

    float detail = vnoise(in.worldPos.xz * 2.0) * 0.4 + vnoise(in.worldPos.xz * 8.0) * 0.1;
    grassColor *= (0.8 + detail * 0.4);
    rockColor *= (0.7 + detail * 0.6);
    sedimentColor *= (0.8 + detail * 0.4);

    float3 baseColor = mix(grassColor, rockColor, smoothstep(0.3, 0.8, rockMask));

    float strata = sin(in.worldPos.y * 3.0) + sin(in.worldPos.y * 7.2) * 0.4;
    baseColor = mix(baseColor, float3(0.35, 0.32, 0.28),
                    smoothstep(0.2, 1.0, strata) * rockMask * 0.5);

    baseColor = mix(baseColor, sedimentColor, smoothstep(0.05, 0.5, sedimentMask) * (1.0 - rockMask));

    float wetness = smoothstep(0.1, 0.9, flowMask);
    baseColor = mix(baseColor, waterColor, wetness * 0.5);

    baseColor = mix(baseColor, snowColor, smoothstep(0.78, 0.98, height));

    float3 color = baseColor * (diff * 0.8 + 0.25);

    // Specular for snow/wet surfaces.
    float3 viewDir = normalize(u.cameraPosAndFog.xyz - in.worldPos);
    float3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(n, halfDir), 0.0), 32.0);
    float specMask = max(smoothstep(0.7, 1.0, height), wetness * 0.6);
    color += float3(spec * specMask * 0.3);

    // Distance fog toward the sky color.
    float dist = length(in.worldPos.xz - u.cameraPosAndFog.xz);
    float fog = 1.0 - exp(-dist * u.cameraPosAndFog.w * 2.0);
    color = mix(color, u.skyColorAndTime.xyz, fog);

    return float4(color, 1.0);
}
"""
