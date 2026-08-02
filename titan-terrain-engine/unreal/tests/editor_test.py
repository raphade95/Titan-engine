# Full editor test for TitanBridge.
#
# Everything verified so far has been the plugin's *engine* contract, checked
# natively. This exercises the half nothing else touches: the actor really
# spawning in a world, GenerateTerrain's async round trip completing on the
# game thread, and CreateMeshSection accepting the buffers.
#
# It also checks the cell-size fix where it actually matters — the actor's
# world-space bounds — because that is the number a level designer sees.

import json
import os
import unreal

RESULT = os.environ.get("TITAN_TEST_RESULT", "/tmp/TitanEditorTest/result.json")
CM_PER_UNIT = 100.0
WORLD_SIZE = 256
MAX_TICKS = 1800          # generous; erosion at 256 takes a moment

log = unreal.log
results = {"checks": [], "done": False}


def check(ok, label, detail=""):
    results["checks"].append({"ok": bool(ok), "label": label, "detail": str(detail)})
    log("[titan-test] %s  %s %s" % ("PASS" if ok else "FAIL", label, detail))


# Start from an empty level, not whatever the editor opens by default.
#
# This cost a whole debugging session: the default startup map is an Open
# World template that already contains a Landscape actor, so the landscape
# assertions below were measuring the template's terrain and reporting
# failures that had nothing to do with the plugin. A test that inherits an
# unknown world is not testing what it thinks it is.
try:
    subsystem = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    made = subsystem.new_level("/Game/TitanEditorTestMap")
    check(bool(made), "started from a new empty level")
except Exception as exc:                                          # noqa: BLE001
    check(False, "started from a new empty level", exc)

pre_existing = None  # filled in once landscapes() is defined


def spawn(name, resolution):
    actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
        unreal.TitanTerrainActor, unreal.Vector(0.0, 0.0, 0.0))
    actor.set_actor_label(name)
    actor.set_editor_property("seed", "titan")
    actor.set_editor_property("resolution", resolution)
    actor.set_editor_property("world_size", WORLD_SIZE)
    actor.set_editor_property("units_per_world_unit", CM_PER_UNIT)
    # Keep the run short and deterministic: the mesh path is what is under
    # test, not how long erosion takes.
    actor.set_editor_property("hydraulic_erosion", False)
    actor.set_editor_property("river_networks", False)
    actor.set_editor_property("thermal_weathering", False)
    return actor


def landscapes():
    found = []
    for actor in unreal.EditorLevelLibrary.get_all_level_actors():
        if isinstance(actor, unreal.Landscape):
            found.append(actor)
    return found


pre_existing = len(landscapes())
check(pre_existing == 0, "the level starts with no Landscape actors",
      "found %d" % pre_existing)

full = spawn("TitanFull", 256)
half = spawn("TitanHalf", 128)
check(full is not None and half is not None, "both actors spawn in the editor world")

# A third actor set to Landscape output. It builds a separate ALandscape
# rather than a mesh section, so it is polled on its own terms below.
land = spawn("TitanLandscape", 256)
land.set_editor_property("output", unreal.TitanOutput.LANDSCAPE)

full.generate_terrain()
half.generate_terrain()
land.generate_terrain()
log("[titan-test] generation requested")


def spawned_landscape():
    """The landscape the plugin itself recorded — authoritative about whether
    ApplyLandscape ran at all, as opposed to what happens to be in the world."""
    try:
        return land.get_editor_property("spawned_landscape")
    except Exception:                                             # noqa: BLE001
        return None

state = {"ticks": 0, "handle": None}


def sections(actor):
    mesh = actor.get_editor_property("procedural_mesh")
    return mesh.get_num_sections() if mesh else 0


def finish():
    try:
        # 1. The async round trip completed and a section exists.
        check(sections(full) == 1, "GenerateTerrain produced one mesh section",
              "sections=%d after %d ticks" % (sections(full), state["ticks"]))

        # 2. Bounds. This is the cell-size fix, measured where a designer sees
        #    it: the actor must be WorldSize * cm-per-unit across.
        _, extent = full.get_actor_bounds(False)
        span_x = extent.x * 2.0
        span_y = extent.y * 2.0
        expected = WORLD_SIZE * CM_PER_UNIT
        check(abs(span_x - expected) < expected * 0.05,
              "actor spans WorldSize * UnitsPerWorldUnit in X",
              "%.0f cm vs %.0f expected" % (span_x, expected))
        check(abs(span_y - expected) < expected * 0.05,
              "actor spans WorldSize * UnitsPerWorldUnit in Y",
              "%.0f cm vs %.0f expected" % (span_y, expected))

        # 3. Resolution independence, end to end in the editor. Under the old
        #    hardcoded cell size the half-resolution actor was half the size.
        _, half_extent = half.get_actor_bounds(False)
        half_span = half_extent.x * 2.0
        check(abs(half_span - span_x) < span_x * 0.05,
              "half-resolution actor covers the same ground",
              "%.0f cm vs %.0f cm" % (half_span, span_x))

        # 4. The terrain has actual relief rather than being a flat plane.
        check(extent.z > 1.0, "terrain has vertical relief",
              "half-height %.0f cm" % extent.z)

        # 5. Landscape output built a real ALandscape, not a mesh.
        # Ask the plugin what it built, rather than inferring from the world.
        mine = spawned_landscape()
        check(mine is not None,
              "ApplyLandscape ran and recorded a Landscape actor")
        found = landscapes()
        check(len(found) == 1, "exactly one Landscape actor exists",
              "found %d: %s" % (len(found),
                                [a.get_actor_label() for a in found]))
        if mine is not None:
            ls = mine
            _, ls_extent = ls.get_actor_bounds(False)
            ls_span = ls_extent.x * 2.0
            check(abs(ls_span - expected) < expected * 0.05,
                  "the Landscape covers the same world as the mesh path",
                  "%.0f cm vs %.0f expected" % (ls_span, expected))
            check(ls_extent.z > 1.0, "the Landscape has vertical relief",
                  "half-height %.0f cm" % ls_extent.z)
            # A landscape with no LandscapeInfo looks correct and behaves like
            # a prop: no sculpting, no layer painting. Worth asserting.
            check(ls.get_landscape_actor() is not None,
                  "the Landscape resolves its own actor (LandscapeInfo exists)")
        # The mesh on the landscape actor must be cleared, or both outputs
        # occupy the same space.
        land_mesh = land.get_editor_property("procedural_mesh")
        check(land_mesh.get_num_sections() == 0,
              "the landscape actor's own mesh is cleared",
              "sections=%d" % land_mesh.get_num_sections())
    except Exception as exc:                                  # noqa: BLE001
        check(False, "test body raised", exc)

    results["done"] = True
    with open(RESULT, "w") as handle:
        json.dump(results, handle, indent=2)
    log("[titan-test] wrote %s" % RESULT)
    if state["handle"] is not None:
        unreal.unregister_slate_post_tick_callback(state["handle"])
    # A commandlet-less editor run will not exit on its own.
    os._exit(0)


def poll(delta_seconds):
    state["ticks"] += 1
    if (sections(full) >= 1 and sections(half) >= 1
            and spawned_landscape() is not None):
        finish()
    elif state["ticks"] > MAX_TICKS:
        check(False, "generation completed within the tick budget",
              "gave up after %d ticks" % state["ticks"])
        finish()


state["handle"] = unreal.register_slate_post_tick_callback(poll)
log("[titan-test] polling for async completion")
