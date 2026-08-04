#!/bin/bash
# Builds TitanBridge, drops it in a throwaway project, and drives a real
# Unreal editor through generating terrain.
#
#     unreal/tests/run_editor_test.sh [/path/to/UE_5.x]
#
# This is the one test CI cannot run: it needs an Unreal installation, and
# there is none on a runner. cpp/tests/test_bridge_contract.cpp covers the
# engine side of the same path natively and does run in CI — what only this
# can reach is the Unreal half: the actor spawning in a world, the async
# generation completing back on the game thread, and CreateMeshSection
# accepting the buffers the plugin hands it.
#
# Run it before shipping a plugin build, and after any change to
# TitanTerrainActor or to the engine's mesh output.
set -uo pipefail

UE=${1:-/Users/Shared/Epic Games/UE_5.8}
HERE=$(cd "$(dirname "$0")" && pwd)
PLUGIN="$HERE/../TitanBridge/TitanBridge.uplugin"
WORK=${TITAN_EDITOR_TEST_DIR:-/tmp/TitanEditorTest}

UAT="$UE/Engine/Build/BatchFiles/RunUAT.sh"
EDITOR="$UE/Engine/Binaries/Mac/UnrealEditor-Cmd"

for required in "$UAT" "$EDITOR"; do
    if [ ! -x "$required" ]; then
        echo "error: $required not found or not executable." >&2
        echo "       Pass the engine root as the first argument, e.g." >&2
        echo "       $0 '/Users/Shared/Epic Games/UE_5.8'" >&2
        exit 1
    fi
done

echo "==> Building TitanBridge against $(basename "$UE")"
rm -rf "$WORK"
mkdir -p "$WORK/Plugins" "$WORK/Content"
if ! "$UAT" BuildPlugin -Plugin="$PLUGIN" -Package="$WORK/Plugins/TitanBridge" \
        -TargetPlatforms=Mac -Rocket >"$WORK/build.log" 2>&1; then
    echo "BUILD FAILED — see $WORK/build.log" >&2
    grep -iE "error" "$WORK/build.log" | head -20 >&2
    exit 1
fi
echo "    built"

cat > "$WORK/TitanEditorTest.uproject" <<'JSON'
{
  "FileVersion": 3,
  "EngineAssociation": "",
  "Description": "Throwaway harness for the TitanBridge editor test.",
  "Plugins": [
    { "Name": "TitanBridge", "Enabled": true },
    { "Name": "ProceduralMeshComponent", "Enabled": true },
    { "Name": "PythonScriptPlugin", "Enabled": true }
  ]
}
JSON


echo "==> Driving the editor"
# -ExecCmds rather than the pythonscript commandlet, deliberately. The
# commandlet runs the script and exits without ticking, and generation
# finishes on a game-thread task — so under the commandlet the mesh never
# arrives and the test would pass or fail for the wrong reason.
RESULT="$WORK/result.json"
rm -f "$RESULT"
TITAN_TEST_RESULT="$RESULT" "$EDITOR" "$WORK/TitanEditorTest.uproject" \
    -ExecCmds="py $HERE/editor_test.py" \
    -unattended -nopause -nosplash -nullrhi -stdout >"$WORK/editor.log" 2>&1

if [ ! -f "$RESULT" ]; then
    echo "EDITOR TEST DID NOT REPORT — see $WORK/editor.log" >&2
    tail -30 "$WORK/editor.log" >&2
    exit 1
fi

report() {
    python3 - "$1" "$2" <<'PY'
import json, sys
name = sys.argv[2]
data = json.load(open(sys.argv[1]))
failed = 0
for c in data["checks"]:
    print("  %s  %s%s" % ("PASS" if c["ok"] else "FAIL", c["label"],
                          ("  (%s)" % c["detail"]) if c["detail"] else ""))
    failed += 0 if c["ok"] else 1
print()
if failed or not data.get("done"):
    print("%s FAILED (%d check%s)" % (name, failed, "" if failed == 1 else "s"))
    sys.exit(1)
print("%s PASSED (%d checks)" % (name, len(data["checks"])))
PY
}

STATUS=0
report "$RESULT" "EDITOR TEST" || STATUS=1

# World Partition needs a different world, so it is a separate editor run
# rather than another phase of the same script.
echo
echo "==> Driving the editor (World Partition)"
WP_RESULT="$WORK/result_wp.json"
rm -f "$WP_RESULT"
TITAN_WP_RESULT="$WP_RESULT" "$EDITOR" "$WORK/TitanEditorTest.uproject" \
    -ExecCmds="py $HERE/editor_test_wp.py" \
    -unattended -nopause -nosplash -nullrhi -stdout >"$WORK/editor_wp.log" 2>&1

if [ ! -f "$WP_RESULT" ]; then
    echo "WORLD PARTITION TEST DID NOT REPORT — see $WORK/editor_wp.log" >&2
    tail -20 "$WORK/editor_wp.log" >&2
    exit 1
fi
report "$WP_RESULT" "WORLD PARTITION TEST" || STATUS=1
exit $STATUS
