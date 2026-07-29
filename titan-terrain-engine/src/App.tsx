import React, { useEffect, useRef, useState, useMemo } from 'react';
import * as THREE from 'three';
import { OrbitControls } from 'three-stdlib';
import { motion, AnimatePresence } from 'motion/react';
import { 
  Settings2, 
  Mountain, 
  Droplets,
  RotateCcw,
  Download, 
  Layers,
  Activity,
  ChevronRight,
  ChevronLeft,
  Maximize2,
  Cpu,
  FileCode,
  Image as ImageIcon,
  Trees,
  RefreshCw,
  Cloud,
  Waves,
  Minus,
  Lock,
  Unlock,
  Undo2,
  Redo2,
  Plus,
  Trash2,
  ArrowUp,
  ArrowDown,
  Save,
  FolderOpen,
  X,
  Hexagon,
  Grid3X3,
  Grid2X2,
  Map as MapIcon,
  Upload,
  Flame
} from 'lucide-react';

import { TitanCore } from './core/TitanCore';
import { TerrainParams, ExportKind } from './core/types';
import {
  ImportedField,
  Layer,
  LAYER_DEFS,
  LayerMask,
  LayerType,
  MASK_MODE_LABELS,
  PRESETS,
  TitanProject,
  deserializeProject,
  instantiatePreset,
  makeLayer,
  runPipeline,
  serializeProject,
} from './core/pipeline';

import { Button } from '@/components/ui/button';
import { Slider } from '@/components/ui/slider';
import { Card } from '@/components/ui/card';
import { Tabs, TabsContent, TabsList, TabsTrigger } from '@/components/ui/tabs';
import { Switch } from '@/components/ui/switch';
import { Label } from '@/components/ui/label';
import { Separator } from '@/components/ui/separator';

const randomSeed = () => Math.random().toString(36).substring(2, 9);

// Preview mesh cap. The engine simulates and exports at full resolution; the
// mesh copied into JS for Three.js is decimated to this many vertices per
// edge. A 2048 terrain at full mesh resolution is 4.2M vertices — roughly
// 300 MB of typed arrays once positions, normals, colours, UVs and indices are
// copied out of the WASM heap, and 4096 would not fit a 32-bit heap at all.
// Decoupling the two is what lets the resolution slider go past 512.
const PREVIEW_MAX_EDGE_VERTS = 512;

// The app opens as a clean, flat canvas: no terrain until the user picks a
// noise structure or tweaks a slider. Every session starts with a fresh seed.
const DEFAULT_PARAMS: TerrainParams = {
  size: 128,
  scale: 2.0,
  heightMultiplier: 40,
  seed: randomSeed(),
  octaves: 6,
  persistence: 0.5,
  lacunarity: 2.0,
  exponent: 1.2,
  warpStrength: 0.5,
  noiseType: 'none',
  biome: 'temperate',
};

export default function App() {
  const containerRef = useRef<HTMLDivElement>(null);
  const canvasRef = useRef<HTMLCanvasElement>(null);
  const [params, setParams] = useState<TerrainParams>(DEFAULT_PARAMS);
  // The real post-stack height range. Normalizing exports (.png16/.r16) stretch
  // to exactly this, so a user needs it to set a correct Z scale on import — it
  // is NOT [0, Height slider] once erosion and deposition have run, which is
  // what the export docs used to tell people to assume.
  const [heightRange, setHeightRange] = useState<{ min: number; max: number } | null>(null);
  // Which layers have their advanced physics panel open, keyed by layer id.
  const [expandedAdvanced, setExpandedAdvanced] = useState<Record<string, boolean>>({});
  const [isSidebarOpen, setIsSidebarOpen] = useState(true);
  const [isGenerating, setIsGenerating] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const [stats, setStats] = useState({ vertices: 0, triangles: 0, time: 0, flora: 0 });
  const [stack, setStack] = useState<Layer[]>([]);
  const [progress, setProgress] = useState<{ frac: number; label: string } | null>(null);
  const [seedLocked, setSeedLocked] = useState(false);
  const [engineStatus, setEngineStatus] = useState<'loading' | 'ready' | 'error'>('loading');
  const [engineVersion, setEngineVersion] = useState('');
  const [historyState, setHistoryState] = useState({ canUndo: false, canRedo: false });
  const runIdRef = useRef(0);
  const historyRef = useRef<{ past: string[]; future: string[] }>({ past: [], future: [] });
  const applyingHistoryRef = useRef(false);
  const loadFileRef = useRef<HTMLInputElement | null>(null);
  const [floraDensity, setFloraDensity] = useState(2000);
  const [isInspectMode, setIsInspectMode] = useState(false);
  const [probeData, setProbeData] = useState<{ x: number, y: number, h: number, s: number, f: number, slope: number } | null>(null);
  const [isCarveMode, setIsCarveMode] = useState(false);
  const [carveRadius, setCarveRadius] = useState(4);
  const [carveDepth, setCarveDepth] = useState(2);
  // Drop-a-volcano mode: press on the terrain to place one, drag to position
  // it. The layer that is being dragged is held here so pointer-move can keep
  // rewriting its coordinates instead of adding a cone per frame.
  const [isVolcanoMode, setIsVolcanoMode] = useState(false);
  const draggingVolcanoRef = useRef<string | null>(null);
  const [waterLevel, setWaterLevel] = useState(-10);
  const [imported, setImported] = useState<ImportedField | undefined>(undefined);
  const [showMinimap, setShowMinimap] = useState(false);
  const minimapRef = useRef<HTMLCanvasElement | null>(null);
  const importFileRef = useRef<HTMLInputElement | null>(null);
  const [sunIntensity, setSunIntensity] = useState(1.4);
  const [sunElevation, setSunElevation] = useState(45);
  const [sunAzimuth, setSunAzimuth] = useState(45);
  const [fogDensity, setFogDensity] = useState(0.002);
  const isMouseDown = useRef(false);

  // Three.js refs
  const sceneRef = useRef<THREE.Scene | null>(null);
  const cameraRef = useRef<THREE.PerspectiveCamera | null>(null);
  const rendererRef = useRef<THREE.WebGLRenderer | null>(null);
  const controlsRef = useRef<OrbitControls | null>(null);
  const meshRef = useRef<THREE.Mesh | null>(null);
  // Kept across mesh rebuilds so the terrain shader is compiled once, not
  // once per pipeline chunk.
  const materialRef = useRef<THREE.ShaderMaterial | null>(null);
  const engineRef = useRef<TitanCore | null>(null);
  const waterRef = useRef<THREE.Mesh | null>(null);
  const sunLightRef = useRef<THREE.DirectionalLight | null>(null);
  const gridRef = useRef<THREE.GridHelper | null>(null);

  const updateAtmosphere = React.useCallback(() => {
    if (!sceneRef.current || !sunLightRef.current) return;

    // Update Sun Position
    const phi = (90 - sunElevation) * (Math.PI / 180);
    const theta = (sunAzimuth) * (Math.PI / 180);
    const sunPos = new THREE.Vector3().setFromSphericalCoords(300, phi, theta);
    sunLightRef.current.position.copy(sunPos);
    sunLightRef.current.intensity = sunIntensity;

    // Update Fog and Background
    const skyColor = new THREE.Color(0x87CEEB).multiplyScalar(Math.max(0.1, sunElevation / 90));
    sceneRef.current.background = skyColor;
    if (sceneRef.current.fog instanceof THREE.FogExp2) {
      sceneRef.current.fog.density = fogDensity;
      sceneRef.current.fog.color = skyColor;
    }

    // Update terrain uniforms
    if (meshRef.current) {
        const mat = meshRef.current.material as THREE.ShaderMaterial;
        mat.uniforms.uSunPos.value.copy(sunPos);
        mat.uniforms.uFogColor.value.copy(skyColor);
        mat.uniforms.uFogDensity.value = fogDensity;
        mat.uniforms.uSunIntensity.value = sunIntensity;
        mat.uniforms.uSkyColor.value.copy(skyColor);
    }
  }, [sunElevation, sunAzimuth, sunIntensity, fogDensity]);

  // 2D top-down map: hypsometric tint + hillshade + sea-level water, drawn
  // straight from the engine's height data (viewer-side presentation only).
  const updateMinimap = React.useCallback(() => {
    const canvas = minimapRef.current;
    const engine = engineRef.current;
    if (!canvas || !engine) return;
    const size = params.size;
    const bedrock = engine.getBedrockMap();
    const sediment = engine.getSedimentMap();
    if (bedrock.length !== size * size) return;

    canvas.width = size;
    canvas.height = size;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    const img = ctx.createImageData(size, size);

    let minH = Infinity, maxH = -Infinity;
    const h = new Float32Array(size * size);
    for (let i = 0; i < h.length; i++) {
      h[i] = bedrock[i] + sediment[i];
      if (h[i] < minH) minH = h[i];
      if (h[i] > maxH) maxH = h[i];
    }
    const range = maxH - minH || 1;

    // Hypsometric ramp stops (lowlands green -> tan -> rock -> snow).
    const stops = [
      [46, 102, 60], [110, 139, 61], [190, 171, 110],
      [139, 105, 74], [120, 115, 110], [245, 248, 250],
    ];
    const ramp = (t: number) => {
      const f = Math.min(0.9999, Math.max(0, t)) * (stops.length - 1);
      const i = Math.floor(f);
      const u = f - i;
      return [
        stops[i][0] + (stops[i + 1][0] - stops[i][0]) * u,
        stops[i][1] + (stops[i + 1][1] - stops[i][1]) * u,
        stops[i][2] + (stops[i + 1][2] - stops[i][2]) * u,
      ];
    };

    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        const i = y * size + x;
        const xm = x > 0 ? h[i - 1] : h[i];
        const xp = x < size - 1 ? h[i + 1] : h[i];
        const ym = y > 0 ? h[i - size] : h[i];
        const yp = y < size - 1 ? h[i + size] : h[i];
        // NW-lit hillshade.
        const shade = 0.7 + Math.max(-0.6, Math.min(0.6, ((xm - xp) + (ym - yp)) * 0.12));

        let r: number, g: number, b: number;
        if (h[i] < waterLevel) {
          const depth = Math.min(1, (waterLevel - h[i]) / 20);
          r = 30 - 15 * depth; g = 90 - 45 * depth; b = 160 - 60 * depth;
        } else {
          const c = ramp((h[i] - minH) / range);
          r = c[0] * shade; g = c[1] * shade; b = c[2] * shade;
        }
        img.data[i * 4 + 0] = Math.max(0, Math.min(255, r));
        img.data[i * 4 + 1] = Math.max(0, Math.min(255, g));
        img.data[i * 4 + 2] = Math.max(0, Math.min(255, b));
        img.data[i * 4 + 3] = 255;
      }
    }
    ctx.putImageData(img, 0, 0);
  }, [params.size, waterLevel]);

  // Terrain spans size * cellSize world units, so a 2048 grid is 16x the
  // extent of a 128 one. Without reframing, raising the resolution drops the
  // camera inside the terrain.
  const frameTerrain = React.useCallback((extent: number) => {
    const camera = cameraRef.current;
    const controls = controlsRef.current;
    if (!camera || !controls) return;
    const d = extent * 0.8;
    camera.position.set(d, d * 0.75, d);
    controls.target.set(0, 0, 0);
    controls.maxDistance = extent * 4;
    controls.update();
  }, []);

  const refreshHeightRange = React.useCallback(() => {
    if (!engineRef.current) return;
    try {
      setHeightRange(engineRef.current.heightRange());
    } catch {
      setHeightRange(null);
    }
  }, []);

  const updateMesh = React.useCallback(() => {
    if (!sceneRef.current || !engineRef.current) return;

    const meshData = engineRef.current.buildMesh(PREVIEW_MAX_EDGE_VERTS);

    // Reuse the material across rebuilds.
    //
    // The pipeline calls this once per chunk so the viewport animates while a
    // pass runs, and building a fresh ShaderMaterial each time forces WebGL to
    // recompile and relink the program — hundreds of milliseconds that dwarf
    // the work being visualised. A 900-step lava eruption reports 91 ms of
    // engine time and used to take twelve seconds of wall clock, essentially
    // all of it spent recompiling the same shader fifteen times.
    // A cached material outlives a hot reload, so an edit to the shader source
    // or its uniform set would otherwise never reach the GPU — the old program
    // keeps rendering and writes to uniforms that no longer exist throw. Probe
    // for a uniform the current source declares and rebuild if it is missing.
    const cached = materialRef.current;
    const existing = cached && cached.uniforms.uTerrainExtent ? cached : null;
    if (cached && !existing) {
      cached.dispose();
      materialRef.current = null;
    }

    if (existing) {
      existing.uniforms.uBiome.value =
        ['arctic', 'temperate', 'volcanic', 'desert'].indexOf(params.biome);
      existing.uniforms.uHeightScale.value = Math.max(1, params.heightMultiplier);
      existing.uniforms.uTerrainExtent.value = params.size;
    }

    if (meshRef.current) {
      sceneRef.current.remove(meshRef.current);
      meshRef.current.geometry.dispose();
      if (!existing) (meshRef.current.material as THREE.Material).dispose();
    }

    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(meshData.vertices, 3));
    geometry.setAttribute('normal', new THREE.BufferAttribute(meshData.normals, 3));
    geometry.setAttribute('color', new THREE.BufferAttribute(meshData.colors, 4));
    geometry.setAttribute('uv', new THREE.BufferAttribute(meshData.uvs, 2));
    // molten depth / heat / chilled-rock depth / glow — zeroed when nothing
    // has erupted, so the attribute is always bound and the shader is one
    // program rather than two.
    geometry.setAttribute('lava', new THREE.BufferAttribute(meshData.lava, 4));
    // ao / curvature / snow depth / water depth — all simulated by the engine.
    geometry.setAttribute('surface', new THREE.BufferAttribute(meshData.surface, 4));
    geometry.setIndex(new THREE.BufferAttribute(meshData.indices, 1));

    const phi = (90 - sunElevation) * (Math.PI / 180);
    const theta = (sunAzimuth) * (Math.PI / 180);
    const sunPos = new THREE.Vector3().setFromSphericalCoords(300, phi, theta);

    // Custom Shader Material for Splat Mapping
    const material = existing ?? new THREE.ShaderMaterial({
      vertexColors: true,
      side: THREE.DoubleSide,
      uniforms: {
        uTime: { value: 0 },
        uSunPos: { value: sunPos },
        uFogColor: { value: new THREE.Color(0x87CEEB) },
        uFogDensity: { value: fogDensity },
        uBiome: { value: ['arctic', 'temperate', 'volcanic', 'desert'].indexOf(params.biome) },
        // Height scale, so strata banding keeps a constant band count instead
        // of multiplying with the Height slider.
        uHeightScale: { value: Math.max(1, params.heightMultiplier) },
        uSunColor: { value: new THREE.Color(1.0, 0.96, 0.9) },
        uSkyColor: { value: new THREE.Color(0x87CEEB) },
        uSunIntensity: { value: sunIntensity },
        uExposure: { value: 1.0 },
        uTerrainExtent: { value: params.size },
      },
      vertexShader: `
        attribute vec4 lava;
        attribute vec4 surface;

        varying vec3 vPosition;
        varying vec3 vNormal;
        varying vec4 vColor;
        varying vec3 vWorldPos;
        varying vec4 vLava;
        varying vec4 vSurface;

        void main() {
          vPosition = position;
          vNormal = normal;
          vColor = color;
          vLava = lava;
          vSurface = surface;
          vec4 worldPos = modelMatrix * vec4(position, 1.0);
          vWorldPos = worldPos.xyz;
          gl_Position = projectionMatrix * modelViewMatrix * vec4(position, 1.0);
        }
      `,
      fragmentShader: `
        varying vec3 vPosition;
        varying vec3 vNormal;
        varying vec4 vColor;
        varying vec3 vWorldPos;
        varying vec4 vLava;
        varying vec4 vSurface;

        uniform vec3 uSunPos;
        uniform vec3 uFogColor;
        uniform float uFogDensity;
        uniform int uBiome;
        uniform float uHeightScale;
        uniform float uTime;
        uniform vec3 uSunColor;
        uniform vec3 uSkyColor;
        uniform float uSunIntensity;
        uniform float uExposure;
        uniform float uTerrainExtent;

        // --- Colour management ------------------------------------------
        // The palettes below were eyeballed on screen, so they are sRGB
        // values. Lighting them directly — multiplying an sRGB colour by a
        // cosine term — is the single most common reason a renderer looks
        // "flat and plasticky": mid-tones wash out and shadow falloff is
        // wrong everywhere. Decode to linear, light in linear, tone map, then
        // encode back. Three.js does not touch a raw ShaderMaterial's output,
        // so this shader owns the whole chain.
        vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
        vec3 toSRGB(vec3 c)   { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }

        // ACES filmic approximation (Narkowicz). Rolls highlights off instead
        // of clipping them, which is what stops molten lava and lit snow from
        // turning into flat white paste.
        vec3 tonemapACES(vec3 x) {
          const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
          return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
        }

        // Simple hash function for detail noise
        float hash(vec2 p) {
          return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453123);
        }

        float noise(vec2 p) {
          vec2 i = floor(p);
          vec2 f = fract(p);
          f = f * f * (3.0 - 2.0 * f);
          return mix(mix(hash(i + vec2(0.0, 0.0)), hash(i + vec2(1.0, 0.0)), f.x),
                     mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), f.x), f.y);
        }

        float fbm(vec2 p) {
          float v = 0.0;
          float a = 0.5;
          for (int i = 0; i < 5; i++) {
            v += noise(p) * a;
            p = p * 2.03 + vec2(17.3, 5.1);
            a *= 0.5;
          }
          return v;
        }

        // Incandescence ramp. Real lava runs black -> deep red -> orange ->
        // yellow -> near-white as it heats, and reading that gradient off a
        // surface is most of how an eye judges how molten something is.
        // Returns linear light: the stops are authored as sRGB swatches, so
        // they get decoded like every other palette in this shader.
        vec3 blackbody(float t) {
          t = clamp(t, 0.0, 1.0);
          vec3 c = mix(vec3(0.32, 0.02, 0.005), vec3(0.95, 0.18, 0.02),
                       smoothstep(0.0, 0.35, t));
          c = mix(c, vec3(1.0, 0.48, 0.06), smoothstep(0.3, 0.65, t));
          c = mix(c, vec3(1.0, 0.82, 0.30), smoothstep(0.6, 0.87, t));
          c = mix(c, vec3(1.0, 0.96, 0.80), smoothstep(0.85, 1.0, t));
          return toLinear(c);
        }

        void main() {
          vec3 n = normalize(vNormal);
          vec3 lightDir = normalize(uSunPos);
          vec3 viewDir = normalize(cameraPosition - vWorldPos);
          vec3 halfDir = normalize(lightDir + viewDir);

          float ao        = vSurface.x;
          float curvature = vSurface.y;
          float snowDepth = vSurface.z;
          float lakeDepth = vSurface.w;

          float diff = max(dot(n, lightDir), 0.0);

          // Splat mapping logic
          vec3 grassColor;
          vec3 rockColor;
          vec3 snowColor = vec3(0.95, 0.98, 1.0);
          vec3 sedimentColor;
          vec3 waterColor = vec3(0.02, 0.08, 0.18);

          if (uBiome == 0) { // Arctic
            grassColor = vec3(0.4, 0.45, 0.5);
            rockColor = vec3(0.4, 0.42, 0.45);
            sedimentColor = vec3(0.8, 0.85, 0.9);
            snowColor = vec3(1.0);
          } else if (uBiome == 2) { // Volcanic
            grassColor = vec3(0.05, 0.1, 0.02);
            rockColor = vec3(0.05, 0.05, 0.06);
            sedimentColor = vec3(0.12, 0.08, 0.06);
            // Pale ash and pumice, not snow. This palette overrode every other
            // colour but left the high-altitude band pure white, so any tall
            // volcanic peak came out snow-capped.
            snowColor = vec3(0.58, 0.55, 0.53);
          } else if (uBiome == 3) { // Desert
            grassColor = vec3(0.4, 0.35, 0.2);
            rockColor = vec3(0.45, 0.3, 0.2);
            sedimentColor = vec3(0.7, 0.5, 0.3);
          } else { // Temperate
            grassColor = vec3(0.12, 0.22, 0.08);
            rockColor = vec3(0.25, 0.24, 0.22);
            sedimentColor = vec3(0.22, 0.16, 0.1); 
          }
          
          float rockMask = vColor.r;
          float height = vColor.g;
          float flowMask = vColor.b;
          float sedimentMask = vColor.a;
          
          // Everything from here on is linear light.
          grassColor = toLinear(grassColor);
          rockColor = toLinear(rockColor);
          snowColor = toLinear(snowColor);
          sedimentColor = toLinear(sedimentColor);
          waterColor = toLinear(waterColor);

          // Detail texturing. Two scales of fBm rather than two raw value-noise
          // taps — the old version was a single octave at each scale, which
          // reads as television static up close rather than as ground.
          float detail = fbm(vWorldPos.xz * 0.35) * 0.65 + fbm(vWorldPos.xz * 2.2) * 0.35;
          grassColor *= (0.75 + detail * 0.55);
          rockColor *= (0.65 + detail * 0.75);
          sedimentColor *= (0.78 + detail * 0.5);

          // Base mix: Grass and Rock
          vec3 baseColor = mix(grassColor, rockColor, smoothstep(0.4, 0.8, rockMask));
          
          // Visual Strata logic (Horizontal banding on rock).
          //
          // Frequency is relative to the terrain's own height scale, matching
          // the engine's HardnessAt convention (20 / heightScale). A fixed
          // absolute frequency produced ~5 bands at Height 25 but ~33 at
          // Height 70, which reads as contour lines rather than geology — the
          // same scale-dependence bug the engine's strata constant had.
          float strataFreq = 20.0 / uHeightScale;
          float strataDetail = sin(vWorldPos.y * strataFreq)
                             + sin(vWorldPos.y * strataFreq * 2.4) * 0.4;
          vec3 strataColor = toLinear(vec3(0.35, 0.32, 0.28));
          baseColor = mix(baseColor, strataColor, smoothstep(0.2, 1.0, strataDetail) * rockMask * 0.6);

          // Cavity shading from the engine's curvature. Creases hold dirt and
          // shadow; convex edges are scoured and catch light. This is the cue
          // that makes a heightfield read as rock rather than as a tinted
          // bedsheet, and it costs one lerp.
          float crease = smoothstep(0.5, 0.85, curvature);
          float edge   = smoothstep(0.5, 0.15, curvature);
          baseColor *= (1.0 - crease * 0.35) * (1.0 + edge * 0.18);

          // Apply sediment/soil layer
          baseColor = mix(baseColor, sedimentColor, smoothstep(0.01, 0.4, sedimentMask));

          // Apply wetness/flow mapping
          float wetness = smoothstep(0.1, 0.9, flowMask);
          baseColor = mix(baseColor, waterColor, wetness * 0.5);

          // Snow, from the engine's actual snowpack.
          //
          // This used to be smoothstep over the normalized height channel — a
          // pure shader invention with no connection to the Snow layer, which
          // simulates accumulation, slope shedding, creep settling and melt and
          // then had its entire result ignored. Anything above the altitude
          // threshold went white whether or not a single flake had settled
          // there, and every drift the simulation actually produced was
          // invisible. Depth drives coverage: a dusting lets rock show through,
          // a deep drift buries it.
          float snowFall = smoothstep(0.02, 0.9, snowDepth);
          baseColor = mix(baseColor, snowColor, snowFall);

          // ---- Volcanism -------------------------------------------------
          float molten    = vLava.x;
          float lavaHeat  = vLava.y;
          float basalt    = vLava.z;
          float lavaGlow  = vLava.w;

          // Cooled flows are fresh basalt: near-black, faintly iridescent,
          // and rougher than the rock around them. Painted before the molten
          // pass so a flow that has crusted over still reads as a flow.
          float basaltMask = smoothstep(0.02, 0.5, basalt);
          vec3 basaltColor = vec3(0.045, 0.040, 0.045)
                           * (0.55 + fbm(vWorldPos.xz * 1.6) * 0.9);
          // Ropy pahoehoe texture at grazing angles.
          basaltColor += vec3(0.06, 0.045, 0.035)
                       * smoothstep(0.5, 0.9, fbm(vWorldPos.xz * 5.0));
          baseColor = mix(baseColor, basaltColor, basaltMask);

          // Snow cannot survive on a live flow.
          float snowKill = 1.0 - clamp(basaltMask * 0.7 + molten * 4.0, 0.0, 1.0);

          // ---- Lakes -----------------------------------------------------
          // The Lakes layer priority-floods every basin that cannot drain and
          // produces a per-cell depth. The mesh is displaced to that level, so
          // here the surface really is the pond top and only needs shading.
          // Nothing rendered it before: the layer was computable, exportable,
          // and completely invisible in 3D.
          float lake = smoothstep(0.02, 0.35, lakeDepth);
          if (lake > 0.001) {
            vec3 shallow = toLinear(vec3(0.16, 0.35, 0.38));
            vec3 deep    = toLinear(vec3(0.01, 0.05, 0.11));
            vec3 pond = mix(shallow, deep, smoothstep(0.0, 6.0, lakeDepth));
            baseColor = mix(baseColor, pond, lake);
            // A pond surface is flat and mirror-like regardless of the lakebed
            // shape underneath it.
            n = normalize(mix(n, vec3(0.0, 1.0, 0.0), lake));
            diff = max(dot(n, lightDir), 0.0);
          }

          // ---- Lighting ----------------------------------------------------
          // Sun plus a hemisphere ambient. The old model was one Lambert term
          // over a flat grey constant, which lights the underside of a cliff
          // exactly as brightly as its top and is why the terrain read as
          // shadowless. Sky light comes from above and bounce comes from the
          // ground, so an upward-facing surface should be lit blue-ish and a
          // downward-facing one warm and dim.
          // Light levels are scene-referred now, not display-referred. Under
          // the old model a colour was written more or less straight to the
          // framebuffer, so "intensity 1.4" meant roughly full brightness.
          // Here the palettes are albedos and get multiplied by incoming light
          // before tone mapping, so sunlight has to be several units strong for
          // a 0.25-albedo rock to land near mid-grey. Left at the old scale the
          // whole island rendered as a black silhouette.
          vec3 skyLight    = toLinear(uSkyColor) * 1.5;
          vec3 groundLight = toLinear(vec3(0.16, 0.13, 0.10)) * 0.9;
          float hemi = 0.5 + 0.5 * n.y;
          vec3 ambient = mix(groundLight, skyLight, hemi);

          // Ambient occlusion, horizon-traced by the engine. It gates the
          // ambient term only — occlusion is about how much sky a point can
          // see, and applying it to direct sunlight would darken lit slopes
          // that are plainly in the sun.
          ambient *= ao;

          // Sun colour warms and dims as it approaches the horizon.
          float sunHeight = clamp(lightDir.y, 0.0, 1.0);
          vec3 sunTint = mix(toLinear(vec3(1.0, 0.45, 0.18)),
                             toLinear(uSunColor), smoothstep(0.0, 0.35, sunHeight));
          vec3 sun = sunTint * uSunIntensity * 2.4 * diff;
          // Soft terminator: real ground scatters light a little past 90°.
          sun *= smoothstep(-0.08, 0.15, dot(n, lightDir)) * 0.6 + 0.4;

          vec3 finalColor = baseColor * (sun + ambient);

          // Specular for wet/ice/water surfaces.
          float gloss = max(max(snowFall * snowKill * 0.5, wetness * 0.6), lake);
          float shininess = mix(32.0, 200.0, lake);
          float spec = pow(max(dot(n, halfDir), 0.0), shininess);
          // Fresnel — water goes mirror-bright at grazing angles.
          float fres = pow(1.0 - max(dot(n, viewDir), 0.0), 5.0);
          finalColor += sunTint * uSunIntensity * spec * gloss * (0.35 + fres * 1.6);
          finalColor += skyLight * fres * lake * 0.5 * ao;

          // A flow lights the ground it runs past. Without this the lava is a
          // bright ribbon lying on unlit rock, which is the single thing that
          // most makes rendered lava look pasted on. Modulated by the surface
          // albedo, because this is bounce light landing on rock — not a decal.
          // Cubed rather than squared, and modest: this is bounce light, so it
          // should pick out the rock within a stone's throw of the channel and
          // fall away fast. Squared-and-strong smeared a bright wash over half
          // the cone and swallowed all the relief AO had just brought out.
          finalColor += baseColor * blackbody(0.6) * pow(lavaGlow, 3.0) * 2.0;

          // ---- Molten lava -----------------------------------------------
          // Thin margins fade out rather than ending on a hard edge: the
          // engine leaves a chilled veneer at the flow front and it should
          // read as the crust it is.
          float lavaMask = smoothstep(0.015, 0.18, molten);
          if (lavaMask > 0.001) {
            // Downhill direction. For a heightfield normal (-dh/dx, 1, -dh/dz)
            // the surface descends along n.xz, so this is the direction the
            // flow is actually travelling — no extra data needed.
            vec2 flowDir = vNormal.xz;
            float flowLen = length(flowDir);
            flowDir = flowLen > 0.001 ? flowDir / flowLen : vec2(0.0, 1.0);
            vec2 across = vec2(-flowDir.y, flowDir.x);

            // Crust pattern in a frame aligned to the flow: stretched along it,
            // compressed across it. That anisotropy is what makes the surface
            // read as *moving* rather than as a static noise field, and it is
            // why the crust plates look torn downstream.
            float along  = dot(vWorldPos.xz, flowDir);
            float side   = dot(vWorldPos.xz, across);
            float drift  = uTime * (0.35 + lavaHeat * 0.9);

            // Warp the frame before sampling. A channel is only a few cells
            // wide, so the across-flow coordinate barely varies over it and an
            // unwarped pattern collapses into evenly spaced rungs — a ladder
            // painted on the lava. Displacing the coordinates by a
            // low-frequency field bends the plate boundaries into the
            // irregular arcs ropy pahoehoe forms as its crust is dragged
            // downstream.
            float warp = fbm(vWorldPos.xz * 0.55) - 0.5;
            vec2 crustUV = vec2((along - drift) * 0.24 + warp * 1.6,
                                side * 0.85 + warp * 0.7);

            float plates = fbm(crustUV);
            // Cracks are the low ridges between plates; the hotter the flow,
            // the wider they gape and the more incandescence shows through.
            float crack = 1.0 - smoothstep(0.0, 0.18 + lavaHeat * 0.22,
                                           abs(plates - 0.5));
            float fine  = fbm(crustUV * 3.7 + vec2(drift * 0.4, 0.0));
            crack = max(crack, (1.0 - smoothstep(0.0, 0.07, abs(fine - 0.5)))
                               * lavaHeat * 0.7);

            // A fast flow tears its crust apart; a stalled one skins over.
            float exposure = clamp(crack * (0.35 + lavaHeat) + lavaHeat * 0.30,
                                   0.0, 1.0);

            // Interior temperature: the channel core runs hotter than its
            // margins, so deep lava glows brighter than the same lava spread
            // thin over the same rock.
            float coreT = lavaHeat * (0.55 + 0.45 * smoothstep(0.05, 1.2, molten));
            vec3 glowColor = blackbody(coreT);

            // Chilled crust: nearly black, with the plates faintly visible.
            vec3 crustColor = toLinear(vec3(0.035, 0.030, 0.032))
                            * (0.6 + plates * 0.8);

            vec3 lavaSurface = mix(crustColor * (sun * 0.35 + ambient),
                                   glowColor, exposure);
            // Emission proper — unlit, so it stays bright in shadow and at
            // night, which is the whole point of a self-luminous material.
            //
            // Driven well past 1.0 on purpose. The tone mapper compresses the
            // top end, so an emitter that peaks at white-on-the-wire lands as
            // a dull cream once it has been rolled off; overdriving it is what
            // buys back a core that actually reads as incandescent.
            vec3 emission = glowColor * exposure * (0.7 + coreT * 2.6);
            // Even sealed crust radiates while it is still hot.
            emission += blackbody(coreT * 0.55) * lavaHeat * 0.25;

            // Molten rock is glassy: a tight, strong highlight.
            float lavaSpec = pow(max(dot(n, halfDir), 0.0), 90.0);
            lavaSurface += toLinear(vec3(1.0, 0.85, 0.6)) * lavaSpec * 0.5;

            finalColor = mix(finalColor, lavaSurface + emission, lavaMask);
          }

          // ---- Atmosphere --------------------------------------------------
          // Distance from the *camera*, with a height falloff.
          //
          // This measured distance from the terrain's origin, so haze formed a
          // fixed bowl centred on the map: it thickened as you looked outward
          // even from a metre away, and never changed when you flew backwards.
          // That is the opposite of aerial perspective, which is precisely the
          // cue an eye uses to read scale — and terrain is nothing but scale.
          // Distance is normalized by the terrain extent, so one density
          // setting means the same amount of haze whether the map is 128 or
          // 2048 units across — the same scale-independence the strata and
          // erosion constants already follow.
          float dist = length(cameraPosition - vWorldPos) / max(uTerrainExtent, 1.0);
          float heightFalloff = exp(-max(vWorldPos.y, 0.0) / max(uTerrainExtent * 0.35, 1.0));
          float fog = 1.0 - exp(-dist * uFogDensity * 120.0 * heightFalloff);

          // Haze scatters the sun: looking toward it, the air glows.
          float sunAmount = max(dot(-viewDir, lightDir), 0.0);
          vec3 fogCol = mix(toLinear(uFogColor),
                            sunTint * 1.15,
                            pow(sunAmount, 6.0) * 0.55);
          finalColor = mix(finalColor, fogCol, clamp(fog, 0.0, 1.0));

          // ---- Output ------------------------------------------------------
          finalColor = tonemapACES(finalColor * uExposure);
          gl_FragColor = vec4(toSRGB(finalColor), 1.0);
        }
      `,
    });

    materialRef.current = material;
    const mesh = new THREE.Mesh(geometry, material);
    sceneRef.current.add(mesh);
    meshRef.current = mesh;

    updateAtmosphere();

    setStats(prev => ({
      ...prev,
      vertices: meshData.vertices.length / 3,
      triangles: meshData.indices.length / 3
    }));

    if (minimapRef.current) updateMinimap();
  
    refreshHeightRange();
  }, [params.biome, sunElevation, sunAzimuth, sunIntensity, fogDensity, updateAtmosphere, updateMinimap, refreshHeightRange]);

  useEffect(() => {
    if (!canvasRef.current) return;

    // 1. Core Scene Setup (Infrastructure) - Only runs once
    const scene = new THREE.Scene();
    const skyColor = new THREE.Color(0x87CEEB);
    scene.background = skyColor;
    scene.fog = new THREE.FogExp2(skyColor, 0.002);
    sceneRef.current = scene;

    // Far plane sized for the largest terrain the resolution slider allows
    // (2048 world units across at cellSize 1), plus headroom to orbit out.
    const camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.1, 12000);
    camera.position.set(100, 100, 100);
    cameraRef.current = camera;

    const renderer = new THREE.WebGLRenderer({ 
      canvas: canvasRef.current, 
      antialias: true,
      powerPreference: 'high-performance'
    });
    renderer.setSize(window.innerWidth, window.innerHeight);
    renderer.setPixelRatio(Math.min(window.devicePixelRatio, 2));
    renderer.setClearColor(skyColor, 1.0);
    // The terrain shader owns its whole colour chain — it decodes its palettes
    // to linear, tone maps, and encodes back to sRGB itself. Telling Three.js
    // the output is already sRGB stops it applying a second conversion on top.
    renderer.outputColorSpace = THREE.SRGBColorSpace;
    rendererRef.current = renderer;

    const controls = new OrbitControls(camera, renderer.domElement);
    controls.enableDamping = true;
    controlsRef.current = controls;

    // Lighting
    const ambientLight = new THREE.AmbientLight(0x404040, 0.6);
    scene.add(ambientLight);

    const sunLight = new THREE.DirectionalLight(0xffffff, 1.4);
    sunLight.position.set(100, 200, 100);
    sunLight.castShadow = true;
    scene.add(sunLight);
    sunLightRef.current = sunLight;

    // Sea-level plane. Segmented, because the shader displaces its vertices
    // into swell — the previous single-quad geometry had four corner vertices,
    // so every ripple the vertex shader computed was interpolated flat across
    // the whole ocean and none of it was ever visible.
    const waterGeom = new THREE.PlaneGeometry(4000, 4000, 160, 160);
    const waterMat = new THREE.ShaderMaterial({
      transparent: true,
      side: THREE.DoubleSide,
      uniforms: {
        uTime: { value: 0 },
        uColor: { value: new THREE.Color(0x004e7c) },
        uSunPos: { value: new THREE.Vector3(1, 1, 1) }
      },
      vertexShader: `
        varying vec3 vWorldPos;
        uniform float uTime;

        void main() {
          vec3 pos = position;
          // Displacement is along the plane's local z, which becomes world Y
          // once the mesh is rotated flat — this is the swell.
          pos.z += sin(pos.x * 0.05 + uTime) * 0.35;
          pos.z += cos(pos.y * 0.07 + uTime * 1.2) * 0.25;
          pos.z += sin((pos.x + pos.y) * 0.021 - uTime * 0.6) * 0.5;
          vec4 worldPos = modelMatrix * vec4(pos, 1.0);
          vWorldPos = worldPos.xyz;
          gl_Position = projectionMatrix * modelViewMatrix * vec4(pos, 1.0);
        }
      `,
      fragmentShader: `
        varying vec3 vWorldPos;
        uniform vec3 uColor;
        uniform vec3 uSunPos;
        uniform float uTime;

        vec3 toLinear(vec3 c) { return pow(max(c, vec3(0.0)), vec3(2.2)); }
        vec3 toSRGB(vec3 c)   { return pow(max(c, vec3(0.0)), vec3(1.0 / 2.2)); }
        vec3 tonemapACES(vec3 x) {
          const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
          return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
        }

        float h21(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
        float vn(vec2 p) {
          vec2 i = floor(p), f = fract(p);
          f = f * f * (3.0 - 2.0 * f);
          return mix(mix(h21(i), h21(i + vec2(1, 0)), f.x),
                     mix(h21(i + vec2(0, 1)), h21(i + vec2(1, 1)), f.x), f.y);
        }

        void main() {
          vec3 viewDir = normalize(cameraPosition - vWorldPos);
          vec3 lightDir = normalize(uSunPos);

          // Ripple normal, so the sea is not a mirror-flat pane. Two drifting
          // octaves give a moving surface without needing a normal map.
          float e = 0.6;
          vec2 q = vWorldPos.xz * 0.06;
          float n0 = vn(q + vec2(uTime * 0.05, 0.0)) + vn(q * 2.7 - vec2(0.0, uTime * 0.08)) * 0.5;
          float nx = vn(q + vec2(e, 0.0) + vec2(uTime * 0.05, 0.0))
                   + vn((q + vec2(e, 0.0)) * 2.7 - vec2(0.0, uTime * 0.08)) * 0.5;
          float nz = vn(q + vec2(0.0, e) + vec2(uTime * 0.05, 0.0))
                   + vn((q + vec2(0.0, e)) * 2.7 - vec2(0.0, uTime * 0.08)) * 0.5;
          vec3 nrm = normalize(vec3(-(nx - n0) * 0.35, 1.0, -(nz - n0) * 0.35));

          float fresnel = pow(1.0 - max(dot(viewDir, nrm), 0.0), 4.0);
          vec3 deep = toLinear(uColor);
          vec3 sky  = toLinear(vec3(0.42, 0.62, 0.82));
          vec3 color = mix(deep, sky, clamp(fresnel * 1.2, 0.0, 1.0));

          // Sun glitter.
          vec3 halfDir = normalize(lightDir + viewDir);
          float spec = pow(max(dot(nrm, halfDir), 0.0), 220.0);
          color += toLinear(vec3(1.0, 0.95, 0.85)) * spec * 3.0;

          gl_FragColor = vec4(toSRGB(tonemapACES(color)), 0.86);
        }
      `
    });
    const water = new THREE.Mesh(waterGeom, waterMat);
    water.rotation.x = -Math.PI / 2;
    water.position.y = waterLevel;
    scene.add(water);
    waterRef.current = water;

    // Rebuilt when the resolution changes — see the gridRef effect. A fixed
    // 200-unit grid vanished under a 2048-wide terrain.
    const grid = new THREE.GridHelper(200, 20, 0x333333, 0x222222);
    grid.position.y = -0.1;
    scene.add(grid);
    gridRef.current = grid;

    // Animation loop
    let animationId: number;
    let startTime = performance.now();
    const animate = () => {
      animationId = requestAnimationFrame(animate);
      const time = (performance.now() - startTime) * 0.001;
      
      if (waterRef.current) {
        (waterRef.current.material as THREE.ShaderMaterial).uniforms.uTime.value = time;
      }
      if (meshRef.current) {
        (meshRef.current.material as THREE.ShaderMaterial).uniforms.uTime.value = time;
      }

      controls.update();
      renderer.render(scene, camera);
    };
    animate();

    // Resize handler
    const handleResize = () => {
      camera.aspect = window.innerWidth / window.innerHeight;
      camera.updateProjectionMatrix();
      renderer.setSize(window.innerWidth, window.innerHeight);
    };
    window.addEventListener('resize', handleResize);

    return () => {
      window.removeEventListener('resize', handleResize);
      cancelAnimationFrame(animationId);
      renderer.dispose();
      (scene as any).dispose?.(); // Clean up if needed
    };
  }, []); // Only run once on mount

  // --- Volcano placement ---------------------------------------------------

  // Drops a volcano at a normalized position and returns its layer id so the
  // drag handler can keep moving it.
  //
  // A volcano is inert without an eruption, so the first one placed also
  // appends a Lava Flow layer. It goes at the end of the stack because it has
  // to run after every volcano that feeds it — placing a second cone then
  // reuses that same layer, which is what makes several vents erupt together
  // into one shared flow field rather than each getting its own pass.
  const placeVolcano = React.useCallback((nx: number, ny: number): string => {
    const volcano = makeLayer('volcano');
    volcano.params.x = nx;
    volcano.params.y = ny;

    setStack(s => {
      const volcanoCount = s.filter(l => l.type === 'volcano').length;
      volcano.params.variant = volcanoCount % 100;

      const lavaIndex = s.findIndex(l => l.type === 'lava');
      if (lavaIndex >= 0) {
        // Keep the eruption downstream of the new cone.
        const next = [...s];
        next.splice(lavaIndex, 0, volcano);
        return next;
      }
      return [...s, volcano, makeLayer('lava')];
    });

    return volcano.id;
  }, []);

  const moveVolcano = React.useCallback((id: string, nx: number, ny: number) => {
    setStack(s => s.map(l =>
      l.id === id ? { ...l, params: { ...l.params, x: nx, y: ny } } : l));
  }, []);

  useEffect(() => {
    // 2. Interaction Layer (Raycasting/Events) - Re-bind on mode changes
    if (!rendererRef.current || !cameraRef.current || !sceneRef.current) return;

    const raycaster = new THREE.Raycaster();
    const mouse = new THREE.Vector2();

    const handlePointerMove = (event: PointerEvent) => {
      const camera = cameraRef.current;
      const mesh = meshRef.current;
      const engine = engineRef.current;

      if ((!isInspectMode && !isCarveMode && !isVolcanoMode) || !camera || !mesh || !engine) {
        setProbeData(null);
        return;
      }

      mouse.x = (event.clientX / window.innerWidth) * 2 - 1;
      mouse.y = -(event.clientY / window.innerHeight) * 2 + 1;

      raycaster.setFromCamera(mouse, camera);
      const intersects = raycaster.intersectObject(mesh);

      if (intersects.length > 0) {
        const point = intersects[0].point;
        const size = params.size;

        const x = Math.round(point.x + size / 2);
        const y = Math.round(point.z + size / 2);

        if (x >= 0 && x < size && y >= 0 && y < size) {
          if (isInspectMode) {
            const h = engine.getHeight(x, y);
            const s = engine.getSediment(x, y);
            const f = engine.getFlow(x, y);
            const slope = engine.calculateSlope(x, y);
            setProbeData({ x, y, h, s, f, slope });
          }

          if (isCarveMode && isMouseDown.current) {
            engine.carve(x, y, carveRadius, carveDepth);
            updateMesh();
          }

          // Dragging a freshly dropped volcano: rewrite its position rather
          // than placing another one.
          if (isVolcanoMode && isMouseDown.current && draggingVolcanoRef.current) {
            moveVolcano(draggingVolcanoRef.current, x / size, y / size);
          }
        }
      } else {
        setProbeData(null);
      }
    };

    const handlePointerDown = (event: PointerEvent) => {
      if (isCarveMode) {
        isMouseDown.current = true;
        if (controlsRef.current) controlsRef.current.enabled = false;
        handlePointerMove(event);
        return;
      }

      if (isVolcanoMode) {
        const camera = cameraRef.current;
        const mesh = meshRef.current;
        if (!camera || !mesh) return;

        mouse.x = (event.clientX / window.innerWidth) * 2 - 1;
        mouse.y = -(event.clientY / window.innerHeight) * 2 + 1;
        raycaster.setFromCamera(mouse, camera);
        const hits = raycaster.intersectObject(mesh);
        if (hits.length === 0) return;

        const size = params.size;
        const gx = (hits[0].point.x + size / 2) / size;
        const gy = (hits[0].point.z + size / 2) / size;
        if (gx < 0 || gx > 1 || gy < 0 || gy > 1) return;

        isMouseDown.current = true;
        if (controlsRef.current) controlsRef.current.enabled = false;
        draggingVolcanoRef.current = placeVolcano(gx, gy);
      }
    };

    const handlePointerUp = () => {
      isMouseDown.current = false;
      draggingVolcanoRef.current = null;
      if (controlsRef.current) controlsRef.current.enabled = true;
    };

    window.addEventListener('pointermove', handlePointerMove);
    window.addEventListener('pointerdown', handlePointerDown);
    window.addEventListener('pointerup', handlePointerUp);

    return () => {
      window.removeEventListener('pointermove', handlePointerMove);
      window.removeEventListener('pointerdown', handlePointerDown);
      window.removeEventListener('pointerup', handlePointerUp);
    };
  }, [isInspectMode, isCarveMode, isVolcanoMode, carveRadius, carveDepth,
      params.size, updateMesh, placeVolcano, moveVolcano]);

  const floraRef = useRef<THREE.InstancedMesh | null>(null);

  const scatterFlora = React.useCallback(() => {
    if (!sceneRef.current || !engineRef.current) return;
    
    // Remove old flora
    if (floraRef.current) {
      sceneRef.current.remove(floraRef.current);
      floraRef.current.geometry.dispose();
      (floraRef.current.material as THREE.Material).dispose();
      floraRef.current = null;
    }

    const size = params.size;
    const maxFlora = floraDensity;
    const instances: THREE.Matrix4[] = [];
    const dummy = new THREE.Object3D();
    
    const bedrock = engineRef.current.getBedrockMap();
    const sediment = engineRef.current.getSedimentMap();
    const flow = engineRef.current.getFlowMap();
    
    for (let i = 0; i < maxFlora; i++) {
      const x = Math.floor(Math.random() * size);
      const y = Math.floor(Math.random() * size);
      const idx = y * size + x;
      
      const b = bedrock[idx];
      const s = sediment[idx];
      const h = b + s;
      const f = flow[idx];
      
      // Placement rules
      const slope = engineRef.current.calculateSlope(x, y);
      const heightNorm = h / params.heightMultiplier;
      
      // Biome-specific placement rules
      if (params.biome === 'arctic') {
        if (heightNorm > 0.4 || h < waterLevel + 1) continue; // Only low areas, not in snow/water
        if (Math.random() > 0.1) continue; // Extra sparse
      } else if (params.biome === 'desert') {
        if (f < 0.3 || h < waterLevel + 1) continue; // Only near water/flow
      } else if (params.biome === 'volcanic') {
        if (slope > 0.3 || heightNorm > 0.8) continue;
      } else { // Temperate
        if (slope > 0.5 || heightNorm > 0.75 || h < waterLevel + 1) continue;
      }
      
      // Prefer valleys with sediment and flow
      const probability = (s / 5.0) * 0.5 + (f * 0.5);
      if (Math.random() > probability && i > 500) continue; 
      
      dummy.position.set(x - size / 2, h, y - size / 2);
      dummy.scale.setScalar(0.5 + Math.random() * 0.5);
      
      // Variation for desert (cactus-like)
      if (params.biome === 'desert') {
        dummy.scale.set(0.2, 0.8 + Math.random() * 1.5, 0.2);
      }
      
      dummy.rotation.y = Math.random() * Math.PI * 2;
      dummy.updateMatrix();
      instances.push(dummy.matrix.clone());
    }
    
    if (instances.length === 0) return;
    
    // Create vegetation based on biome
    let geometry;
    let color;
    
    if (params.biome === 'desert') {
      geometry = new THREE.CylinderGeometry(0.2, 0.2, 2, 6);
      color = 0x2d5a27;
    } else if (params.biome === 'arctic') {
      geometry = new THREE.ConeGeometry(0.4, 1.2, 5);
      color = 0x2f4f4f;
    } else if (params.biome === 'volcanic') {
      geometry = new THREE.IcosahedronGeometry(0.5, 0);
      color = 0x332211;
    } else {
      geometry = new THREE.ConeGeometry(0.3, 1.5, 6);
      color = 0x1a3300;
    }
    
    geometry.translate(0, params.biome === 'volcanic' ? 0.25 : 0.75, 0); // Pivot at base
    const material = new THREE.MeshPhongMaterial({ color });
    
    const imesh = new THREE.InstancedMesh(geometry, material, instances.length);
    instances.forEach((m, idx) => imesh.setMatrixAt(idx, m));
    
    sceneRef.current.add(imesh);
    floraRef.current = imesh;
    
    setStats(prev => ({ ...prev, flora: instances.length }));
  }, [params.size, params.heightMultiplier, params.biome, floraDensity]);

  // Runs the whole layer stack, chunked so the viewport updates live and the
  // UI never freezes. A newer run (or Cancel) simply invalidates this one.
  const runStack = React.useCallback(async () => {
    const engine = engineRef.current;
    if (!sceneRef.current || !engine) return;
    const myRun = ++runIdRef.current;
    setIsGenerating(true);
    setError(null);
    const startTime = performance.now();

    try {
      const project: TitanProject = { version: 1, params, stack, imported };
      const completed = await runPipeline(engine, project, {
        onProgress: (frac, label) => {
          if (runIdRef.current === myRun) setProgress({ frac, label });
        },
        onChunk: () => {
          if (runIdRef.current === myRun) updateMesh();
        },
        shouldCancel: () => runIdRef.current !== myRun,
      });
      if (completed && runIdRef.current === myRun) {
        // Trace ambient occlusion once the stack has settled, then rebuild the
        // mesh so it carries it. This is deliberately outside the chunk loop:
        // AO is the engine's most expensive derived map and re-tracing it per
        // chunk would cost more than every simulation pass combined.
        engine.computeAO();
        updateMesh();
        setStats(prev => ({ ...prev, time: Math.round(performance.now() - startTime) }));
      }
    } catch (err) {
      console.error('Pipeline failed:', err);
      setError(err instanceof Error ? err.message : 'Unknown error during generation');
    } finally {
      if (runIdRef.current === myRun) {
        setIsGenerating(false);
        setProgress(null);
      }
    }
  }, [params, stack, imported, updateMesh]);

  const cancelRun = React.useCallback(() => {
    runIdRef.current++;
    setIsGenerating(false);
    setProgress(null);
  }, []);

  // "Regenerate" pulls a fresh seed unless the user has locked it, so no two
  // generations ever look the same by accident.
  const handleRegenerate = React.useCallback(() => {
    if (seedLocked) {
      runStack();
    } else {
      setParams(p => ({ ...p, seed: randomSeed() }));
    }
  }, [seedLocked, runStack]);

  // --- Undo/redo over (params, stack, import) snapshots -------------------
  useEffect(() => {
    const snapshot = serializeProject({ version: 1, params, stack, imported });
    const h = historyRef.current;
    if (applyingHistoryRef.current) {
      applyingHistoryRef.current = false;
    } else if (h.past[h.past.length - 1] !== snapshot) {
      h.past.push(snapshot);
      if (h.past.length > 50) h.past.shift();
      h.future = [];
    }
    setHistoryState({ canUndo: h.past.length > 1, canRedo: h.future.length > 0 });
  }, [params, stack, imported]);

  const applySnapshot = React.useCallback((json: string) => {
    const project = deserializeProject(json);
    applyingHistoryRef.current = true;
    setParams(project.params);
    setStack(project.stack);
    setImported(project.imported);
  }, []);

  const undo = React.useCallback(() => {
    const h = historyRef.current;
    if (h.past.length < 2) return;
    h.future.push(h.past.pop()!);
    applySnapshot(h.past[h.past.length - 1]);
    setHistoryState({ canUndo: h.past.length > 1, canRedo: true });
  }, [applySnapshot]);

  const redo = React.useCallback(() => {
    const h = historyRef.current;
    const next = h.future.pop();
    if (!next) return;
    h.past.push(next);
    applySnapshot(next);
    setHistoryState({ canUndo: true, canRedo: h.future.length > 0 });
  }, [applySnapshot]);

  // --- Stack editing helpers ----------------------------------------------
  const addLayer = (type: LayerType) => setStack(s => [...s, makeLayer(type)]);
  const removeLayer = (id: string) => setStack(s => s.filter(l => l.id !== id));
  const toggleLayer = (id: string, enabled: boolean) =>
    setStack(s => s.map(l => (l.id === id ? { ...l, enabled } : l)));
  const setLayerParam = (id: string, key: string, value: number) =>
    setStack(s => s.map(l => (l.id === id ? { ...l, params: { ...l.params, [key]: value } } : l)));
  const setLayerMask = (id: string, patch: Partial<LayerMask>) =>
    setStack(s => s.map(l => (l.id === id ? { ...l, mask: { ...l.mask, ...patch } } : l)));
  const moveLayer = (id: string, dir: -1 | 1) =>
    setStack(s => {
      const i = s.findIndex(l => l.id === id);
      if (i < 0 || i + dir < 0 || i + dir >= s.length) return s;
      const copy = [...s];
      [copy[i], copy[i + dir]] = [copy[i + dir], copy[i]];
      return copy;
    });

  const applyPreset = (index: number) => {
    const { params: presetParams, stack: presetStack } = instantiatePreset(PRESETS[index]);
    setParams(p => ({
      ...p,
      ...presetParams,
      seed: seedLocked ? p.seed : randomSeed(),
    }));
    setStack(presetStack);
  };

  // --- Heightmap import ----------------------------------------------------
  // Decodes .png (any image), .r16 (uint16 LE RAW), or .r32 (float32 LE RAW)
  // into a normalized 0..1 square field. The engine resamples it to the
  // working resolution and the Height slider sets its vertical scale.
  const importHeightmap = async (file: File) => {
    const ext = file.name.split('.').pop()?.toLowerCase();
    try {
      let size: number;
      let data: Float32Array;

      if (ext === 'r16' || ext === 'raw') {
        const u16 = new Uint16Array(await file.arrayBuffer());
        size = Math.floor(Math.sqrt(u16.length));
        if (size < 2 || size * size !== u16.length) {
          throw new Error('RAW heightmap must be a square uint16 grid');
        }
        data = new Float32Array(size * size);
        for (let i = 0; i < data.length; i++) data[i] = u16[i] / 65535;
      } else if (ext === 'r32') {
        const f32 = new Float32Array(await file.arrayBuffer());
        size = Math.floor(Math.sqrt(f32.length));
        if (size < 2 || size * size !== f32.length) {
          throw new Error('RAW heightmap must be a square float32 grid');
        }
        let max = 0;
        for (let i = 0; i < f32.length; i++) max = Math.max(max, f32[i]);
        data = new Float32Array(size * size);
        for (let i = 0; i < data.length; i++) data[i] = max > 0 ? Math.max(0, f32[i]) / max : 0;
      } else {
        const bitmap = await createImageBitmap(file);
        size = Math.min(1024, Math.max(bitmap.width, bitmap.height));
        const canvas = document.createElement('canvas');
        canvas.width = size;
        canvas.height = size;
        const ctx = canvas.getContext('2d')!;
        ctx.drawImage(bitmap, 0, 0, size, size);
        const pixels = ctx.getImageData(0, 0, size, size).data;
        data = new Float32Array(size * size);
        for (let i = 0; i < data.length; i++) {
          data[i] = (pixels[i * 4] + pixels[i * 4 + 1] + pixels[i * 4 + 2]) / (3 * 255);
        }
        bitmap.close();
      }

      setImported({ size, data, name: file.name });
      // Flat noise structure makes the import the base terrain.
      setParams(p => ({ ...p, noiseType: 'none' }));
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Could not import heightmap');
    }
  };

  // --- Project save/load ---------------------------------------------------
  const saveProject = () => {
    const json = serializeProject({ version: 1, params, stack, imported });
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `terrain_${params.seed}.titan`;
    a.click();
    URL.revokeObjectURL(url);
  };

  const loadProject = async (file: File) => {
    try {
      const project = deserializeProject(await file.text());
      setParams(project.params);
      setStack(project.stack);
      setImported(project.imported);
      setError(null);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Could not read project file');
    }
  };

  const downloadBinary = (data: Uint8Array, filename: string) => {
    const blob = new Blob([data as BlobPart], { type: 'application/octet-stream' });
    const url = URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = filename;
    a.click();
    URL.revokeObjectURL(url);
  };

  const exportVia = (kind: ExportKind, ext: string) => {
    if (!engineRef.current) return;
    try {
      downloadBinary(
        engineRef.current.exportFile(kind),
        `titan_${params.seed}_${params.size}x${params.size}.${ext}`
      );
    } catch (err) {
      // The engine now reports failures (out of memory on a large grid being
      // the realistic one) instead of silently producing an empty file.
      setError(err instanceof Error ? err.message : 'Export failed');
    }
  };


  // exportHeightmapPNG and exportSplatmap used to build their images in
  // JavaScript from copied-out buffers. Both were wrong in ways the user could
  // not see: the heightmap was 8-bit (banded, useless as a heightmap) next to a
  // correct 16-bit C++ exporter, and the splatmap computed its rock mask as
  // slope * 2.5 while the mesh shader used (slope - 0.36) / 0.48, so the
  // exported masks did not line up with the terrain on screen. Both now go
  // through the engine, which shares one channel definition with the mesh.

  const updateParam = (key: keyof TerrainParams, value: any) => {
    console.log(`Updating param ${key} to ${value}`);
    if (key !== 'seed' && (typeof value !== 'number' || isNaN(value))) return;
    setParams(p => ({ ...p, [key]: value }));
  };

  useEffect(() => {
    console.log('Params updated:', params);
  }, [params]);

  // Reframe the camera when the terrain extent changes.
  useEffect(() => {
    frameTerrain(params.size);
  }, [params.size, frameTerrain]);

  // Resize the reference grid with the terrain. It spans the map plus a
  // margin, with one division per 1/16th, so it reads as a scale reference at
  // any resolution rather than a 200-unit postage stamp under a 2048 map.
  useEffect(() => {
    const scene = sceneRef.current;
    if (!scene) return;
    const extent = Math.max(64, params.size) * 1.5;
    const old = gridRef.current;
    if (old) {
      scene.remove(old);
      old.geometry.dispose();
      (old.material as THREE.Material).dispose();
    }
    const grid = new THREE.GridHelper(extent, 24, 0x3a3a3a, 0x252525);
    grid.position.y = -0.1;
    grid.visible = old ? old.visible : true;
    scene.add(grid);
    gridRef.current = grid;
  }, [params.size]);

  useEffect(() => {
    updateAtmosphere();
    if (waterRef.current) {
      waterRef.current.position.y = waterLevel;
    }
    if (minimapRef.current) updateMinimap();
  }, [waterLevel, sunElevation, sunAzimuth, sunIntensity, fogDensity, updateAtmosphere, updateMinimap]);

  useEffect(() => {
    // Set explicit background for the window to match sky
    document.body.style.backgroundColor = '#87CEEB';
    return () => {
      document.body.style.backgroundColor = '';
    };
  }, []);

  // Boot the C++ engine (WASM build of libTitanCore) once.
  useEffect(() => {
    let disposed = false;
    TitanCore.create()
      .then(engine => {
        if (disposed) {
          engine.dispose();
          return;
        }
        engineRef.current = engine;
        setEngineVersion(engine.version());
        setEngineStatus('ready');
      })
      .catch(err => {
        console.error('Failed to load libTitanCore WASM module:', err);
        setEngineStatus('error');
        setError('Engine failed to load — check console');
      });
    return () => {
      disposed = true;
      engineRef.current?.dispose();
      engineRef.current = null;
    };
  }, []);

  // Rebuild the full stack when anything changes (and once the engine is up)
  useEffect(() => {
    if (engineStatus !== 'ready') return;
    const timer = setTimeout(() => {
      runStack();
    }, 250);
    return () => clearTimeout(timer);
  }, [runStack, engineStatus]);

  return (
    <div className="relative w-full h-screen bg-[#87CEEB] text-zinc-100 font-sans overflow-hidden">
      {/* 3D Viewport */}
      <canvas ref={canvasRef} className="absolute inset-0 w-full h-full z-0" />

      {/* Header */}
      <header className="absolute top-0 left-0 w-full p-6 flex justify-between items-start pointer-events-none z-20">
        <div className="flex flex-col pointer-events-auto">
          <div className="flex items-center gap-3 mb-1">
            <div className="w-10 h-10 bg-zinc-100 flex items-center justify-center rounded-sm">
              <Mountain className="text-zinc-900 w-6 h-6" />
            </div>
            <div>
              <h1 className="text-2xl font-bold tracking-tighter uppercase">Titan Lab</h1>
              <p className="text-[10px] uppercase tracking-[0.2em] text-zinc-500 font-mono">Procedural Terrain Engine v1.0</p>
            </div>
          </div>
        </div>

        <div className="flex items-center gap-4 pointer-events-auto">
          <Card className="bg-zinc-900/80 backdrop-blur-md border-zinc-800 p-3 flex items-center gap-6">
            <div className="flex flex-col">
              <span className="text-[9px] uppercase text-zinc-500 font-mono">Vertices</span>
              <span className="text-sm font-mono">{stats.vertices.toLocaleString()}</span>
            </div>
            <div className="flex flex-col">
              <span className="text-[9px] uppercase text-zinc-500 font-mono">Triangles</span>
              <span className="text-sm font-mono">{stats.triangles.toLocaleString()}</span>
            </div>
            <div className="flex flex-col">
              <span className="text-[9px] uppercase text-zinc-500 font-mono">Compute</span>
              <span className="text-sm font-mono text-emerald-400">{stats.time}ms</span>
            </div>
            {stats.flora > 0 && (
              <div className="flex flex-col">
                <span className="text-[9px] uppercase text-zinc-500 font-mono">Flora</span>
                <span className="text-sm font-mono text-emerald-400">{stats.flora.toLocaleString()}</span>
              </div>
            )}
          </Card>
          <Button variant="outline" size="icon" className="bg-zinc-900/80 border-zinc-800 hover:bg-zinc-800">
            <Maximize2 className="w-4 h-4" />
          </Button>
        </div>
      </header>

      {/* Sidebar Toggle */}
      <button 
        onClick={() => setIsSidebarOpen(!isSidebarOpen)}
        className="absolute top-1/2 left-0 -translate-y-1/2 z-30 bg-zinc-900 border-y border-r border-zinc-800 p-1 rounded-r-md hover:bg-zinc-800 transition-colors"
      >
        {isSidebarOpen ? <ChevronLeft className="w-4 h-4" /> : <ChevronRight className="w-4 h-4" />}
      </button>

      {/* Sidebar */}
      {isSidebarOpen && (
        <div
          className="absolute top-0 left-0 h-full w-[380px] bg-zinc-950/90 backdrop-blur-xl border-r border-zinc-800 z-20 flex flex-col pt-24 pointer-events-auto"
        >
          <div className="flex-1 min-h-0 overflow-y-auto px-6 pb-8">
              {error && (
                <div className="mb-6 p-3 bg-red-500/10 border border-red-500/50 rounded text-[10px] text-red-400 font-mono uppercase tracking-wider">
                  Error: {error}
                </div>
              )}
              <div className="flex items-center justify-end gap-1 mb-3">
                <Button
                  variant="ghost"
                  size="sm"
                  className="h-7 px-2 text-zinc-500 hover:text-zinc-100 disabled:opacity-30"
                  onClick={undo}
                  disabled={!historyState.canUndo}
                  title="Undo"
                >
                  <Undo2 className="w-3.5 h-3.5" />
                </Button>
                <Button
                  variant="ghost"
                  size="sm"
                  className="h-7 px-2 text-zinc-500 hover:text-zinc-100 disabled:opacity-30"
                  onClick={redo}
                  disabled={!historyState.canRedo}
                  title="Redo"
                >
                  <Redo2 className="w-3.5 h-3.5" />
                </Button>
              </div>
              <Tabs defaultValue="generator" className="w-full">
                <TabsList className="grid w-full grid-cols-4 bg-zinc-900 mb-8">
                  <TabsTrigger value="generator" className="text-[10px] uppercase tracking-wider">Base</TabsTrigger>
                  <TabsTrigger value="erosion" className="text-[10px] uppercase tracking-wider">Stack</TabsTrigger>
                  <TabsTrigger value="render" className="text-[10px] uppercase tracking-wider">Render</TabsTrigger>
                  <TabsTrigger value="export" className="text-[10px] uppercase tracking-wider">Export</TabsTrigger>
                </TabsList>

                <TabsContent value="generator" className="space-y-8">
                  <div className="space-y-3">
                    <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Presets</Label>
                    <div className="space-y-2">
                      {PRESETS.map((preset, i) => (
                        <button
                          key={preset.name}
                          onClick={() => applyPreset(i)}
                          className="w-full text-left p-3 bg-zinc-900 hover:bg-zinc-800 border border-zinc-800 hover:border-emerald-500/40 rounded-lg transition-colors group"
                        >
                          <div className="text-xs font-bold uppercase tracking-widest group-hover:text-emerald-400">{preset.name}</div>
                          <div className="text-[10px] text-zinc-500 mt-0.5">{preset.tagline}</div>
                        </button>
                      ))}
                    </div>
                  </div>

                  <Separator className="bg-zinc-800" />

                  <div className="space-y-4">
                    <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Biological Biome</Label>
                    <div className="grid grid-cols-4 gap-2">
                       {[
                         { id: 'temperate', icon: Trees, label: 'Forest' },
                         { id: 'arctic', icon: Mountain, label: 'Arctic' },
                         { id: 'volcanic', icon: Activity, label: 'Vulcan' },
                         { id: 'desert', icon: Droplets, label: 'Desert' }
                       ].map((t) => (
                         <Button
                           key={t.id}
                           variant={params.biome === t.id ? 'default' : 'outline'}
                           className={`flex flex-col gap-1 h-14 bg-zinc-900 border-zinc-800 hover:bg-zinc-800 ${params.biome === t.id ? 'border-emerald-500/50 text-emerald-400' : ''}`}
                           onClick={() => setParams(p => ({ ...p, biome: t.id as any }))}
                         >
                           <t.icon className="w-4 h-4" />
                           <span className="text-[8px] uppercase tracking-tighter">{t.label}</span>
                         </Button>
                       ))}
                    </div>
                  </div>

                  <div className="space-y-4">
                    <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Noise Structure</Label>
                    <div className="grid grid-cols-4 gap-2">
                       {[
                         { id: 'none', icon: Minus, label: 'Flat' },
                         { id: 'standard', icon: Mountain, label: 'Simplex' },
                         { id: 'ridged', icon: Activity, label: 'Ridged' },
                         { id: 'billow', icon: Cloud, label: 'Billow' },
                         { id: 'voronoi', icon: Hexagon, label: 'Cells' },
                         { id: 'voronoiRidge', icon: Grid3X3, label: 'Walls' },
                         { id: 'worleyManhattan', icon: Grid2X2, label: 'Worley' },
                         { id: 'hybrid', icon: Layers, label: 'Hybrid' }
                       ].map((t) => (
                         <Button
                           key={t.id}
                           variant={params.noiseType === t.id ? 'default' : 'outline'}
                           className={`flex flex-col gap-1 h-14 bg-zinc-900 border-zinc-800 hover:bg-zinc-800 ${params.noiseType === t.id ? 'border-emerald-500/50 text-emerald-400' : ''}`}
                           onClick={() => setParams(p => ({ ...p, noiseType: t.id as any }))}
                         >
                           <t.icon className="w-4 h-4" />
                           <span className="text-[8px] uppercase tracking-tighter">{t.label}</span>
                         </Button>
                       ))}
                    </div>
                  </div>

                  <div className="space-y-3">
                    <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Heightmap Import</Label>
                    {imported ? (
                      <div className="flex items-center gap-2 p-3 bg-zinc-900 rounded border border-emerald-500/30">
                        <ImageIcon className="w-4 h-4 text-emerald-400 shrink-0" />
                        <div className="flex-1 min-w-0">
                          <div className="text-[10px] font-bold truncate">{imported.name ?? 'Imported heightmap'}</div>
                          <div className="text-[9px] text-zinc-500 font-mono">{imported.size}×{imported.size} · scaled by Height</div>
                        </div>
                        <Button variant="ghost" size="sm" className="h-6 w-6 p-0 text-zinc-500 hover:text-red-400"
                          onClick={() => setImported(undefined)}>
                          <X className="w-3 h-3" />
                        </Button>
                      </div>
                    ) : (
                      <Button
                        variant="outline"
                        className="w-full border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-10"
                        onClick={() => importFileRef.current?.click()}
                      >
                        <Upload className="w-3 h-3 mr-2" />
                        Import .png / .r16 / .r32
                      </Button>
                    )}
                    <input
                      ref={importFileRef}
                      type="file"
                      accept=".png,.jpg,.jpeg,.webp,.r16,.r32,.raw,image/*"
                      className="hidden"
                      onChange={(e) => {
                        const file = e.target.files?.[0];
                        if (file) importHeightmap(file);
                        e.target.value = '';
                      }}
                    />
                    <p className="text-[9px] text-zinc-600 leading-relaxed">
                      The import becomes the base terrain (resampled to the working resolution). Blend it further with a Combine Import layer.
                    </p>
                  </div>

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Seed</Label>
                      <div className="flex items-center gap-1">
                        <Button
                          variant="ghost"
                          size="sm"
                          className={`h-6 text-[10px] px-2 ${seedLocked ? 'text-emerald-400' : 'text-zinc-500 hover:text-zinc-100'}`}
                          onClick={() => setSeedLocked(l => !l)}
                          title={seedLocked ? 'Seed locked — regenerate reproduces this exact terrain' : 'Seed unlocked — regenerate picks a fresh seed'}
                        >
                          {seedLocked ? <Lock className="w-3 h-3 mr-1" /> : <Unlock className="w-3 h-3 mr-1" />}
                          {seedLocked ? 'Locked' : 'Lock'}
                        </Button>
                        <Button
                          variant="ghost"
                          size="sm"
                          className="h-6 text-[10px] text-zinc-500 hover:text-zinc-100 px-2"
                          onClick={() => updateParam('seed', randomSeed() as any)}
                        >
                          <RefreshCw className="w-3 h-3 mr-1" />
                          Randomize
                        </Button>
                      </div>
                    </div>
                    <div className="p-3 bg-zinc-900 rounded border border-zinc-800 font-mono text-xs text-zinc-400 truncate">
                      {params.seed}
                    </div>
                  </div>

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Resolution</Label>
                      <span className="text-xs font-mono">{params.size}x{params.size}</span>
                    </div>
                    <Slider 
                      value={[params.size || 128]} 
                      min={64} 
                      max={2048} 
                      step={64} 
                      onValueChange={(v: number[]) => {
                        if (v && v.length > 0) updateParam('size', v[0]);
                      }}
                    />
                  </div>

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Scale</Label>
                      <span className="text-xs font-mono">{(params.scale || 0).toFixed(1)}</span>
                    </div>
                    <Slider 
                      value={[params.scale || 2.0]} 
                      min={0.1} 
                      max={10} 
                      step={0.1} 
                      onValueChange={(v: number[]) => {
                        if (v && v.length > 0) updateParam('scale', v[0]);
                      }}
                    />
                  </div>

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Height</Label>
                      <span className="text-xs font-mono">{params.heightMultiplier || 0}</span>
                    </div>
                    <Slider 
                      value={[params.heightMultiplier || 40]} 
                      min={10} 
                      max={500} 
                      step={1} 
                      onValueChange={(v: number[]) => {
                        if (v && v.length > 0) updateParam('heightMultiplier', v[0]);
                      }}
                    />
                  </div>

                  <Separator className="bg-zinc-800" />

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Octaves</Label>
                      <span className="text-xs font-mono">{params.octaves || 0}</span>
                    </div>
                    <Slider 
                      value={[params.octaves || 6]} 
                      min={1} 
                      max={12} 
                      step={1} 
                      onValueChange={(v: number[]) => {
                        if (v && v.length > 0) updateParam('octaves', v[0]);
                      }}
                    />
                  </div>

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Exponent</Label>
                      <span className="text-xs font-mono">{(params.exponent || 0).toFixed(2)}</span>
                    </div>
                    <Slider
                      value={[params.exponent || 1.2]}
                      min={0.5}
                      max={3.0}
                      step={0.05}
                      onValueChange={(v: number[]) => {
                        if (v && v.length > 0) updateParam('exponent', v[0]);
                      }}
                    />
                  </div>

                  <div className="space-y-4">
                    <div className="flex items-center justify-between">
                      <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Domain Warp</Label>
                      <span className="text-xs font-mono">{(params.warpStrength ?? 0).toFixed(2)}</span>
                    </div>
                    <Slider
                      value={[params.warpStrength ?? 0.5]}
                      min={0}
                      max={2.0}
                      step={0.05}
                      onValueChange={(v: number[]) => {
                        if (v && v.length > 0) updateParam('warpStrength', v[0]);
                      }}
                    />
                    <p className="text-[9px] text-zinc-600 leading-relaxed -mt-2">
                      Bends noise coordinates through a second noise field — turns blobby shapes into tectonic-looking flows.
                    </p>
                  </div>

                  <div className="pt-4">
                    <Button
                      onClick={handleRegenerate}
                      className="w-full bg-zinc-100 text-zinc-900 hover:bg-zinc-200 font-bold uppercase tracking-tighter"
                      disabled={isGenerating || engineStatus !== 'ready'}
                    >
                      {isGenerating ? <Activity className="w-4 h-4 animate-spin mr-2" /> : <RotateCcw className="w-4 h-4 mr-2" />}
                      Regenerate Core
                    </Button>
                  </div>
                </TabsContent>

                <TabsContent value="erosion" className="space-y-6">
                  <p className="text-[10px] text-zinc-500 leading-relaxed">
                    The stack runs top to bottom on every rebuild. Same seed + same stack = identical terrain, every time.
                  </p>

                  {stack.length === 0 && (
                    <div className="p-6 bg-zinc-900/50 border border-dashed border-zinc-800 rounded-lg text-center">
                      <span className="text-[10px] uppercase tracking-widest text-zinc-600">No layers yet — add one below or pick a preset</span>
                    </div>
                  )}

                  {stack.map((layer, idx) => {
                    const def = LAYER_DEFS[layer.type];
                    return (
                      <div key={layer.id} className={`p-4 bg-zinc-900 rounded-lg border ${layer.enabled ? 'border-zinc-800' : 'border-zinc-800/50 opacity-50'}`}>
                        <div className="flex items-center gap-2 mb-2">
                          <Switch checked={layer.enabled} onCheckedChange={(v) => toggleLayer(layer.id, v)} />
                          <h3 className="text-xs font-bold uppercase tracking-widest flex-1">{def.label}</h3>
                          <Button variant="ghost" size="sm" className="h-6 w-6 p-0 text-zinc-600 hover:text-zinc-200 disabled:opacity-20" disabled={idx === 0} onClick={() => moveLayer(layer.id, -1)}>
                            <ArrowUp className="w-3 h-3" />
                          </Button>
                          <Button variant="ghost" size="sm" className="h-6 w-6 p-0 text-zinc-600 hover:text-zinc-200 disabled:opacity-20" disabled={idx === stack.length - 1} onClick={() => moveLayer(layer.id, 1)}>
                            <ArrowDown className="w-3 h-3" />
                          </Button>
                          <Button variant="ghost" size="sm" className="h-6 w-6 p-0 text-zinc-600 hover:text-red-400" onClick={() => removeLayer(layer.id)}>
                            <Trash2 className="w-3 h-3" />
                          </Button>
                        </div>
                        <p className="text-[9px] text-zinc-600 mb-4 leading-relaxed">{def.description}</p>
                        <div className="space-y-4">
                          {def.params.filter(pd => !pd.advanced).map(pd => (
                            <div key={pd.key} className="space-y-2">
                              <div className="flex items-center justify-between">
                                <Label className="text-[10px] uppercase text-zinc-400">{pd.label}</Label>
                                {!pd.choices && (
                                  <span className="text-xs font-mono">{(layer.params[pd.key] ?? pd.defaultValue).toLocaleString()}</span>
                                )}
                              </div>
                              {pd.choices ? (
                                <div className="grid grid-cols-2 gap-2">
                                  {pd.choices.map((choice, ci) => (
                                    <Button key={choice} variant="outline" size="sm"
                                      className={`h-7 text-[9px] uppercase tracking-wider bg-zinc-950 border-zinc-800 ${layer.params[pd.key] === ci ? 'border-emerald-500/50 text-emerald-400' : 'text-zinc-500'}`}
                                      onClick={() => setLayerParam(layer.id, pd.key, ci)}>
                                      {choice}
                                    </Button>
                                  ))}
                                </div>
                              ) : (
                                <Slider value={[layer.params[pd.key] ?? pd.defaultValue]} min={pd.min} max={pd.max} step={pd.step}
                                  onValueChange={(v: number[]) => { if (v && v.length > 0) setLayerParam(layer.id, pd.key, v[0]); }} />
                              )}
                            </div>
                          ))}

                          {/* Advanced physics. These map onto the engine's _ex
                              erosion entry points, which shipped in v0.4 and
                              were reachable from no UI until now. Collapsed by
                              default so the common case stays two sliders. */}
                          {def.params.some(pd => pd.advanced) && (
                            <div className="pt-2">
                              <button
                                className="text-[9px] uppercase tracking-widest text-zinc-500 hover:text-emerald-400"
                                onClick={() => setExpandedAdvanced(prev => ({ ...prev, [layer.id]: !prev[layer.id] }))}
                              >
                                {expandedAdvanced[layer.id] ? '- ' : '+ '}Advanced ({def.params.filter(pd => pd.advanced).length})
                              </button>
                              {expandedAdvanced[layer.id] && (
                                <div className="mt-3 space-y-4 pl-2 border-l border-zinc-800">
                                  {def.params.filter(pd => pd.advanced).map(pd => (
                                    <div key={pd.key} className="space-y-2">
                                      <div className="flex items-center justify-between">
                                        <Label className="text-[10px] uppercase text-zinc-400">{pd.label}</Label>
                                        <span className="text-xs font-mono">{(layer.params[pd.key] ?? pd.defaultValue).toLocaleString()}</span>
                                      </div>
                                      <Slider value={[layer.params[pd.key] ?? pd.defaultValue]} min={pd.min} max={pd.max} step={pd.step}
                                        onValueChange={(v: number[]) => { if (v && v.length > 0) setLayerParam(layer.id, pd.key, v[0]); }} />
                                    </div>
                                  ))}
                                </div>
                              )}
                            </div>
                          )}

                          {/* Per-layer mask: gate this layer by a terrain feature */}
                          <div className="pt-3 border-t border-zinc-800/70 space-y-3">
                            <div className="flex items-center justify-between">
                              <Label className="text-[10px] uppercase text-zinc-500">Mask</Label>
                              {layer.mask.mode > 0 && (
                                <button
                                  className={`text-[9px] uppercase tracking-wider ${layer.mask.invert ? 'text-emerald-400' : 'text-zinc-600 hover:text-zinc-300'}`}
                                  onClick={() => setLayerMask(layer.id, { invert: !layer.mask.invert })}
                                >
                                  Invert {layer.mask.invert ? 'On' : 'Off'}
                                </button>
                              )}
                            </div>
                            <div className="grid grid-cols-5 gap-1">
                              {MASK_MODE_LABELS.map((label, mi) => (
                                <Button key={label} variant="outline" size="sm"
                                  className={`h-6 px-0 text-[8px] uppercase tracking-tight bg-zinc-950 border-zinc-800 ${layer.mask.mode === mi ? 'border-emerald-500/50 text-emerald-400' : 'text-zinc-500'}`}
                                  onClick={() => setLayerMask(layer.id, { mode: mi })}>
                                  {label}
                                </Button>
                              ))}
                            </div>
                            {layer.mask.mode > 0 && (
                              <>
                                <div className="space-y-2">
                                  <div className="flex justify-between">
                                    <Label className="text-[9px] text-zinc-500 uppercase">Band Min</Label>
                                    <span className="text-[9px] font-mono">{layer.mask.lo.toFixed(2)}</span>
                                  </div>
                                  <Slider value={[layer.mask.lo]} min={0} max={1} step={0.05}
                                    onValueChange={(v: number[]) => { if (v && v.length > 0) setLayerMask(layer.id, { lo: v[0] }); }} />
                                </div>
                                <div className="space-y-2">
                                  <div className="flex justify-between">
                                    <Label className="text-[9px] text-zinc-500 uppercase">Band Max</Label>
                                    <span className="text-[9px] font-mono">{layer.mask.hi.toFixed(2)}</span>
                                  </div>
                                  <Slider value={[layer.mask.hi]} min={0} max={1} step={0.05}
                                    onValueChange={(v: number[]) => { if (v && v.length > 0) setLayerMask(layer.id, { hi: v[0] }); }} />
                                </div>
                              </>
                            )}
                          </div>
                        </div>
                      </div>
                    );
                  })}

                  <div className="space-y-2">
                    <Label className="text-[11px] uppercase tracking-widest text-zinc-400">Add Layer</Label>
                    <div className="grid grid-cols-2 gap-2">
                      {(Object.keys(LAYER_DEFS) as LayerType[]).map(type => (
                        <Button key={type} variant="outline"
                          className="h-9 justify-start text-[10px] uppercase tracking-wider bg-zinc-900 border-zinc-800 hover:bg-zinc-800"
                          onClick={() => addLayer(type)}>
                          <Plus className="w-3 h-3 mr-2" />
                          {LAYER_DEFS[type].label}
                        </Button>
                      ))}
                    </div>
                  </div>

                  <div className="p-4 bg-zinc-900 rounded-lg border border-orange-500/30">
                    <div className="flex items-center gap-3 mb-3">
                      <Flame className="w-5 h-5 text-orange-400" />
                      <h3 className="text-xs font-bold uppercase tracking-widest">Volcanoes</h3>
                    </div>
                    <p className="text-[10px] text-zinc-500 mb-4 leading-relaxed">
                      Turn this on and press anywhere on the terrain to drop a volcano — drag before releasing to position it.
                      Each one adds a Volcano layer you can tune below; a single Lava Flow layer erupts them all.
                    </p>
                    <Button
                      onClick={() => {
                        setIsVolcanoMode(v => !v);
                        setIsCarveMode(false);
                        setIsInspectMode(false);
                      }}
                      className={`w-full text-xs uppercase font-bold tracking-widest ${
                        isVolcanoMode
                          ? 'bg-orange-500 hover:bg-orange-400 text-zinc-950'
                          : 'bg-zinc-800 hover:bg-zinc-700 text-zinc-100'
                      }`}
                      disabled={isGenerating}
                    >
                      <Flame className="w-3 h-3 mr-2" />
                      {isVolcanoMode ? 'Placing — click the terrain' : 'Place a Volcano'}
                    </Button>
                    <div className="grid grid-cols-2 gap-2 mt-3">
                      <Button
                        variant="outline"
                        className="border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-8"
                        onClick={() => placeVolcano(0.5, 0.5)}
                      >
                        <Plus className="w-3 h-3 mr-1" />
                        At Center
                      </Button>
                      <Button
                        variant="outline"
                        className="border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-8 disabled:opacity-30"
                        disabled={!stack.some(l => l.type === 'volcano')}
                        onClick={() => setStack(s => s.filter(
                          l => l.type !== 'volcano' && l.type !== 'lava'))}
                      >
                        <Trash2 className="w-3 h-3 mr-1" />
                        Clear All
                      </Button>
                    </div>
                    {stack.filter(l => l.type === 'volcano').length > 0 && (
                      <p className="text-[9px] text-orange-400/80 font-mono mt-3">
                        {stack.filter(l => l.type === 'volcano').length} volcano
                        {stack.filter(l => l.type === 'volcano').length === 1 ? '' : 'es'} placed
                      </p>
                    )}
                  </div>

                  <div className="p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex items-center gap-3 mb-3">
                      <Trees className="w-5 h-5 text-emerald-400" />
                      <h3 className="text-xs font-bold uppercase tracking-widest">Flora Scattering</h3>
                    </div>
                    <p className="text-[10px] text-zinc-500 mb-4 leading-relaxed">
                      Procedurally scatter vegetation based on moisture (flow), sediment depth, and slope analysis.
                    </p>
                    <div className="space-y-4 mb-6">
                      <div className="flex items-center justify-between">
                        <Label className="text-[10px] uppercase text-zinc-400">Density</Label>
                        <span className="text-xs font-mono">{floraDensity.toLocaleString()}</span>
                      </div>
                      <Slider 
                        value={[floraDensity]} 
                        min={100} 
                        max={10000} 
                        step={100} 
                        onValueChange={(v: number[]) => setFloraDensity(v[0] || 2000)}
                      />
                    </div>
                    <Button 
                      onClick={scatterFlora} 
                      className="w-full bg-emerald-600 hover:bg-emerald-500 text-zinc-100 text-xs uppercase font-bold tracking-widest"
                      disabled={isGenerating}
                    >
                      Scatter Foliage
                    </Button>
                  </div>
                </TabsContent>

                <TabsContent value="render" className="space-y-6 pb-20">
                  <div className="p-4 bg-zinc-900 rounded-lg border border-zinc-800 space-y-4">
                    <div className="flex items-center gap-3 mb-2">
                      <Waves className="w-4 h-4 text-blue-400" />
                      <h3 className="text-xs font-bold uppercase tracking-widest">Hydrology</h3>
                    </div>
                    <div className="space-y-4">
                      <div className="flex items-center justify-between">
                        <Label className="text-[10px] uppercase text-zinc-400">Sea Level</Label>
                        <span className="text-xs font-mono">{waterLevel.toFixed(1)}m</span>
                      </div>
                      <Slider value={[waterLevel]} min={-40} max={100} step={0.5} onValueChange={(v) => setWaterLevel(v[0])} />
                    </div>
                  </div>

                  <div className="p-4 bg-zinc-900 rounded-lg border border-zinc-800 space-y-4">
                    <div className="flex items-center gap-3 mb-2">
                       <Cloud className="w-4 h-4 text-sky-400" />
                       <h3 className="text-xs font-bold uppercase tracking-widest">Atmosphere</h3>
                    </div>
                    
                    <div className="space-y-4">
                      <div className="flex items-center justify-between">
                        <Label className="text-[10px] uppercase text-zinc-400">Sun Elevation</Label>
                        <span className="text-xs font-mono">{sunElevation}°</span>
                      </div>
                      <Slider value={[sunElevation]} min={-10} max={90} step={1} onValueChange={(v) => setSunElevation(v[0])} />
                    </div>

                    <div className="space-y-4">
                      <div className="flex items-center justify-between">
                        <Label className="text-[10px] uppercase text-zinc-400">Sun Azimuth</Label>
                        <span className="text-xs font-mono">{sunAzimuth}°</span>
                      </div>
                      <Slider value={[sunAzimuth]} min={0} max={360} step={1} onValueChange={(v) => setSunAzimuth(v[0])} />
                    </div>

                    <div className="space-y-4">
                      <div className="flex items-center justify-between">
                        <Label className="text-[10px] uppercase text-zinc-400">Sun Intensity</Label>
                        <span className="text-xs font-mono">{sunIntensity.toFixed(1)}x</span>
                      </div>
                      <Slider value={[sunIntensity]} min={0} max={5} step={0.1} onValueChange={(v) => setSunIntensity(v[0])} />
                    </div>

                    <div className="space-y-4">
                      <div className="flex items-center justify-between">
                        <Label className="text-[10px] uppercase text-zinc-400">Fog Density</Label>
                        <span className="text-xs font-mono">{fogDensity.toFixed(4)}</span>
                      </div>
                      <Slider value={[fogDensity]} min={0} max={0.01} step={0.0001} onValueChange={(v) => setFogDensity(v[0])} />
                    </div>
                  </div>

                  <div className="flex items-center justify-between p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex flex-col gap-1">
                      <Label className="text-xs font-bold uppercase tracking-widest">2D Top-Down Map</Label>
                      <span className="text-[10px] text-zinc-500">Hypsometric overhead view</span>
                    </div>
                    <Switch checked={showMinimap} onCheckedChange={setShowMinimap} />
                  </div>

                  <div className="flex items-center justify-between p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex flex-col gap-1">
                      <Label className="text-xs font-bold uppercase tracking-widest">Wireframe</Label>
                      <span className="text-[10px] text-zinc-500">View underlying geometry</span>
                    </div>
                    <Switch onCheckedChange={(v) => {
                      if (meshRef.current) {
                        (meshRef.current.material as THREE.ShaderMaterial).wireframe = v;
                      }
                    }} />
                  </div>

                  <div className="flex items-center justify-between p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex flex-col gap-1">
                      <Label className="text-xs font-bold uppercase tracking-widest">Grid Helper</Label>
                      <span className="text-[10px] text-zinc-500">Show spatial reference</span>
                    </div>
                    <Switch defaultChecked onCheckedChange={(v) => {
                      if (gridRef.current) gridRef.current.visible = v;
                    }} />
                  </div>

                  <div className="flex items-center justify-between p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex flex-col gap-1">
                      <Label className="text-xs font-bold uppercase tracking-widest">Terrain Inspector</Label>
                      <span className="text-[10px] text-zinc-500">Enable data lookup probe</span>
                    </div>
                    <Switch checked={isInspectMode} onCheckedChange={(v) => {
                      setIsInspectMode(v);
                      if (v) { setIsCarveMode(false); setIsVolcanoMode(false); }
                    }} />
                  </div>

                  <div className="p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex items-center justify-between mb-2">
                      <div className="flex flex-col gap-1">
                        <Label className="text-xs font-bold uppercase tracking-widest">Manual Carver</Label>
                        <span className="text-[10px] text-zinc-500">Draw to carve bedrock</span>
                      </div>
                      <Switch checked={isCarveMode} onCheckedChange={(v) => {
                        setIsCarveMode(v);
                        if (v) { setIsInspectMode(false); setIsVolcanoMode(false); }
                      }} />
                    </div>
                    {isCarveMode && (
                      <div className="space-y-4 pt-2">
                        <div className="space-y-2">
                          <div className="flex justify-between">
                            <Label className="text-[10px] text-zinc-500 uppercase">Brush Size</Label>
                            <span className="text-[10px] font-mono">{carveRadius}</span>
                          </div>
                          <Slider value={[carveRadius]} min={1} max={20} step={1} onValueChange={(v) => setCarveRadius(v[0])} />
                        </div>
                        <div className="space-y-2">
                          <div className="flex justify-between">
                            <Label className="text-[10px] text-zinc-500 uppercase">Depth</Label>
                            <span className="text-[10px] font-mono">{carveDepth}</span>
                          </div>
                          <Slider value={[carveDepth]} min={0.1} max={10} step={0.1} onValueChange={(v) => setCarveDepth(v[0])} />
                        </div>
                      </div>
                    )}
                  </div>
                </TabsContent>

                <TabsContent value="export" className="space-y-6">
                  <div className="p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex items-center gap-3 mb-3">
                      <Download className="w-5 h-5 text-emerald-400" />
                      <h3 className="text-xs font-bold uppercase tracking-widest">Unreal Engine Export</h3>
                    </div>
                    <p className="text-[10px] text-zinc-500 mb-4 leading-relaxed">
                      Export high-bitrate heightmaps and splatmaps for direct import into Unreal Engine 5.
                    </p>

                    {heightRange && (
                      <div className="mb-6 rounded-md border border-zinc-800 bg-zinc-900/60 p-3">
                        <div className="text-[9px] uppercase tracking-widest text-zinc-500 mb-1">
                          Height range (actual)
                        </div>
                        <div className="font-mono text-[11px] text-zinc-200">
                          {heightRange.min.toFixed(2)} &rarr; {heightRange.max.toFixed(2)}
                          <span className="text-zinc-500">
                            {'  span '}{(heightRange.max - heightRange.min).toFixed(2)}
                          </span>
                        </div>
                        <div className="text-[9px] text-zinc-500 mt-2 leading-relaxed">
                          .r16 and .png16 are normalized to exactly this span, not to the
                          Height slider ({params.heightMultiplier}) &mdash; erosion and
                          deposition move both ends. Unreal Z scale ={' '}
                          <span className="font-mono text-zinc-300">
                            {(((heightRange.max - heightRange.min) * 100) / 512).toFixed(3)}
                          </span>{' '}
                          at 1 unit = 1 m.
                        </div>
                      </div>
                    )}
                    
                    <div className="grid grid-cols-2 gap-3">
                      {([
                        { kind: 'r16', ext: 'r16', label: '.r16', sub: '16-bit RAW · Unreal' },
                        { kind: 'png16', ext: 'png', label: '.png 16', sub: '16-bit PNG · Unity' },
                        { kind: 'exr', ext: 'exr', label: '.exr', sub: 'Float32 · Blender/Nuke' },
                        { kind: 'r32', ext: 'r32', label: '.r32', sub: 'Float32 RAW · absolute' },
                        { kind: 'obj', ext: 'obj', label: '.obj', sub: 'Mesh · Blender' },
                        { kind: 'normal', ext: 'png', label: '.png N', sub: 'Normal map · RGB8' },
                        { kind: 'ao', ext: 'png', label: '.png AO', sub: 'Ambient occlusion' },
                        { kind: 'splat', ext: 'png', label: '.png Splat', sub: 'RGBA masks · matches viewport' },
                      ] as const).map(f => (
                        <Button
                          key={f.kind}
                          onClick={() => exportVia(f.kind, f.ext)}
                          variant="outline"
                          className="w-full border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-12 text-center flex flex-col items-center justify-center"
                        >
                          <div className="flex items-center mb-1">
                            <FileCode className="w-3 h-3 mr-1" />
                            <span>{f.label}</span>
                          </div>
                          <span className="text-[8px] opacity-60">{f.sub}</span>
                        </Button>
                      ))}
                    </div>
                  </div>

                  <div className="p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex items-center gap-3 mb-3">
                      <Save className="w-5 h-5 text-sky-400" />
                      <h3 className="text-xs font-bold uppercase tracking-widest">Project</h3>
                    </div>
                    <p className="text-[10px] text-zinc-500 mb-4 leading-relaxed">
                      A .titan file stores the seed, base parameters, and full layer stack — a perfect, tiny reproduction recipe.
                    </p>
                    <div className="grid grid-cols-2 gap-3">
                      <Button
                        onClick={saveProject}
                        variant="outline"
                        className="border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-10"
                      >
                        <Save className="w-3 h-3 mr-2" />
                        Save .titan
                      </Button>
                      <Button
                        onClick={() => loadFileRef.current?.click()}
                        variant="outline"
                        className="border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-10"
                      >
                        <FolderOpen className="w-3 h-3 mr-2" />
                        Load .titan
                      </Button>
                    </div>
                    <input
                      ref={loadFileRef}
                      type="file"
                      accept=".titan,application/json"
                      className="hidden"
                      onChange={(e) => {
                        const file = e.target.files?.[0];
                        if (file) loadProject(file);
                        e.target.value = '';
                      }}
                    />
                  </div>

                  <div className="p-4 bg-emerald-500/5 rounded-lg border border-emerald-500/20">
                    <h4 className="text-[10px] font-bold text-emerald-400 uppercase tracking-widest mb-2">Import Guide</h4>
                    <ul className="text-[9px] text-zinc-400 space-y-1 list-disc pl-4">
                      <li>Unreal: Landscape mode → "Import from File" → .r16 (16-bit LE RAW).</li>
                      <li>Unity: Terrain → Import Raw, or the 16-bit PNG.</li>
                      <li>Blender: .exr as displacement, or import the .obj mesh directly.</li>
                      <li>Splatmap: R=Rock, G=Height, B=Flow/Wetness.</li>
                    </ul>
                  </div>
                </TabsContent>
              </Tabs>
            </div>

            <div className="p-6 border-t border-zinc-800 bg-zinc-950">
              <div className="flex items-center justify-between mb-4">
                <div className="flex items-center gap-2 text-zinc-500">
                  <Cpu className="w-3 h-3" />
                  <span className="text-[9px] uppercase tracking-[0.2em] font-mono">System Status</span>
                </div>
                <div className="px-1.5 py-0.5 bg-emerald-500/10 border border-emerald-500/50 rounded flex items-center gap-1">
                  <div className="w-1 h-1 rounded-full bg-emerald-500" />
                  <span className="text-[8px] font-bold text-emerald-400 uppercase tracking-tighter">Daylight Active</span>
                </div>
              </div>
              <div className="flex items-center gap-2">
                <div className={`w-2 h-2 rounded-full ${
                  engineStatus === 'ready' ? 'bg-emerald-500 animate-pulse'
                  : engineStatus === 'error' ? 'bg-red-500'
                  : 'bg-yellow-500 animate-pulse'
                }`} />
                <span className="text-[10px] font-mono text-zinc-400">
                  {engineStatus === 'ready' && `${engineVersion} — C++ core (WASM)`}
                  {engineStatus === 'loading' && 'Loading C++ engine…'}
                  {engineStatus === 'error' && 'Engine failed to load'}
                </span>
              </div>
            </div>
          </div>
        )}

      {/* Pipeline progress */}
      {progress && (
        <div className="absolute bottom-6 left-1/2 -translate-x-1/2 z-30 w-[420px] max-w-[80vw]">
          <Card className="bg-zinc-950/90 backdrop-blur-md border-zinc-800 p-3">
            <div className="flex items-center gap-3">
              <Activity className="w-3.5 h-3.5 text-emerald-400 animate-pulse" />
              <div className="flex-1">
                <div className="flex justify-between mb-1.5">
                  <span className="text-[9px] uppercase tracking-widest text-zinc-400">{progress.label}</span>
                  <span className="text-[9px] font-mono text-zinc-500">{Math.round(progress.frac * 100)}%</span>
                </div>
                <div className="h-1 bg-zinc-800 rounded overflow-hidden">
                  <div className="h-full bg-emerald-500 transition-all duration-150" style={{ width: `${progress.frac * 100}%` }} />
                </div>
              </div>
              <Button variant="ghost" size="sm" className="h-6 w-6 p-0 text-zinc-500 hover:text-red-400" onClick={cancelRun}>
                <X className="w-3 h-3" />
              </Button>
            </div>
          </Card>
        </div>
      )}

      {/* 2D top-down map */}
      {showMinimap && (
        <div className="absolute top-24 right-6 z-20 pointer-events-auto">
          <Card className="bg-zinc-950/85 backdrop-blur-md border-zinc-800 p-2">
            <div className="flex items-center justify-between mb-1.5 px-0.5">
              <div className="flex items-center gap-1.5">
                <MapIcon className="w-3 h-3 text-emerald-400" />
                <span className="text-[9px] font-bold uppercase tracking-widest text-zinc-400">Top-Down</span>
              </div>
              <span className="text-[8px] font-mono text-zinc-600">{params.size}×{params.size}</span>
            </div>
            <canvas
              ref={(el) => {
                minimapRef.current = el;
                if (el) updateMinimap();
              }}
              className="w-[220px] h-[220px] rounded-sm border border-zinc-800"
              style={{ imageRendering: 'pixelated' }}
            />
          </Card>
        </div>
      )}

      {/* Footer / Overlay */}
      <div className="absolute bottom-6 right-6 z-20 flex flex-col items-end gap-2 pointer-events-none">
        {probeData && (
          <Card className="bg-zinc-950/80 backdrop-blur-md border border-emerald-500/30 p-4 mb-4 min-w-[200px] pointer-events-none">
            <div className="flex items-center gap-2 mb-3 border-b border-zinc-800 pb-2">
              <Activity className="w-3 h-3 text-emerald-400" />
              <span className="text-[10px] font-bold uppercase tracking-widest text-emerald-400">Terrain Probe</span>
            </div>
            <div className="grid grid-cols-2 gap-x-4 gap-y-2">
              <div className="flex flex-col">
                <span className="text-[8px] text-zinc-500 uppercase">Coordinates</span>
                <span className="text-xs font-mono">{probeData.x}, {probeData.y}</span>
              </div>
              <div className="flex flex-col">
                <span className="text-[8px] text-zinc-500 uppercase">Height</span>
                <span className="text-xs font-mono">{probeData.h.toFixed(2)}m</span>
              </div>
              <div className="flex flex-col">
                <span className="text-[8px] text-zinc-500 uppercase">Sediment</span>
                <span className="text-xs font-mono text-orange-400">{(probeData.s * 10).toFixed(2)}cm</span>
              </div>
              <div className="flex flex-col">
                <span className="text-[8px] text-zinc-500 uppercase">Flow Rate</span>
                <span className="text-xs font-mono text-blue-400">{(probeData.f * 100).toFixed(1)}%</span>
              </div>
              <div className="flex flex-col col-span-2">
                <span className="text-[8px] text-zinc-500 uppercase">Slope Gradient</span>
                <span className="text-xs font-mono text-zinc-300">{(probeData.slope * 57.29).toFixed(1)}°</span>
              </div>
            </div>
          </Card>
        )}
        <div className="flex items-center gap-2 pointer-events-auto">
          {isVolcanoMode && (
            <Card className="bg-orange-500/15 backdrop-blur-sm border-orange-500/50 px-3 py-1.5">
              <span className="text-[10px] font-mono text-orange-300 uppercase tracking-widest">
                Press terrain to drop a volcano · drag to place
              </span>
            </Card>
          )}
          <Card className="bg-zinc-950/50 backdrop-blur-sm border-zinc-800/50 px-3 py-1.5">
            <span className="text-[10px] font-mono text-zinc-500 uppercase tracking-widest">Orbit: Left Mouse</span>
          </Card>
          <Card className="bg-zinc-950/50 backdrop-blur-sm border-zinc-800/50 px-3 py-1.5">
            <span className="text-[10px] font-mono text-zinc-500 uppercase tracking-widest">Pan: Right Mouse</span>
          </Card>
        </div>
      </div>
    </div>
  );
}
