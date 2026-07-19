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
  X
} from 'lucide-react';

import { TitanCore } from './core/TitanCore';
import { TerrainParams } from './core/types';
import {
  Layer,
  LAYER_DEFS,
  LayerType,
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
import { ScrollArea } from '@/components/ui/scroll-area';

const randomSeed = () => Math.random().toString(36).substring(2, 9);

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
  const [waterLevel, setWaterLevel] = useState(-10);
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
  const engineRef = useRef<TitanCore | null>(null);
  const waterRef = useRef<THREE.Mesh | null>(null);
  const sunLightRef = useRef<THREE.DirectionalLight | null>(null);

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
    }
  }, [sunElevation, sunAzimuth, sunIntensity, fogDensity]);

  const updateMesh = React.useCallback(() => {
    if (!sceneRef.current || !engineRef.current) return;

    const meshData = engineRef.current.buildMesh();

    if (meshRef.current) {
      sceneRef.current.remove(meshRef.current);
      meshRef.current.geometry.dispose();
      (meshRef.current.material as THREE.Material).dispose();
    }

    const geometry = new THREE.BufferGeometry();
    geometry.setAttribute('position', new THREE.BufferAttribute(meshData.vertices, 3));
    geometry.setAttribute('normal', new THREE.BufferAttribute(meshData.normals, 3));
    geometry.setAttribute('color', new THREE.BufferAttribute(meshData.colors, 4));
    geometry.setAttribute('uv', new THREE.BufferAttribute(meshData.uvs, 2));
    geometry.setIndex(new THREE.BufferAttribute(meshData.indices, 1));

    const phi = (90 - sunElevation) * (Math.PI / 180);
    const theta = (sunAzimuth) * (Math.PI / 180);
    const sunPos = new THREE.Vector3().setFromSphericalCoords(300, phi, theta);

    // Custom Shader Material for Splat Mapping
    const material = new THREE.ShaderMaterial({
      vertexColors: true,
      side: THREE.DoubleSide,
      uniforms: {
        uTime: { value: 0 },
        uSunPos: { value: sunPos },
        uFogColor: { value: new THREE.Color(0x87CEEB) },
        uFogDensity: { value: fogDensity },
        uBiome: { value: ['arctic', 'temperate', 'volcanic', 'desert'].indexOf(params.biome) }
      },
      vertexShader: `
        varying vec3 vPosition;
        varying vec3 vNormal;
        varying vec4 vColor;
        varying vec3 vWorldPos;

        void main() {
          vPosition = position;
          vNormal = normal;
          vColor = color;
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

        uniform vec3 uSunPos;
        uniform vec3 uFogColor;
        uniform float uFogDensity;
        uniform int uBiome;

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

        void main() {
          vec3 lightDir = normalize(uSunPos);
          vec3 viewDir = normalize(cameraPosition - vWorldPos);
          vec3 halfDir = normalize(lightDir + viewDir);
          
          float diff = max(dot(vNormal, lightDir), 0.0);
          
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
          
          // Detail Texturing via Triplanar-like noise
          float detail = noise(vWorldPos.xz * 2.0) * 0.4 + noise(vWorldPos.xz * 8.0) * 0.1;
          grassColor *= (0.8 + detail * 0.4);
          rockColor *= (0.7 + detail * 0.6);
          sedimentColor *= (0.8 + detail * 0.4);
          
          // Base mix: Grass and Rock
          vec3 baseColor = mix(grassColor, rockColor, smoothstep(0.4, 0.8, rockMask));
          
          // Visual Strata logic (Horizontal banding on rock)
          float strataDetail = sin(vWorldPos.y * 3.0) + sin(vWorldPos.y * 7.2) * 0.4;
          vec3 strataColor = vec3(0.35, 0.32, 0.28); 
          baseColor = mix(baseColor, strataColor, smoothstep(0.2, 1.0, strataDetail) * rockMask * 0.6);
          
          // Apply sediment/soil layer
          baseColor = mix(baseColor, sedimentColor, smoothstep(0.01, 0.4, sedimentMask));
          
          // Apply wetness/flow mapping
          float wetness = smoothstep(0.1, 0.9, flowMask);
          baseColor = mix(baseColor, waterColor, wetness * 0.5);
          
          // Apply snow at high altitudes
          baseColor = mix(baseColor, snowColor, smoothstep(0.78, 0.98, height));
          
          // Lighting
          vec3 finalColor = baseColor * (diff * 0.8 + 0.2);
          
          // Specular for wet/ice areas
          float spec = pow(max(dot(vNormal, halfDir), 0.0), 32.0);
          float specMask = max(smoothstep(0.7, 1.0, height), wetness * 0.6);
          finalColor += vec3(1.0) * spec * specMask * 0.3;
          
          // Fog
          float dist = length(vPosition.xz);
          float fog = 1.0 - exp(-dist * uFogDensity * 2.0);
          finalColor = mix(finalColor, uFogColor, fog);

          gl_FragColor = vec4(finalColor, 1.0);
        }
      `,
    });

    const mesh = new THREE.Mesh(geometry, material);
    sceneRef.current.add(mesh);
    meshRef.current = mesh;

    updateAtmosphere();

    setStats(prev => ({
      ...prev,
      vertices: meshData.vertices.length / 3,
      triangles: meshData.indices.length / 3
    }));
  }, [params.biome, sunElevation, sunAzimuth, sunIntensity, fogDensity, updateAtmosphere]);

  useEffect(() => {
    if (!canvasRef.current) return;

    // 1. Core Scene Setup (Infrastructure) - Only runs once
    const scene = new THREE.Scene();
    const skyColor = new THREE.Color(0x87CEEB);
    scene.background = skyColor;
    scene.fog = new THREE.FogExp2(skyColor, 0.002);
    sceneRef.current = scene;

    const camera = new THREE.PerspectiveCamera(60, window.innerWidth / window.innerHeight, 0.1, 2000);
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

    // Water Plane
    const waterGeom = new THREE.PlaneGeometry(1000, 1000);
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
          pos.z += sin(pos.x * 0.1 + uTime) * 0.2;
          pos.z += cos(pos.y * 0.12 + uTime * 1.2) * 0.15;
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

        void main() {
          vec3 viewDir = normalize(cameraPosition - vWorldPos);
          vec3 lightDir = normalize(uSunPos);
          
          float fresnel = pow(1.0 - max(dot(viewDir, vec3(0.0, 1.0, 0.0)), 0.0), 3.0);
          vec3 color = mix(uColor, vec3(0.4, 0.7, 1.0), fresnel * 0.5);
          
          // Specular highlights
          vec3 halfDir = normalize(lightDir + viewDir);
          float spec = pow(max(dot(vec3(0.0, 1.0, 0.0), halfDir), 0.0), 128.0);
          color += spec * 0.5;

          gl_FragColor = vec4(color, 0.8);
        }
      `
    });
    const water = new THREE.Mesh(waterGeom, waterMat);
    water.rotation.x = -Math.PI / 2;
    water.position.y = waterLevel;
    scene.add(water);
    waterRef.current = water;

    const grid = new THREE.GridHelper(200, 20, 0x333333, 0x222222);
    grid.position.y = -0.1;
    scene.add(grid);

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

  useEffect(() => {
    // 2. Interaction Layer (Raycasting/Events) - Re-bind on mode changes
    if (!rendererRef.current || !cameraRef.current || !sceneRef.current) return;

    const raycaster = new THREE.Raycaster();
    const mouse = new THREE.Vector2();

    const handlePointerMove = (event: PointerEvent) => {
      const camera = cameraRef.current;
      const mesh = meshRef.current;
      const engine = engineRef.current;

      if ((!isInspectMode && !isCarveMode) || !camera || !mesh || !engine) {
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
      }
    };

    const handlePointerUp = () => {
      isMouseDown.current = false;
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
  }, [isInspectMode, isCarveMode, carveRadius, carveDepth, params.size, updateMesh]);

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
      const project: TitanProject = { version: 1, params, stack };
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
  }, [params, stack, updateMesh]);

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

  // --- Undo/redo over (params, stack) snapshots ---------------------------
  useEffect(() => {
    const snapshot = serializeProject({ version: 1, params, stack });
    const h = historyRef.current;
    if (applyingHistoryRef.current) {
      applyingHistoryRef.current = false;
    } else if (h.past[h.past.length - 1] !== snapshot) {
      h.past.push(snapshot);
      if (h.past.length > 50) h.past.shift();
      h.future = [];
    }
    setHistoryState({ canUndo: h.past.length > 1, canRedo: h.future.length > 0 });
  }, [params, stack]);

  const applySnapshot = React.useCallback((json: string) => {
    const project = deserializeProject(json);
    applyingHistoryRef.current = true;
    setParams(project.params);
    setStack(project.stack);
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

  // --- Project save/load ---------------------------------------------------
  const saveProject = () => {
    const json = serializeProject({ version: 1, params, stack });
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

  const exportVia = (kind: 'png16' | 'r16' | 'r32' | 'exr' | 'obj', ext: string) => {
    if (!engineRef.current) return;
    downloadBinary(
      engineRef.current.exportFile(kind),
      `titan_${params.seed}_${params.size}x${params.size}.${ext}`
    );
  };


  const exportHeightmapPNG = () => {
    if (!engineRef.current) return;
    const size = params.size;
    const canvas = document.createElement('canvas');
    canvas.width = size;
    canvas.height = size;
    const ctx = canvas.getContext('2d')!;
    const imageData = ctx.createImageData(size, size);
    
    const bedrock = engineRef.current.getBedrockMap();
    const sediment = engineRef.current.getSedimentMap();
    
    let minH = Infinity;
    let maxH = -Infinity;
    for (let i = 0; i < size * size; i++) {
      const h = bedrock[i] + sediment[i];
      if (h < minH) minH = h;
      if (h > maxH) maxH = h;
    }
    const range = maxH - minH || 1;

    for (let i = 0; i < size * size; i++) {
      const h = bedrock[i] + sediment[i];
      const val = Math.floor(((h - minH) / range) * 255);
      imageData.data[i * 4 + 0] = val;
      imageData.data[i * 4 + 1] = val;
      imageData.data[i * 4 + 2] = val;
      imageData.data[i * 4 + 3] = 255;
    }
    
    ctx.putImageData(imageData, 0, 0);
    const url = canvas.toDataURL('image/png');
    const a = document.createElement('a');
    a.href = url;
    a.download = `titan_heightmap_${size}x${size}.png`;
    a.click();
  };

  const exportSplatmap = () => {
    if (!engineRef.current) return;
    const size = params.size;
    const canvas = document.createElement('canvas');
    canvas.width = size;
    canvas.height = size;
    const ctx = canvas.getContext('2d');
    if (!ctx) return;
    
    const imageData = ctx.createImageData(size, size);
    const bedrock = engineRef.current.getBedrockMap();
    const sediment = engineRef.current.getSedimentMap();
    const flowMap = engineRef.current.getFlowMap();

    for (let y = 0; y < size; y++) {
      for (let x = 0; x < size; x++) {
        const i = y * size + x;
        const s = sediment[i];

        // R: Rock mask, G: Height mask, B: Flow mask (Wetness)
        const h = bedrock[i] + s;
        const hNorm = Math.min(255, (h / params.heightMultiplier) * 255);
        const fNorm = Math.min(255, (flowMap[i] || 0) * 255);
        const slope = engineRef.current.calculateSlope(x, y);
        const rNorm = Math.min(255, slope * 2.5 * 255);
        
        imageData.data[i * 4 + 0] = rNorm; // Rock mask
        imageData.data[i * 4 + 1] = hNorm; // Height mask
        imageData.data[i * 4 + 2] = fNorm; // Flow mask
        imageData.data[i * 4 + 3] = 255;
      }
    }
    
    ctx.putImageData(imageData, 0, 0);
    const url = canvas.toDataURL('image/png');
    const a = document.createElement('a');
    a.href = url;
    a.download = `titan_splatmap_${size}x${size}.png`;
    a.click();
  };

  const updateParam = (key: keyof TerrainParams, value: any) => {
    console.log(`Updating param ${key} to ${value}`);
    if (key !== 'seed' && (typeof value !== 'number' || isNaN(value))) return;
    setParams(p => ({ ...p, [key]: value }));
  };

  useEffect(() => {
    console.log('Params updated:', params);
  }, [params]);

  useEffect(() => {
    updateAtmosphere();
    if (waterRef.current) {
      waterRef.current.position.y = waterLevel;
    }
  }, [waterLevel, sunElevation, sunAzimuth, sunIntensity, fogDensity, updateAtmosphere]);

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
          <ScrollArea className="flex-1 px-6 pb-8">
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
                         { id: 'billow', icon: Cloud, label: 'Billow' }
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
                      max={512} 
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
                      max={200} 
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
                          {def.params.map(pd => (
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
                      const grid = sceneRef.current?.children.find(c => c instanceof THREE.GridHelper);
                      if (grid) grid.visible = v;
                    }} />
                  </div>

                  <div className="flex items-center justify-between p-4 bg-zinc-900 rounded-lg border border-zinc-800">
                    <div className="flex flex-col gap-1">
                      <Label className="text-xs font-bold uppercase tracking-widest">Terrain Inspector</Label>
                      <span className="text-[10px] text-zinc-500">Enable data lookup probe</span>
                    </div>
                    <Switch checked={isInspectMode} onCheckedChange={(v) => {
                      setIsInspectMode(v);
                      if (v) setIsCarveMode(false);
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
                        if (v) setIsInspectMode(false);
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
                    <p className="text-[10px] text-zinc-500 mb-6 leading-relaxed">
                      Export high-bitrate heightmaps and splatmaps for direct import into Unreal Engine 5.
                    </p>
                    
                    <div className="grid grid-cols-2 gap-3">
                      {([
                        { kind: 'r16', ext: 'r16', label: '.r16', sub: '16-bit RAW · Unreal' },
                        { kind: 'png16', ext: 'png', label: '.png 16', sub: '16-bit PNG · Unity' },
                        { kind: 'exr', ext: 'exr', label: '.exr', sub: 'Float32 · Blender/Nuke' },
                        { kind: 'r32', ext: 'r32', label: '.r32', sub: 'Float32 RAW · absolute' },
                        { kind: 'obj', ext: 'obj', label: '.obj', sub: 'Mesh · Blender' },
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
                      <Button
                        onClick={exportHeightmapPNG}
                        variant="outline"
                        className="w-full border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-12 text-center flex flex-col items-center justify-center"
                      >
                        <div className="flex items-center mb-1">
                          <ImageIcon className="w-3 h-3 mr-1" />
                          <span>.png 8</span>
                        </div>
                        <span className="text-[8px] opacity-60">8-bit Mask</span>
                      </Button>
                    </div>
                      
                    <Button 
                      onClick={exportSplatmap}
                      variant="outline" 
                      className="w-full border-zinc-800 hover:bg-zinc-800 text-[10px] uppercase tracking-widest h-12"
                    >
                      <ImageIcon className="w-4 h-4 mr-2" />
                      Export Splatmap (PNG)
                    </Button>
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
            </ScrollArea>

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
