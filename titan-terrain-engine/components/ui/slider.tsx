import { Slider as SliderPrimitive } from "@base-ui/react/slider"

import { cn } from "@/lib/utils"

function Slider({
  className,
  defaultValue,
  value,
  min = 0,
  max = 100,
  onValueChange,
  ...props
}: SliderPrimitive.Root.Props) {
  const _values = Array.isArray(value)
    ? value
    : Array.isArray(defaultValue)
      ? defaultValue
      : [min]

  return (
    <SliderPrimitive.Root
      className={cn("relative flex w-full h-10 items-center select-none data-disabled:opacity-50 z-30", className)}
      data-slot="slider"
      value={value}
      min={min}
      max={max}
      onValueChange={(v, event) => {
        const val = Array.isArray(v) ? v : [v];
        console.log('Slider onValueChange:', val);
        onValueChange?.(val, event);
      }}
      {...props}
    >
      <SliderPrimitive.Control className="relative flex w-full items-center h-full">
        <SliderPrimitive.Track
          data-slot="slider-track"
          className="relative grow overflow-hidden rounded-full bg-zinc-800 h-1 w-full cursor-pointer"
        >
          <SliderPrimitive.Indicator
            data-slot="slider-range"
            className="absolute bg-zinc-100 h-full"
          />
        </SliderPrimitive.Track>
        {Array.from({ length: _values.length }, (_, index) => (
          <SliderPrimitive.Thumb
            data-slot="slider-thumb"
            key={index}
            className="relative block size-5 shrink-0 rounded-full border-2 border-zinc-100 bg-zinc-950 ring-offset-background transition-colors focus-visible:outline-none focus-visible:ring-2 focus-visible:ring-ring focus-visible:ring-offset-2 disabled:pointer-events-none disabled:opacity-50 cursor-grab active:cursor-grabbing z-40"
          />
        ))}
      </SliderPrimitive.Control>
    </SliderPrimitive.Root>
  )
}

export { Slider }
