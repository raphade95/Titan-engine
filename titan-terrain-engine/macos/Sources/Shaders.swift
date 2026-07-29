// Metal shaders, compiled at runtime with device.makeLibrary(source:) —
// no build-time .metal toolchain dependency, and the source ships readable.
// The terrain splat logic and biome palettes mirror the web lab's fragment
// shader; fog is distance-from-origin like the web (not camera distance).

let titanShaderSource = """
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    packed_float3 position;
    packed_float3 normal;
    packed_float4 color;   // R rock, G height, B flow, A sediment
    packed_float4 lava;    // molten depth, heat, chilled-rock depth, glow
    packed_float4 surface; // ambient occlusion, curvature, snow depth, water depth
};

struct Uniforms {
    float4x4 mvp;
    float4 sunDirAndIntensity;  // xyz dir, w intensity
    float4 cameraPosAndFog;     // xyz pos, w fog density
    float4 skyColorAndTime;     // xyz sky, w time
    float4 misc;                // x biome, y sea level, z height scale, w terrain extent
};

struct VertexOut {
    float4 position [[position]];
    float3 normal;
    float4 splat;
    float3 worldPos;
    float4 lava;
    float4 surface;
};

vertex VertexOut terrain_vertex(uint vid [[vertex_id]],
                                device const VertexIn* vertices [[buffer(0)]],
                                constant Uniforms& u [[buffer(1)]]) {
    VertexOut out;
    float3 p = float3(vertices[vid].position);
    out.position = u.mvp * float4(p, 1.0);
    out.normal = float3(vertices[vid].normal);
    out.splat = float4(vertices[vid].color);
    out.lava = float4(vertices[vid].lava);
    out.surface = float4(vertices[vid].surface);
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

float vfbm(float2 p) {
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += vnoise(p) * a;
        p = p * 2.03 + float2(17.3, 5.1);
        a *= 0.5;
    }
    return v;
}

// --- Colour management -----------------------------------------------------
// The palettes below are sRGB swatches. Lighting them directly — multiplying
// an sRGB colour by a cosine term — is the most common reason a renderer looks
// flat: mid-tones wash out and shadow falloff is wrong everywhere. Decode to
// linear, light in linear, tone map, encode back.
float3 toLinear(float3 c) { return pow(max(c, float3(0.0)), float3(2.2)); }
float3 toSRGB(float3 c)   { return pow(max(c, float3(0.0)), float3(1.0 / 2.2)); }

// ACES filmic approximation (Narkowicz). Rolls highlights off instead of
// clipping them, which is what stops molten lava and lit snow turning to paste.
float3 tonemapACES(float3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// Incandescence ramp. Real lava runs black -> deep red -> orange -> yellow ->
// near-white as it heats, and reading that gradient off a surface is most of
// how an eye judges how molten something is. Identical to the web viewer's,
// and like it returns linear light.
float3 blackbody(float t) {
    t = clamp(t, 0.0, 1.0);
    float3 c = mix(float3(0.32, 0.02, 0.005), float3(0.95, 0.18, 0.02),
                   smoothstep(0.0, 0.35, t));
    c = mix(c, float3(1.0, 0.48, 0.06), smoothstep(0.3, 0.65, t));
    c = mix(c, float3(1.0, 0.82, 0.30), smoothstep(0.6, 0.87, t));
    c = mix(c, float3(1.0, 0.96, 0.80), smoothstep(0.85, 1.0, t));
    return toLinear(c);
}

fragment float4 terrain_fragment(VertexOut in [[stage_in]],
                                 constant Uniforms& u [[buffer(1)]]) {
    float3 lightDir = normalize(u.sunDirAndIntensity.xyz);
    float3 n = normalize(in.normal);
    float diff = max(dot(n, lightDir), 0.0);

    float rockMask = in.splat.r;
    float height = in.splat.g;
    float flowMask = in.splat.b;
    float sedimentMask = in.splat.a;

    float ao        = in.surface.x;
    float curvature = in.surface.y;
    float snowDepth = in.surface.z;
    float lakeDepth = in.surface.w;

    // Biome palettes — identical to the web lab's fragment shader.
    int biome = int(u.misc.x + 0.5);
    float3 grassColor, rockColor, sedimentColor;
    float3 snowColor = float3(0.95, 0.98, 1.0);
    float3 waterColor = float3(0.02, 0.08, 0.18);
    if (biome == 0) {          // arctic
        grassColor = float3(0.4, 0.45, 0.5);
        rockColor = float3(0.4, 0.42, 0.45);
        sedimentColor = float3(0.8, 0.85, 0.9);
        snowColor = float3(1.0);
    } else if (biome == 2) {   // volcanic
        grassColor = float3(0.05, 0.1, 0.02);
        rockColor = float3(0.05, 0.05, 0.06);
        sedimentColor = float3(0.12, 0.08, 0.06);
        // Pale ash and pumice, not snow. This palette overrode every other
        // colour but left the high-altitude band pure white, so any tall
        // volcanic peak came out snow-capped.
        snowColor = float3(0.58, 0.55, 0.53);
    } else if (biome == 3) {   // desert
        grassColor = float3(0.4, 0.35, 0.2);
        rockColor = float3(0.45, 0.3, 0.2);
        sedimentColor = float3(0.7, 0.5, 0.3);
    } else {                   // temperate
        grassColor = float3(0.12, 0.22, 0.08);
        rockColor = float3(0.25, 0.24, 0.22);
        sedimentColor = float3(0.22, 0.16, 0.10);
    }

    // Everything from here on is linear light.
    grassColor = toLinear(grassColor);
    rockColor = toLinear(rockColor);
    snowColor = toLinear(snowColor);
    sedimentColor = toLinear(sedimentColor);
    waterColor = toLinear(waterColor);

    // Two scales of fBm rather than two raw value-noise taps — the old version
    // was a single octave at each scale, which reads as television static up
    // close rather than as ground.
    float detail = vfbm(in.worldPos.xz * 0.35) * 0.65 + vfbm(in.worldPos.xz * 2.2) * 0.35;
    grassColor *= (0.75 + detail * 0.55);
    rockColor *= (0.65 + detail * 0.75);
    sedimentColor *= (0.78 + detail * 0.5);

    float3 baseColor = mix(grassColor, rockColor, smoothstep(0.4, 0.8, rockMask));

    // Strata frequency is relative to the terrain's height scale, matching the
    // engine's HardnessAt convention (20 / heightScale) and the web viewer. A
    // fixed absolute frequency gave ~5 bands at Height 25 but ~33 at Height 70,
    // which reads as contour lines rather than geology.
    float strataFreq = 20.0 / max(1.0, u.misc.z);
    float strata = sin(in.worldPos.y * strataFreq)
                 + sin(in.worldPos.y * strataFreq * 2.4) * 0.4;
    baseColor = mix(baseColor, toLinear(float3(0.35, 0.32, 0.28)),
                    smoothstep(0.2, 1.0, strata) * rockMask * 0.6);

    // Cavity shading from the engine's curvature. Creases hold dirt and shadow;
    // convex edges are scoured and catch light. This is the cue that makes a
    // heightfield read as rock rather than as a tinted bedsheet.
    float crease = smoothstep(0.5, 0.85, curvature);
    float edgeLit = smoothstep(0.5, 0.15, curvature);
    baseColor *= (1.0 - crease * 0.35) * (1.0 + edgeLit * 0.18);

    baseColor = mix(baseColor, sedimentColor, smoothstep(0.01, 0.4, sedimentMask));

    float wetness = smoothstep(0.1, 0.9, flowMask);
    baseColor = mix(baseColor, waterColor, wetness * 0.5);

    // Snow, from the engine's actual snowpack.
    //
    // This used to be a smoothstep over the normalized height channel — a pure
    // shader invention with no connection to the Snow layer, which simulates
    // accumulation, slope shedding, creep settling and melt and then had its
    // entire result ignored. Depth drives coverage: a dusting lets rock show
    // through, a deep drift buries it.
    float snowFall = smoothstep(0.02, 0.9, snowDepth);
    baseColor = mix(baseColor, snowColor, snowFall);

    // ---- Volcanism ---------------------------------------------------------
    float molten   = in.lava.x;
    float lavaHeat = in.lava.y;
    float basalt   = in.lava.z;
    float lavaGlow = in.lava.w;

    // Cooled flows are fresh basalt: near-black, faintly iridescent, rougher
    // than the rock around them. Painted before the molten pass so a flow that
    // has crusted over still reads as a flow.
    float basaltMask = smoothstep(0.02, 0.5, basalt);
    float3 basaltColor = toLinear(float3(0.045, 0.040, 0.045))
                       * (0.55 + vfbm(in.worldPos.xz * 1.6) * 0.9);
    basaltColor += toLinear(float3(0.06, 0.045, 0.035))
                 * smoothstep(0.5, 0.9, vfbm(in.worldPos.xz * 5.0));
    baseColor = mix(baseColor, basaltColor, basaltMask);

    // Snow cannot survive on a live flow.
    float snowKill = 1.0 - clamp(basaltMask * 0.7 + molten * 4.0, 0.0, 1.0);

    float3 viewDir = normalize(u.cameraPosAndFog.xyz - in.worldPos);
    float3 halfDir = normalize(lightDir + viewDir);

    // ---- Lakes ---------------------------------------------------------
    // The Lakes layer priority-floods every basin that cannot drain and gives
    // a per-cell depth. The mesh is displaced to that level, so here the
    // surface really is the pond top and only needs shading. Nothing rendered
    // it before: the layer was computable, exportable, and invisible in 3D.
    float lake = smoothstep(0.02, 0.35, lakeDepth);
    if (lake > 0.001) {
        float3 shallow = toLinear(float3(0.16, 0.35, 0.38));
        float3 deep    = toLinear(float3(0.01, 0.05, 0.11));
        float3 pond = mix(shallow, deep, smoothstep(0.0, 6.0, lakeDepth));
        baseColor = mix(baseColor, pond, lake);
        // A pond surface is flat regardless of the lakebed under it.
        n = normalize(mix(n, float3(0.0, 1.0, 0.0), lake));
        diff = max(dot(n, lightDir), 0.0);
    }

    // ---- Lighting ------------------------------------------------------
    // Sun plus a hemisphere ambient. The old model was one Lambert term over a
    // flat constant, which lights the underside of a cliff exactly as brightly
    // as its top and is why the terrain read as shadowless.
    //
    // Light levels are scene-referred now, not display-referred: the palettes
    // are albedos and get multiplied by incoming light before tone mapping, so
    // sunlight has to be several units strong for a 0.25-albedo rock to land
    // near mid-grey.
    float3 skyLight    = toLinear(u.skyColorAndTime.xyz) * 1.5;
    float3 groundLight = toLinear(float3(0.16, 0.13, 0.10)) * 0.9;
    float hemi = 0.5 + 0.5 * n.y;
    float3 ambient = mix(groundLight, skyLight, hemi);

    // Ambient occlusion, horizon-traced by the engine. It gates the ambient
    // term only — occlusion is about how much sky a point can see, and applying
    // it to direct sunlight would darken lit slopes that are plainly in the sun.
    ambient *= ao;

    float sunHeight = clamp(lightDir.y, 0.0, 1.0);
    float3 sunTint = mix(toLinear(float3(1.0, 0.45, 0.18)),
                         toLinear(float3(1.0, 0.96, 0.9)),
                         smoothstep(0.0, 0.35, sunHeight));
    float3 sun = sunTint * u.sunDirAndIntensity.w * 2.4 * diff;
    // Soft terminator: real ground scatters light a little past 90 degrees.
    sun *= smoothstep(-0.08, 0.15, dot(n, lightDir)) * 0.6 + 0.4;

    float3 color = baseColor * (sun + ambient);

    // Specular for wet/ice/water surfaces.
    float gloss = max(max(snowFall * snowKill * 0.5, wetness * 0.6), lake);
    float shininess = mix(32.0, 200.0, lake);
    float spec = pow(max(dot(n, halfDir), 0.0), shininess);
    float fres = pow(1.0 - max(dot(n, viewDir), 0.0), 5.0);
    color += sunTint * u.sunDirAndIntensity.w * spec * gloss * (0.35 + fres * 1.6);
    color += skyLight * fres * lake * 0.5 * ao;

    // A flow lights the ground it runs past. Without this the lava is a bright
    // ribbon lying on unlit rock, which is the single thing that most makes
    // rendered lava look pasted on. Modulated by the surface albedo, because
    // this is bounce light landing on rock — not a decal.
    color += baseColor * blackbody(0.6) * pow(lavaGlow, 3.0) * 2.0;

    // ---- Molten lava -------------------------------------------------------
    // Thin margins fade out rather than ending on a hard edge: the engine
    // leaves a chilled veneer at the flow front and it should read as the
    // crust it is.
    float lavaMask = smoothstep(0.015, 0.18, molten);
    if (lavaMask > 0.001) {
        // Downhill direction. For a heightfield normal (-dh/dx, 1, -dh/dz) the
        // surface descends along n.xz, so this is the direction the flow is
        // actually travelling — no extra vertex data needed.
        float2 flowDir = n.xz;
        float flowLen = length(flowDir);
        flowDir = flowLen > 0.001 ? flowDir / flowLen : float2(0.0, 1.0);
        float2 across = float2(-flowDir.y, flowDir.x);

        float along = dot(in.worldPos.xz, flowDir);
        float side  = dot(in.worldPos.xz, across);
        float drift = u.skyColorAndTime.w * (0.35 + lavaHeat * 0.9);

        // Warp the frame before sampling. A channel is only a few cells wide,
        // so the across-flow coordinate barely varies over it and an unwarped
        // pattern collapses into evenly spaced rungs — a ladder painted on the
        // lava. Displacing by a low-frequency field bends the plate boundaries
        // into the irregular arcs ropy pahoehoe forms as its crust is dragged
        // downstream.
        float warp = vfbm(in.worldPos.xz * 0.55) - 0.5;
        float2 crustUV = float2((along - drift) * 0.24 + warp * 1.6,
                                side * 0.85 + warp * 0.7);

        float plates = vfbm(crustUV);
        // Cracks are the low ridges between plates; the hotter the flow, the
        // wider they gape and the more incandescence shows through.
        float crack = 1.0 - smoothstep(0.0, 0.18 + lavaHeat * 0.22,
                                       abs(plates - 0.5));
        float fine = vfbm(crustUV * 3.7 + float2(drift * 0.4, 0.0));
        crack = max(crack, (1.0 - smoothstep(0.0, 0.07, abs(fine - 0.5)))
                           * lavaHeat * 0.7);

        // A fast flow tears its crust apart; a stalled one skins over.
        float exposure = clamp(crack * (0.35 + lavaHeat) + lavaHeat * 0.30, 0.0, 1.0);

        // Interior temperature: the channel core runs hotter than its margins,
        // so deep lava glows brighter than the same lava spread thin.
        float coreT = lavaHeat * (0.55 + 0.45 * smoothstep(0.05, 1.2, molten));
        float3 glowColor = blackbody(coreT);

            float3 crustColor = toLinear(float3(0.035, 0.030, 0.032)) * (0.6 + plates * 0.8);

        float3 lavaSurface = mix(crustColor * (sun * 0.35 + ambient), glowColor, exposure);
        // Emission proper — unlit, so it stays bright in shadow and at night,
        // which is the whole point of a self-luminous material.
        // Driven well past 1.0 on purpose. The tone mapper compresses the top
        // end, so an emitter peaking at white-on-the-wire lands as a dull cream
        // once rolled off; overdriving buys back an incandescent core.
        float3 emission = glowColor * exposure * (0.7 + coreT * 2.6);
        emission += blackbody(coreT * 0.55) * lavaHeat * 0.25;

        // Molten rock is glassy: a tight, strong highlight.
        float lavaSpec = pow(max(dot(n, halfDir), 0.0), 90.0);
        lavaSurface += toLinear(float3(1.0, 0.85, 0.6)) * lavaSpec * 0.5;

        color = mix(color, lavaSurface + emission, lavaMask);
    }

    // ---- Atmosphere ----------------------------------------------------
    // Distance from the *camera*, with a height falloff, normalized by the
    // terrain extent.
    //
    // This measured distance from the terrain's origin, so haze formed a fixed
    // bowl centred on the map: it thickened as you looked outward even from a
    // metre away and never changed when you flew backwards. That is the
    // opposite of aerial perspective, which is precisely the cue an eye uses
    // to read scale — and terrain is nothing but scale.
    float extent = max(u.misc.w, 1.0);
    float dist = length(u.cameraPosAndFog.xyz - in.worldPos) / extent;
    float heightFalloff = exp(-max(in.worldPos.y, 0.0) / max(extent * 0.35, 1.0));
    float fog = 1.0 - exp(-dist * u.cameraPosAndFog.w * 120.0 * heightFalloff);

    // Haze scatters the sun: looking toward it, the air glows.
    float sunAmount = max(dot(-viewDir, lightDir), 0.0);
    float3 fogCol = mix(toLinear(u.skyColorAndTime.xyz), sunTint * 1.15,
                        pow(sunAmount, 6.0) * 0.55);
    color = mix(color, fogCol, clamp(fog, 0.0, 1.0));

    return float4(toSRGB(tonemapACES(color)), 1.0);
}

// --- Flat color (grid helper lines) ----------------------------------------

struct FlatParams {
    float4 color;
};

struct FlatOut {
    float4 position [[position]];
};

vertex FlatOut flat_vertex(uint vid [[vertex_id]],
                           device const packed_float3* verts [[buffer(0)]],
                           constant Uniforms& u [[buffer(1)]]) {
    FlatOut out;
    out.position = u.mvp * float4(float3(verts[vid]), 1.0);
    return out;
}

fragment float4 flat_fragment(FlatOut in [[stage_in]],
                              constant FlatParams& p [[buffer(0)]]) {
    return p.color;
}

// --- Sea-level water plane (translucent, animated) -------------------------

struct WaterOut {
    float4 position [[position]];
    float3 worldPos;
};

vertex WaterOut water_vertex(uint vid [[vertex_id]],
                             device const packed_float3* verts [[buffer(0)]],
                             constant Uniforms& u [[buffer(1)]]) {
    WaterOut out;
    float3 p = float3(verts[vid]);
    float t = u.skyColorAndTime.w;
    p.y = u.misc.y + sin(p.x * 0.05 + t) * 0.35 + cos(p.z * 0.07 + t * 1.2) * 0.25
        + sin((p.x + p.z) * 0.021 - t * 0.6) * 0.5;
    out.position = u.mvp * float4(p, 1.0);
    out.worldPos = p;
    return out;
}

fragment float4 water_fragment(WaterOut in [[stage_in]],
                               constant Uniforms& u [[buffer(1)]]) {
    float3 viewDir = normalize(u.cameraPosAndFog.xyz - in.worldPos);
    float3 lightDir = normalize(u.sunDirAndIntensity.xyz);
    float t = u.skyColorAndTime.w;

    // Ripple normal, so the sea is not a mirror-flat pane. Two drifting
    // octaves give a moving surface without needing a normal map.
    float e = 0.6;
    float2 q = in.worldPos.xz * 0.06;
    float n0 = vnoise(q + float2(t * 0.05, 0.0)) + vnoise(q * 2.7 - float2(0.0, t * 0.08)) * 0.5;
    float nx = vnoise(q + float2(e, 0.0) + float2(t * 0.05, 0.0))
             + vnoise((q + float2(e, 0.0)) * 2.7 - float2(0.0, t * 0.08)) * 0.5;
    float nz = vnoise(q + float2(0.0, e) + float2(t * 0.05, 0.0))
             + vnoise((q + float2(0.0, e)) * 2.7 - float2(0.0, t * 0.08)) * 0.5;
    float3 nrm = normalize(float3(-(nx - n0) * 0.35, 1.0, -(nz - n0) * 0.35));

    float fresnel = pow(1.0 - max(dot(viewDir, nrm), 0.0), 4.0);
    float3 deep = toLinear(float3(0.0, 0.31, 0.49));
    float3 sky  = toLinear(float3(0.42, 0.62, 0.82));
    float3 color = mix(deep, sky, clamp(fresnel * 1.2, 0.0, 1.0));

    float3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(nrm, halfDir), 0.0), 220.0);
    color += toLinear(float3(1.0, 0.95, 0.85)) * spec * 3.0;

    return float4(toSRGB(tonemapACES(color)), 0.86);
}

// --- Instanced flora --------------------------------------------------------

struct FloraVertexIn {
    packed_float3 position;
    packed_float3 normal;
};

struct FloraInstanceIn {
    float4 posRot;  // xyz position, w rotation around Y
    float4 scale;   // xyz scale
};

struct FloraOut {
    float4 position [[position]];
    float3 normal;
    float3 worldPos;
};

vertex FloraOut flora_vertex(uint vid [[vertex_id]],
                             uint iid [[instance_id]],
                             device const FloraVertexIn* verts [[buffer(0)]],
                             constant Uniforms& u [[buffer(1)]],
                             device const FloraInstanceIn* instances [[buffer(2)]]) {
    FloraInstanceIn inst = instances[iid];
    float c = cos(inst.posRot.w);
    float s = sin(inst.posRot.w);

    float3 v = float3(verts[vid].position) * inst.scale.xyz;
    float3 rotated = float3(v.x * c - v.z * s, v.y, v.x * s + v.z * c);
    float3 world = rotated + inst.posRot.xyz;

    float3 n = float3(verts[vid].normal);
    float3 rn = float3(n.x * c - n.z * s, n.y, n.x * s + n.z * c);

    FloraOut out;
    out.position = u.mvp * float4(world, 1.0);
    out.normal = rn;
    out.worldPos = world;
    return out;
}

fragment float4 flora_fragment(FloraOut in [[stage_in]],
                               constant Uniforms& u [[buffer(1)]],
                               constant FlatParams& p [[buffer(0)]]) {
    float3 lightDir = normalize(u.sunDirAndIntensity.xyz);
    float diff = max(dot(normalize(in.normal), lightDir), 0.0) * u.sunDirAndIntensity.w;
    float3 color = p.color.rgb * (diff * 0.7 + 0.3);

    float dist = length(in.worldPos.xz);
    float fog = 1.0 - exp(-dist * u.cameraPosAndFog.w * 2.0);
    color = mix(color, u.skyColorAndTime.xyz, fog);
    return float4(color, 1.0);
}
"""
