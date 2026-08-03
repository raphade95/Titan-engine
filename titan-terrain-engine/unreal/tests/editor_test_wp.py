# World Partition pass for TitanBridge's Landscape output.
#
# The main editor test runs in a plain empty level. That is the right place to
# test the plugin in isolation, and the wrong place to find out whether
# Landscape output survives World Partition — which is the world large-terrain
# customers actually build in, and where ALandscape does not behave like an
# ordinary actor: WP splits a landscape into streaming proxies, and an actor
# spawned with a plain SpawnActor may not be registered with the partition at
# all.
#
# So this repeats the landscape path in a WP world and asserts the same things
# plus the WP-specific ones.

import json
import os
import unreal

RESULT = os.environ.get("TITAN_WP_RESULT", "/tmp/TitanEditorTest/result_wp.json")
CM_PER_UNIT = 100.0
WORLD_SIZE = 4096
MAX_TICKS = 2400

results = {"checks": [], "done": False}


def check(ok, label, detail=""):
    results["checks"].append({"ok": bool(ok), "label": label, "detail": str(detail)})
    unreal.log("[titan-wp] %s  %s %s" % ("PASS" if ok else "FAIL", label, detail))


# Open World is Unreal's own World Partition template, so this is the same
# world shape a customer starts from rather than a hand-rolled approximation.
made = False
try:
    sub = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    made = sub.new_level_from_template("/Game/TitanWPMap",
                                       "/Engine/Maps/Templates/OpenWorld")
except Exception as exc:                                          # noqa: BLE001
    check(False, "created a World Partition level from the Open World template", exc)
# Whether this call succeeds is not the requirement — being in a World
# Partition world is. The editor's own default startup map is the Open World
# template, so the test lands in a WP world either way; recorded rather than
# asserted so a False here does not read as a failure of the plugin.
unreal.log("[titan-wp] new_level_from_template returned %s" % made)

# Confirm the world really is partitioned rather than trusting the template
# name. WorldDataLayers only exists in a World Partition world, so its
# presence is the evidence; asking the UWorld for a "world_partition"
# property is not exposed to Python and quietly reports nothing, which would
# have made every check below meaningless while still looking like a result.
data_layers = [a for a in unreal.EditorLevelLibrary.get_all_level_actors()
               if type(a).__name__ == "WorldDataLayers"]
check(len(data_layers) > 0,
      "the level really is World Partition enabled",
      "WorldDataLayers actors: %d" % len(data_layers))


def landscapes():
    return [a for a in unreal.EditorLevelLibrary.get_all_level_actors()
            if isinstance(a, unreal.Landscape)]


baseline = landscapes()
unreal.log("[titan-wp] template already has %d landscape(s)" % len(baseline))

actor = unreal.EditorLevelLibrary.spawn_actor_from_class(
    unreal.TitanTerrainActor, unreal.Vector(0.0, 0.0, 0.0))
actor.set_actor_label("TitanWP")
actor.set_editor_property("resolution", 1024)
actor.set_editor_property("world_size", WORLD_SIZE)
actor.set_editor_property("units_per_world_unit", CM_PER_UNIT)
actor.set_editor_property("hydraulic_erosion", False)
actor.set_editor_property("river_networks", False)
actor.set_editor_property("thermal_weathering", False)
actor.set_editor_property("output", unreal.TitanOutput.LANDSCAPE)
actor.generate_terrain()

state = {"ticks": 0, "handle": None}


def mine():
    try:
        return actor.get_editor_property("spawned_landscape")
    except Exception:                                             # noqa: BLE001
        return None


def finish():
    try:
        ls = mine()
        check(ls is not None, "ApplyLandscape ran in a World Partition world")
        if ls is not None:
            # Once split, the parent landscape's own bounds are legitimately
            # zero: its components have moved out into the proxies. So measure
            # the terrain through whatever actually holds it.
            parts = [a for a in unreal.EditorLevelLibrary.get_all_level_actors()
                     if isinstance(a, unreal.LandscapeStreamingProxy)
                     and a.get_landscape_actor() == ls] or [ls]
            xs_lo = min(a.get_actor_bounds(False)[0].x - a.get_actor_bounds(False)[1].x
                        for a in parts)
            xs_hi = max(a.get_actor_bounds(False)[0].x + a.get_actor_bounds(False)[1].x
                        for a in parts)
            z_lo = min(a.get_actor_bounds(False)[0].z - a.get_actor_bounds(False)[1].z
                       for a in parts)
            z_hi = max(a.get_actor_bounds(False)[0].z + a.get_actor_bounds(False)[1].z
                       for a in parts)

            class _B:  # minimal stand-in so the checks below read unchanged
                pass
            origin, extent = _B(), _B()
            origin.z = (z_lo + z_hi) * 0.5
            extent.x = (xs_hi - xs_lo) * 0.5
            extent.z = (z_hi - z_lo) * 0.5
            span = extent.x * 2.0
            expected = WORLD_SIZE * CM_PER_UNIT
            check(abs(span - expected) < expected * 0.05,
                  "the WP landscape covers the intended world",
                  "%.0f cm vs %.0f expected" % (span, expected))
            check(extent.z > 1.0, "the WP landscape has vertical relief",
                  "half-height %.0f cm" % extent.z)
            floor = origin.z - extent.z
            check(abs(floor) < 500.0,
                  "the WP landscape sits near the actor's floor, not in the air",
                  "floor %.0f cm" % floor)
            check(ls.get_landscape_actor() is not None,
                  "the WP landscape resolves its LandscapeInfo")
            # The real WP question. A landscape held as a single actor keeps
            # every component always loaded, which is exactly what World
            # Partition exists to avoid — so "it built correctly" is not the
            # bar. It has to be split into streaming proxies.
            # Only this landscape's proxies. The Open World template ships a
            # landscape of its own, and counting every proxy in the level
            # mixes the two — which first showed up as the terrain measuring
            # eight times its own width.
            proxies = [a for a in unreal.EditorLevelLibrary.get_all_level_actors()
                       if isinstance(a, unreal.LandscapeStreamingProxy)
                       and a.get_landscape_actor() == ls]
            check(len(proxies) > 0,
                  "the landscape was split into streaming proxies",
                  "%d proxy actor(s)" % len(proxies))
            streamable = [a for a in proxies
                          if a.get_editor_property("is_spatially_loaded")]
            check(len(streamable) == len(proxies) and len(proxies) > 0,
                  "every proxy is spatially loaded, so World Partition streams them",
                  "%d of %d" % (len(streamable), len(proxies)))
            # The proxies must cover the same ground the landscape did, or the
            # split has quietly moved or dropped terrain.
            if proxies:
                lo = min(a.get_actor_bounds(False)[0].x
                         - a.get_actor_bounds(False)[1].x for a in proxies)
                hi = max(a.get_actor_bounds(False)[0].x
                         + a.get_actor_bounds(False)[1].x for a in proxies)
                check(abs((hi - lo) - expected) < expected * 0.05,
                      "the proxies together cover the whole terrain",
                      "%.0f cm vs %.0f expected" % (hi - lo, expected))
    except Exception as exc:                                      # noqa: BLE001
        check(False, "WP test body raised", exc)

    results["done"] = True
    with open(RESULT, "w") as handle:
        json.dump(results, handle, indent=2)
    if state["handle"] is not None:
        unreal.unregister_slate_post_tick_callback(state["handle"])
    os._exit(0)


def poll(delta_seconds):
    state["ticks"] += 1
    if mine() is not None:
        finish()
    elif state["ticks"] > MAX_TICKS:
        check(False, "generation completed within the tick budget",
              "gave up after %d ticks" % state["ticks"])
        finish()


state["handle"] = unreal.register_slate_post_tick_callback(poll)
unreal.log("[titan-wp] polling")
