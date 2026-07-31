// Draggable height-remap curve editor.
//
// The engine has always accepted an arbitrary number of monotone-cubic control
// points (titan_apply_curve); the UI offered five fixed sliders, so most of
// that capability was unreachable.
//
// The rendered curve comes from the engine's own sampler rather than a spline
// reimplemented here. That matters: this codebase has already paid once for
// duplicating a formula across C++, TypeScript and Swift, and a curve editor
// whose preview disagrees with the result is worse than no editor.

import React, { useCallback, useMemo, useRef, useState } from 'react';
import { CurvePoint, MAX_CURVE_POINTS, sanitizeCurve } from '../core/pipeline';

interface Props {
  points: CurvePoint[];
  /** Samples the engine's spline; identity fallback if the engine is absent. */
  sample: (pts: CurvePoint[], samples: number) => Float32Array | null;
  onChange: (points: CurvePoint[]) => void;
}

const SAMPLES = 96;
// Pointer distance (in curve units, 0..1) that counts as grabbing a point.
const GRAB_RADIUS = 0.055;

export function CurveEditor({ points, sample, onChange }: Props) {
  const svgRef = useRef<SVGSVGElement | null>(null);
  const [dragging, setDragging] = useState<number | null>(null);

  const curvePath = useMemo(() => {
    const ys = sample(points, SAMPLES);
    const segs: string[] = [];
    for (let i = 0; i < SAMPLES; i++) {
      const x = i / (SAMPLES - 1);
      // Fall back to straight segments between control points if the engine is
      // not up yet, rather than drawing a curve that is not the real one.
      const y = ys ? ys[i] : interpolateLinear(points, x);
      segs.push(`${i === 0 ? 'M' : 'L'} ${(x * 100).toFixed(2)} ${((1 - y) * 100).toFixed(2)}`);
    }
    return segs.join(' ');
  }, [points, sample]);

  const toCurveSpace = useCallback((e: React.PointerEvent) => {
    const svg = svgRef.current;
    if (!svg) return { x: 0, y: 0 };
    const r = svg.getBoundingClientRect();
    return {
      x: Math.min(1, Math.max(0, (e.clientX - r.left) / r.width)),
      y: Math.min(1, Math.max(0, 1 - (e.clientY - r.top) / r.height)),
    };
  }, []);

  const nearestPoint = useCallback((pos: { x: number; y: number }) => {
    let best = -1;
    let bestDist = GRAB_RADIUS;
    points.forEach((p, i) => {
      const d = Math.hypot(p.x - pos.x, p.y - pos.y);
      if (d < bestDist) { bestDist = d; best = i; }
    });
    return best;
  }, [points]);

  const handleDown = (e: React.PointerEvent) => {
    const pos = toCurveSpace(e);
    const hit = nearestPoint(pos);
    if (hit >= 0) {
      setDragging(hit);
      (e.target as Element).setPointerCapture?.(e.pointerId);
      return;
    }
    if (points.length >= MAX_CURVE_POINTS) return;
    // Clicking empty space adds a point there and immediately drags it.
    const next = sanitizeCurve([...points, { x: pos.x, y: pos.y }]);
    if (!next) return;
    onChange(next);
    setDragging(next.findIndex(p => Math.abs(p.x - pos.x) < 1e-6));
    (e.target as Element).setPointerCapture?.(e.pointerId);
  };

  const handleMove = (e: React.PointerEvent) => {
    if (dragging === null) return;
    const pos = toCurveSpace(e);
    const next = points.map(p => ({ ...p }));
    // The endpoints stay pinned to x = 0 and x = 1: the remap has to cover the
    // whole height range, and the engine's spline clamps outside it anyway.
    const isEnd = dragging === 0 || dragging === points.length - 1;
    next[dragging] = { x: isEnd ? next[dragging].x : pos.x, y: pos.y };
    const cleaned = sanitizeCurve(next);
    if (cleaned) onChange(cleaned);
  };

  const handleUp = () => setDragging(null);

  const removePoint = (i: number) => {
    // Two points are the minimum the engine will evaluate, and the endpoints
    // anchor the range.
    if (points.length <= 2 || i === 0 || i === points.length - 1) return;
    const next = points.filter((_, k) => k !== i);
    onChange(next);
  };

  return (
    <div className="space-y-2">
      <svg
        ref={svgRef}
        viewBox="0 0 100 100"
        preserveAspectRatio="none"
        className="w-full h-40 bg-zinc-950 border border-zinc-800 rounded touch-none cursor-crosshair"
        onPointerDown={handleDown}
        onPointerMove={handleMove}
        onPointerUp={handleUp}
        onPointerLeave={handleUp}
      >
        {[25, 50, 75].map(v => (
          <g key={v} stroke="#27272a" strokeWidth="0.4">
            <line x1={v} y1="0" x2={v} y2="100" />
            <line x1="0" y1={v} x2="100" y2={v} />
          </g>
        ))}
        {/* Identity reference, so departures from "no change" are readable. */}
        <line x1="0" y1="100" x2="100" y2="0" stroke="#3f3f46"
              strokeWidth="0.5" strokeDasharray="2 2" />
        <path d={curvePath} fill="none" stroke="#34d399" strokeWidth="1.4"
              vectorEffect="non-scaling-stroke" />
        {points.map((p, i) => {
          const pinned = i === 0 || i === points.length - 1;
          return (
            <circle
              key={i}
              cx={p.x * 100}
              cy={(1 - p.y) * 100}
              r={dragging === i ? 3.2 : 2.4}
              fill={pinned ? '#a1a1aa' : '#34d399'}
              stroke="#09090b"
              strokeWidth="0.8"
              vectorEffect="non-scaling-stroke"
              style={{ cursor: pinned ? 'ns-resize' : 'move' }}
              onDoubleClick={(e) => { e.stopPropagation(); removePoint(i); }}
            />
          );
        })}
      </svg>

      <div className="flex items-center justify-between">
        <span className="text-[9px] text-zinc-600">
          {points.length} point{points.length === 1 ? '' : 's'}
          {points.length >= MAX_CURVE_POINTS ? ' (max)' : ''}
        </span>
        <div className="flex gap-1">
          {CURVE_PRESETS.map(preset => (
            <button
              key={preset.name}
              className="text-[9px] uppercase tracking-wider px-1.5 py-0.5 rounded
                         text-zinc-500 hover:text-emerald-400 hover:bg-zinc-800"
              onClick={() => onChange(preset.points.map(p => ({ ...p })))}
              title={preset.hint}
            >
              {preset.name}
            </button>
          ))}
        </div>
      </div>
    </div>
  );
}

/** Straight-line fallback used only before the engine is available. */
function interpolateLinear(points: CurvePoint[], x: number): number {
  if (points.length === 0) return x;
  if (x <= points[0].x) return points[0].y;
  const last = points[points.length - 1];
  if (x >= last.x) return last.y;
  for (let i = 0; i < points.length - 1; i++) {
    const a = points[i];
    const b = points[i + 1];
    if (x <= b.x) {
      const t = (x - a.x) / Math.max(1e-6, b.x - a.x);
      return a.y + (b.y - a.y) * t;
    }
  }
  return last.y;
}

const CURVE_PRESETS: { name: string; hint: string; points: CurvePoint[] }[] = [
  {
    name: 'Reset',
    hint: 'Identity — no change',
    points: [{ x: 0, y: 0 }, { x: 0.5, y: 0.5 }, { x: 1, y: 1 }],
  },
  {
    name: 'S',
    hint: 'Contrast: flattens lowlands, steepens midslopes, rounds peaks',
    points: [{ x: 0, y: 0 }, { x: 0.3, y: 0.16 }, { x: 0.5, y: 0.5 },
             { x: 0.7, y: 0.84 }, { x: 1, y: 1 }],
  },
  {
    name: 'Mesa',
    hint: 'Compresses the top into a plateau',
    points: [{ x: 0, y: 0 }, { x: 0.45, y: 0.5 }, { x: 0.7, y: 0.86 },
             { x: 0.85, y: 0.95 }, { x: 1, y: 1 }],
  },
  {
    name: 'Basin',
    hint: 'Deepens valleys, leaves peaks alone',
    points: [{ x: 0, y: 0 }, { x: 0.35, y: 0.12 }, { x: 0.7, y: 0.55 },
             { x: 1, y: 1 }],
  },
];
