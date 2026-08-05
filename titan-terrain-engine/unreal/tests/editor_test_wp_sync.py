# World Partition coverage that works on every engine.
#
# The tick-based WP suite creates a level from the Open World template, which
# UE 5.5 cannot survive: spawning an actor from Python under -ExecCmds asserts
# inside that engine. This one *loads* Unreal's Open World template instead of
# creating a level — a different operation, and one 5.5 is happy with — and
# drives generation synchronously so a commandlet can see the result.
#
# So World Partition is covered on 5.5 as well as 5.8. Where the two WP suites
# disagree, the tick-based one is the reference: it exercises the async path
# that actually ships.

import json
import os
import unreal

RESULT = os.environ.get("TITAN_WPSYNC_RESULT",
                        "/tmp/TitanEditorTest/result_wp_sync.json")
CM_PER_UNIT = 100.0
WORLD_SIZE = 4096

ACTORS = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
results = {"checks": [], "done": False}


def check(ok, label, detail=""):
    results["checks"].append({"ok": bool(ok), "label": label, "detail": str(detail)})
    unreal.log("[titan-wpsync] %s  %s %s" % ("PASS" if ok else "FAIL", label, detail))


def finish():
    results["done"] = True
    with open(RESULT, "w") as handle:
        json.dump(results, handle, indent=2)
    unreal.log("[titan-wpsync] wrote %s" % RESULT)


try:
    # Load, do not create. Creating is what 5.5 rejects.
    unreal.EditorLoadingAndSavingUtils.load_map("/Engine/Maps/Templates/OpenWorld")

    # Evidence, not the template's name: WorldDataLayers only exists in a
    # partitioned world.
    data_layers = [a for a in ACTORS.get_all_level_actors()
                   if type(a).__name__ == "WorldDataLayers"]
    check(len(data_layers) > 0, "loaded a World Partition world",
          "WorldDataLayers actors: %d" % len(data_layers))
    if not data_layers:
        finish()
        raise SystemExit(0)

    before = len([a for a in ACTORS.get_all_level_actors()
                  if isinstance(a, unreal.Landscape)])

    actor = ACTORS.spawn_actor_from_class(
        unreal.TitanTerrainActor, unreal.Vector(0.0, 0.0, 0.0))
    actor.set_actor_label("TitanWPSync")
    actor.set_editor_property("resolution", 1024)
    actor.set_editor_property("world_size", WORLD_SIZE)
    actor.set_editor_property("units_per_world_unit", CM_PER_UNIT)
    actor.set_editor_property("hydraulic_erosion", False)
    actor.set_editor_property("river_networks", False)
    actor.set_editor_property("thermal_weathering", False)
    actor.set_editor_property("output", unreal.TitanOutput.LANDSCAPE)
    actor.generate_terrain_now()

    ls = actor.get_editor_property("spawned_landscape")
    check(ls is not None, "ApplyLandscape ran in a World Partition world")

    after = len([a for a in ACTORS.get_all_level_actors()
                 if isinstance(a, unreal.Landscape)])
    check(after == before + 1, "the run added exactly one Landscape actor",
          "%d before, %d after" % (before, after))

    if ls is not None:
        # Scoped to this landscape. The template ships one of its own, and
        # counting every proxy in the level mixes the two — which first showed
        # up as the terrain measuring eight times its own width.
        proxies = [a for a in ACTORS.get_all_level_actors()
                   if isinstance(a, unreal.LandscapeStreamingProxy)
                   and a.get_landscape_actor() == ls]
        check(len(proxies) > 0, "the landscape was split into streaming proxies",
              "%d proxy actor(s)" % len(proxies))

        streamable = [a for a in proxies
                      if a.get_editor_property("is_spatially_loaded")]
        check(len(proxies) > 0 and len(streamable) == len(proxies),
              "every proxy is spatially loaded, so World Partition streams them",
              "%d of %d" % (len(streamable), len(proxies)))

        # Once split, the parent's own bounds are legitimately zero — its
        # components have moved into the proxies — so measure through whatever
        # holds the terrain.
        parts = proxies or [ls]
        lo = min(a.get_actor_bounds(False)[0].x - a.get_actor_bounds(False)[1].x
                 for a in parts)
        hi = max(a.get_actor_bounds(False)[0].x + a.get_actor_bounds(False)[1].x
                 for a in parts)
        z_lo = min(a.get_actor_bounds(False)[0].z - a.get_actor_bounds(False)[1].z
                   for a in parts)
        z_hi = max(a.get_actor_bounds(False)[0].z + a.get_actor_bounds(False)[1].z
                   for a in parts)
        expected = WORLD_SIZE * CM_PER_UNIT
        check(abs((hi - lo) - expected) < expected * 0.05,
              "the proxies together cover the whole terrain",
              "%.0f cm vs %.0f expected" % (hi - lo, expected))
        check((z_hi - z_lo) > 1.0, "the WP landscape has vertical relief",
              "%.0f cm tall" % (z_hi - z_lo))
        check(abs(z_lo) < 50.0, "the WP landscape's base lands at the actor's Z",
              "floor %.0f cm" % z_lo)
        check(ls.get_landscape_actor() is not None,
              "the WP landscape resolves its LandscapeInfo")
except SystemExit:
    raise
except Exception as exc:                                          # noqa: BLE001
    check(False, "suite ran to completion", exc)

finish()
