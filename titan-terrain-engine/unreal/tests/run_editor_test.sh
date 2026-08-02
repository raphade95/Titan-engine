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

python3 - "$RESULT" <<'PY'
import json, sys
data = json.load(open(sys.argv[1]))
failed = 0
for c in data["checks"]:
    print("  %s  %s%s" % ("PASS" if c["ok"] else "FAIL", c["label"],
                          ("  (%s)" % c["detail"]) if c["detail"] else ""))
    failed += 0 if c["ok"] else 1
print()
if failed or not data.get("done"):
    print("EDITOR TEST FAILED (%d check%s)" % (failed, "" if failed == 1 else "s"))
    sys.exit(1)
print("EDITOR TEST PASSED (%d checks)" % len(data["checks"]))
PY
