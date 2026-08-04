# Commandlet suite: the same checks, driven synchronously.
#
# This exists because UE 5.5 will not run the tick-based suites at all —
# spawning an actor from Python under -ExecCmds asserts inside the engine
# regardless of API, timing, or whether a level was created, while the same
# spawn from a pythonscript commandlet works fine. The commandlet, though,
# never ticks, so the game-thread task the async generation path ends with
# never runs and the terrain never arrives.
#
# GenerateTerrainNow closes that gap: it runs the pipeline on the calling
# thread and applies the result before returning, so a commandlet can verify
# the same things the editor suites do.
#
# Kept deliberately close to editor_test.py's assertions. Where the two
# disagree, the tick-based suite is the reference — it exercises the async
# path that ships.

import json
import os
import unreal

RESULT = os.environ.get("TITAN_SYNC_RESULT", "/tmp/TitanEditorTest/result_sync.json")
CM_PER_UNIT = 100.0
WORLD_SIZE = 256
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "alpine.titan")
FIXTURE_V4 = os.path.join(HERE, "fixtures", "graph_v4.titan")

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
results = {"checks": [], "done": False}


def check(ok, label, detail=""):
    results["checks"].append({"ok": bool(ok), "label": label, "detail": str(detail)})
    unreal.log("[titan-sync] %s  %s %s" % ("PASS" if ok else "FAIL", label, detail))


def landscapes():
    return [a for a in ACTORS.get_all_level_actors()
            if isinstance(a, unreal.Landscape)]


def spawn(name, resolution):
    a = ACTORS.spawn_actor_from_class(
        unreal.TitanTerrainActor, unreal.Vector(0.0, 0.0, 0.0))
    a.set_actor_label(name)
    a.set_editor_property("seed", "titan")
    a.set_editor_property("resolution", resolution)
    a.set_editor_property("world_size", WORLD_SIZE)
    a.set_editor_property("units_per_world_unit", CM_PER_UNIT)
    a.set_editor_property("hydraulic_erosion", False)
    a.set_editor_property("river_networks", False)
    a.set_editor_property("thermal_weathering", False)
    return a


try:
    # No level is created: on 5.5 that is what crashes. Assertions are scoped
    # to actors this run made, so whatever world the commandlet opened is
    # irrelevant.
    before = len(landscapes())

    full = spawn("SyncFull", 256)
    half = spawn("SyncHalf", 128)
    check(full is not None and half is not None, "actors spawn from a commandlet")

    full.generate_terrain_now()
    half.generate_terrain_now()

    mesh = full.get_editor_property("procedural_mesh")
    check(mesh.get_num_sections() == 1,
          "GenerateTerrainNow produced a mesh section before returning",
          "sections=%d" % mesh.get_num_sections())

    origin, extent = full.get_actor_bounds(False)
    expected = WORLD_SIZE * CM_PER_UNIT
    check(abs(extent.x * 2.0 - expected) < expected * 0.05,
          "the mesh spans WorldSize * UnitsPerWorldUnit",
          "%.0f cm vs %.0f expected" % (extent.x * 2.0, expected))
    check(extent.z > 1.0, "the mesh has vertical relief",
          "half-height %.0f cm" % extent.z)
    mesh_floor = origin.z - extent.z
    check(abs(mesh_floor) < 50.0, "the mesh base lands at the actor's Z",
          "%.0f cm" % mesh_floor)

    _, half_extent = half.get_actor_bounds(False)
    check(abs(half_extent.x * 2.0 - extent.x * 2.0) < extent.x * 2.0 * 0.05,
          "half-resolution covers the same ground",
          "%.0f cm vs %.0f cm" % (half_extent.x * 2.0, extent.x * 2.0))

    # Landscape output, which is the thing 5.5 has never verified.
    land = spawn("SyncLandscape", 256)
    land.set_editor_property("output", unreal.TitanOutput.LANDSCAPE)
    land.generate_terrain_now()

    ls = land.get_editor_property("spawned_landscape")
    check(ls is not None, "ApplyLandscape ran and recorded a Landscape actor")
    check(len(landscapes()) == before + 1,
          "the run added exactly one Landscape actor",
          "%d before, %d after" % (before, len(landscapes())))
    if ls is not None:
        parts = [a for a in ACTORS.get_all_level_actors()
                 if isinstance(a, unreal.LandscapeStreamingProxy)
                 and a.get_landscape_actor() == ls] or [ls]
        lo = min(a.get_actor_bounds(False)[0].x - a.get_actor_bounds(False)[1].x
                 for a in parts)
        hi = max(a.get_actor_bounds(False)[0].x + a.get_actor_bounds(False)[1].x
                 for a in parts)
        z_lo = min(a.get_actor_bounds(False)[0].z - a.get_actor_bounds(False)[1].z
                   for a in parts)
        check(abs((hi - lo) - expected) < expected * 0.05,
              "the Landscape covers the same world as the mesh",
              "%.0f cm vs %.0f expected" % (hi - lo, expected))
        check(abs(z_lo) < 50.0, "the Landscape base lands at the actor's Z",
              "%.0f cm" % z_lo)
        check(ls.get_landscape_actor() is not None,
              "the Landscape resolves its LandscapeInfo")
        check(land.get_editor_property("procedural_mesh").get_num_sections() == 0,
              "the landscape actor's own mesh is cleared")

    # .titan import, same fixture the tick-based suite uses.
    imp = spawn("SyncImport", 256)
    imp.set_editor_property("project_file", unreal.FilePath(FIXTURE))
    imp.import_project()
    report = imp.get_editor_property("import_report")
    check(report.startswith("Imported v2"), "the project imports", report)
    check(imp.get_editor_property("seed") == "wvcwvqj", "seed came across")
    check(imp.get_editor_property("world_size") == 128, "world size came across")
    check(imp.get_editor_property("noise_type") == unreal.TitanNoiseType.RIDGED,
          "noise structure mapped from its name")
    check(imp.get_editor_property("droplet_rounds") == 4,
          "hydraulic iterations converted to rounds")

    imp.set_editor_property("project_file", unreal.FilePath(FIXTURE_V4))
    imp.import_project()
    v4 = imp.get_editor_property("import_report")
    check("failed" in v4.lower() and "node graph" in v4.lower(),
          "a graph-driven v4 project is refused with a reason", v4)
except Exception as exc:                                          # noqa: BLE001
    check(False, "suite ran to completion", exc)

results["done"] = True
with open(RESULT, "w") as handle:
    json.dump(results, handle, indent=2)
unreal.log("[titan-sync] wrote %s" % RESULT)
