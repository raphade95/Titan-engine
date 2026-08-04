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
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "alpine.titan")
FIXTURE_V4 = os.path.join(HERE, "fixtures", "graph_v4.titan")

log = unreal.log
results = {"checks": [], "done": False}

# EditorLevelLibrary is deprecated and, on 5.5, actively dangerous: spawning
# through it after a new_level asserts inside the engine. Everything goes
# through the subsystems instead.
ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
LEVELS = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)



def check(ok, label, detail=""):
    results["checks"].append({"ok": bool(ok), "label": label, "detail": str(detail)})
    log("[titan-test] %s  %s %s" % ("PASS" if ok else "FAIL", label, detail))


def bail(why):
    """Stop rather than test in a world we did not build.

    Proceeding past a failed level setup is what produced this harness's worst
    results: it ran in the editor's Open World default, measured the
    template's landscape, and reported failures that had nothing to do with
    the plugin. A setup failure has to end the run, not colour it.
    """
    check(False, "level setup succeeded", why)
    results["done"] = True
    with open(RESULT, "w") as handle:
        json.dump(results, handle, indent=2)
    log("[titan-test] ABORTED: %s" % why)
    os._exit(0)


# Start from an empty level, not whatever the editor opens by default.
#
# This cost a whole debugging session: the default startup map is an Open
# World template that already contains a Landscape actor, so the landscape
# assertions below were measuring the template's terrain and reporting
# failures that had nothing to do with the plugin. A test that inherits an
# unknown world is not testing what it thinks it is.
# All world setup is deferred to a tick, not run inline.
#
# -ExecCmds fires while the editor is still bringing a world up. On 5.8 that
# is survivable; on 5.5 creating a level and then spawning into it asserts
# inside SpawnActorFromClass — "array index out of bounds: 0 into an array of
# size 0" — because the new world has no level to spawn into yet. Giving the
# editor a few ticks between each step is what makes the harness portable
# across engine versions.
G = {}


def spawn(name, resolution):
    actor = ACTORS.spawn_actor_from_class(
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
    return [a for a in ACTORS.get_all_level_actors()
            if isinstance(a, unreal.Landscape)]


def make_level():
    try:
        made = LEVELS.new_level("/Game/TitanEditorTestMap")
    except Exception as exc:                                      # noqa: BLE001
        bail("new_level raised: %s" % exc)
        return
    if not made:
        bail("new_level returned False; refusing to test in the editor's default world")
    check(True, "started from a new empty level")


def make_actors():
    G["pre_existing"] = len(landscapes())
    check(G["pre_existing"] == 0, "the level starts with no Landscape actors",
          "found %d" % G["pre_existing"])

    G["full"] = spawn("TitanFull", 256)
    G["half"] = spawn("TitanHalf", 128)
    check(G["full"] is not None and G["half"] is not None,
          "both actors spawn in the editor world")

    # A third actor set to Landscape output. It builds a separate ALandscape
    # rather than a mesh section, so it is polled on its own terms below.
    G["land"] = spawn("TitanLandscape", 256)
    G["land"].set_editor_property("output", unreal.TitanOutput.LANDSCAPE)

    G["full"].generate_terrain()
    G["half"].generate_terrain()
    G["land"].generate_terrain()
    log("[titan-test] generation requested")


def land():
    return G.get("land")


def spawned_landscape():
    """The landscape the plugin itself recorded — authoritative about whether
    ApplyLandscape ran at all, as opposed to what happens to be in the world."""
    try:
        return land().get_editor_property("spawned_landscape")
    except Exception:                                             # noqa: BLE001
        return None

state = {"ticks": 0, "handle": None}


def sections(actor):
    mesh = actor.get_editor_property("procedural_mesh")
    return mesh.get_num_sections() if mesh else 0


def finish():
    try:
        # 1. The async round trip completed and a section exists.
        check(sections(G["full"]) == 1, "GenerateTerrain produced one mesh section",
              "sections=%d after %d ticks" % (sections(G["full"]), state["ticks"]))

        # 2. Bounds. This is the cell-size fix, measured where a designer sees
        #    it: the actor must be WorldSize * cm-per-unit across.
        full_origin, extent = G["full"].get_actor_bounds(False)
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
        _, half_extent = G["half"].get_actor_bounds(False)
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
        # Scoped to what this test made, not to what is in the world.
        found = landscapes()
        check(len(found) == G["pre_existing"] + 1,
              "the run added exactly one Landscape actor",
              "%d before, %d after" % (G["pre_existing"], len(found)))
        check(mine in found, "and it is the one the plugin reported building")
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

            # Grounding. A landscape's zero plane is sample 32768, so spawning
            # it at the actor's Z buries the lower half of the terrain and
            # leaves the upper half in the air. Its floor must line up with the
            # mesh path's floor, or the same project lands in two different
            # places depending on which output you picked.
            ls_origin, _ = ls.get_actor_bounds(False)
            ls_floor = ls_origin.z - ls_extent.z
            mesh_floor = full_origin.z - extent.z
            check(abs(ls_floor - mesh_floor) < 50.0,
                  "the Landscape sits on the same floor as the mesh",
                  "landscape floor %.0f cm vs mesh floor %.0f cm" % (ls_floor, mesh_floor))
            # And that floor is the actor's own Z. Agreeing with each other is
            # not enough: both used to float by the terrain's own minimum
            # height, which put a project's base 145 cm above the ground you
            # placed it on.
            check(abs(mesh_floor) < 50.0,
                  "the mesh base lands at the actor's Z, not above it",
                  "%.0f cm" % mesh_floor)
            check(abs(ls_floor) < 50.0,
                  "the Landscape base lands at the actor's Z, not above it",
                  "%.0f cm" % ls_floor)
            check(ls.get_editor_property("landscape_material") is not None,
                  "the Landscape has a material")
        # 6. .titan import. A real project saved out of TitanLab, not a
        #    hand-written approximation of the schema — the point is to catch
        #    the format drifting away from this reader.
        imp = ACTORS.spawn_actor_from_class(
            unreal.TitanTerrainActor, unreal.Vector(0.0, 0.0, 0.0))
        imp.set_actor_label("TitanImported")
        imp.set_editor_property("project_file", unreal.FilePath(FIXTURE))
        imp.import_project()
        report = imp.get_editor_property("import_report")
        check(report.startswith("Imported v2"),
              "the project imports and reports what it did", report)
        check(imp.get_editor_property("seed") == "wvcwvqj",
              "seed came across", imp.get_editor_property("seed"))
        check(imp.get_editor_property("resolution") == 128,
              "resolution came across",
              imp.get_editor_property("resolution"))
        check(imp.get_editor_property("world_size") == 128,
              "world size came across",
              imp.get_editor_property("world_size"))
        check(imp.get_editor_property("noise_type") == unreal.TitanNoiseType.RIDGED,
              "noise structure mapped from its name")
        check(imp.get_editor_property("octaves") == 8, "octaves came across")
        # The fixture's stack is fluvial + hydraulic + thermal, so all three
        # toggles must be on and none reported as skipped.
        check(imp.get_editor_property("river_networks")
              and imp.get_editor_property("hydraulic_erosion")
              and imp.get_editor_property("thermal_weathering"),
              "all three erosion layers were recognised")
        check("not reproduced" not in report,
              "nothing in this project was silently dropped", report)
        # 65536 iterations / 16384 per round = 4.
        check(imp.get_editor_property("droplet_rounds") == 4,
              "hydraulic iterations converted to rounds",
              imp.get_editor_property("droplet_rounds"))

        # A v4 file is graph-driven and must be refused, not approximated.
        imp.set_editor_property("project_file", unreal.FilePath(FIXTURE_V4))
        imp.import_project()
        v4 = imp.get_editor_property("import_report")
        check("failed" in v4.lower() and "node graph" in v4.lower(),
              "a graph-driven v4 project is refused with a reason", v4)

        # Terrain must not arrive as the grey no-material placeholder.
        check(G["full"].get_editor_property("procedural_mesh").get_material(0) is not None,
              "the procedural mesh has a material")

        # The mesh on the landscape actor must be cleared, or both outputs
        # occupy the same space.
        land_mesh = G["land"].get_editor_property("procedural_mesh")
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
    n = state["ticks"]
    # Deliberate gaps: each step needs the editor to have ticked since the
    # last one. See the note on deferred setup above.
    try:
        if n == 3:
            make_level()
            return
        if n == 8:
            make_actors()
            return
    except Exception as exc:                                      # noqa: BLE001
        bail("setup raised on tick %d: %s" % (n, exc))
        return
    if n < 9:
        return
    if (sections(G["full"]) >= 1 and sections(G["half"]) >= 1
            and spawned_landscape() is not None):
        finish()
    elif state["ticks"] > MAX_TICKS:
        check(False, "generation completed within the tick budget",
              "gave up after %d ticks" % state["ticks"])
        finish()


state["handle"] = unreal.register_slate_post_tick_callback(poll)
log("[titan-test] polling for async completion")
