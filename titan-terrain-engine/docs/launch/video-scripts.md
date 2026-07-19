# Tutorial Video Scripts

Three videos, 60–90 seconds each. Screen-capture only, voiceover per the
script, no talking head needed. Record at 1080p+, crop the browser chrome.

---

## Video 1 — "Zero to mountain in 60 seconds" (hero video)

**Open on the flat canvas.**

> "This is Titan. It opens empty — no demo terrain pretending to be your
> work. Let's make a mountain range."

**Click Alpine Peaks preset.** (Let the live erosion play out — the terrain
visibly carves itself. This moment is the whole pitch; don't cut it.)

> "One click. What you're watching is real simulation — rivers routed
> across the whole map, sixty-five thousand erosion droplets, rockfall
> settling to the angle of repose. Not a filter. Geology."

**Click Randomize twice.**

> "Every seed is a new landscape. Found one you love?"

**Click the seed lock.**

> "Lock it. Same seed, same terrain — bit-for-bit, forever. That's a
> guarantee, not a hope."

**Open Export tab, click .r16.**

> "Sixteen-bit heightmaps for Unreal, EXR for Blender, OBJ meshes, one
> click each. Titan runs free in your browser — link below. The native Mac
> app and the Unreal plugin are in the description."

---

## Video 2 — "The layer stack" (product depth)

**Start from Canyonlands preset, open Stack tab.**

> "Terrain tools make you choose: node graphs that need a course, or
> one-button generators you can't art-direct. Titan's answer is a layer
> stack — like Photoshop, for geology."

**Toggle the Terrace layer off/on.** (Terrain rebuilds each time.)

> "Every layer is a real simulation you can reorder, retune, or switch
> off. Terrace first, then rivers? Mesas. Rivers first, then terrace?
> Stepped canyons. The stack is the recipe."

**Drag River Networks strength up, release.**

> "Changes rebuild the whole stack deterministically — no baked state, no
> 'undo broke my terrain'."

**Save .titan, drag the file back in.**

> "The entire terrain is this two-kilobyte file. Seed, settings, stack.
> Email it, version it, load it in the Mac app or straight into Unreal."

---

## Video 3 — "Titan to Unreal in two minutes" (pipeline proof)

**Web lab: Alpine preset at 512, export .r16 + splatmap.**

> "Let's ship this terrain to Unreal — the manual way first."

**Unreal: Landscape mode → Import from File, set Z scale.**

> "Import the heightmap, one Z-scale number, done. The splatmap's channels
> — rock, height, wetness — drive your landscape material."

**Delete the landscape. Place Titan Terrain Actor instead.**

> "Or skip the files. The TitanBridge plugin generates in-editor, on a
> background thread, with the same engine. Same seed as the web lab, same
> mountain."

**Click Randomize Seed in the Details panel; terrain rebuilds in viewport.**

> "Iterate here, or iterate in the lab and paste the seed across. One
> engine, three doors in. Titan — the terrain engine your Mac never had."
