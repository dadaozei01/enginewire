#!/bin/bash
# Build EngineWire for UE 5.8 on a Mac. Run from a build/ue58* checkout.
#
#   scripts/build-ue58-mac.sh [--check-only] [--engine <UE_5.8 dir>] [--out <new output dir>]
#
# Never edits the engine, Apple_SDK.json, xcode-select or an installed plugin. An unsupported Xcode stops the
# build. To use another Xcode for this build only: DEVELOPER_DIR=/Applications/Xcode_X.app/Contents/Developer
set -uo pipefail

ENGINE="/Users/Shared/Epic Games/UE_5.8"
OUT=""
CHECK_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --check-only) CHECK_ONLY=1; shift ;;
    --engine) ENGINE="${2:?--engine needs a path}"; shift 2 ;;
    --out) OUT="${2:?--out needs a path}"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 2 ;;
  esac
done

fail() { echo "STOP: $*" >&2; exit 1; }

REPO="$(cd "$(dirname "$0")/.." && pwd)"
PLUGIN="$REPO/plugins/EngineWire/EngineWire.uplugin"
RUNUAT="$ENGINE/Engine/Build/BatchFiles/RunUAT.sh"
VERSION_FILE="$ENGINE/Engine/Build/Build.version"

# 1. The engine must be fully installed. The launcher creates these files late, so a running download fails here.
[ -f "$PLUGIN" ] || fail "plugin descriptor not found: $PLUGIN"
[ -f "$VERSION_FILE" ] && [ -x "$RUNUAT" ] && [ -d "$ENGINE/Engine/Binaries/Mac/UnrealEditor.app" ] ||
  fail "engine is not fully installed yet: $ENGINE"

read -r ENGINE_VER ENGINE_BUILD < <(/usr/bin/python3 -I -c '
import json,sys
v=json.load(open(sys.argv[1]))
print("%d.%d.%d %s"%(v["MajorVersion"],v["MinorVersion"],v["PatchVersion"],v.get("CompatibleChangelist","?")))' "$VERSION_FILE")
PLUGIN_VER="$(/usr/bin/python3 -I -c 'import json,sys;print(json.load(open(sys.argv[1]))["EngineVersion"])' "$PLUGIN")"

# 2. The descriptor must target this engine, so a 5.4 checkout cannot be built against 5.8 or the reverse.
[ "${ENGINE_VER%.*}" = "${PLUGIN_VER%.*}" ] ||
  fail "descriptor targets UE $PLUGIN_VER but the engine is UE $ENGINE_VER. Use the matching branch."

REV="$(git -C "$REPO" rev-parse --short HEAD)"
DIRTY="$(git -C "$REPO" status --porcelain -- plugins | wc -l | tr -d ' ')"
echo "Engine:  UE $ENGINE_VER (compatible changelist $ENGINE_BUILD)"
echo "Source:  $(git -C "$REPO" rev-parse --abbrev-ref HEAD) @ $REV, $DIRTY changed plugin files"
echo "Xcode:   $(xcodebuild -version | tr '\n' ' ')($(xcode-select -p))"
echo "Clang:   $(clang --version | head -1)"
echo "Windows package reference: UE 5.8.4"
[ "$ENGINE_VER" = "5.8.4" ] || echo "NOTE: this engine differs from the 5.8.4 the Windows package was tested on."

[ -n "$OUT" ] || OUT="$HOME/Builds/EngineWire-UE${ENGINE_VER}-Mac-$REV-$(date +%Y%m%d-%H%M%S)"
[ ! -e "$OUT" ] || fail "output already exists; each build needs a new directory: $OUT"
LOG_DIR="$HOME/Builds/logs"
mkdir -p "$LOG_DIR" || fail "cannot create $LOG_DIR"
STAMP="$(date +%Y%m%d-%H%M%S)"
SDK_LOG="$LOG_DIR/ue58-verifysdk-$STAMP.log"
BUILD_LOG="$LOG_DIR/ue58-buildplugin-$STAMP.log"

# 3. Is this Xcode inside the range the engine allows? Reports only; changing it is the user's decision.
echo; echo "== VerifySdk =="
"$RUNUAT" Turnkey -command=VerifySdk -platform=Mac -UpdateIfNeeded=false >"$SDK_LOG" 2>&1
SDK_RC=$?
grep -E 'Status=|Current_Sdk|MaxAllowed_Sdk|MinAllowed_Sdk|ManualSDK|AutoSDK' "$SDK_LOG" | head -12
echo "Allowed range in Apple_SDK.json:"
grep -E '"(MinVersion|MaxVersion)"' "$ENGINE/Engine/Config/Apple/Apple_SDK.json" 2>/dev/null
if [ "$SDK_RC" -ne 0 ] || grep -q 'Status=Invalid' "$SDK_LOG"; then
  echo "Log: $SDK_LOG"
  fail "this Xcode is not accepted by UE $ENGINE_VER. Choose: install a supported Xcode and set DEVELOPER_DIR, or back up and raise MaxVersion yourself."
fi

if [ "$CHECK_ONLY" = 1 ]; then
  echo; echo "Checks passed. Nothing was built."
  exit 0
fi

# 4. Build. Keep the full log; print the distinct compiler errors on failure.
echo; echo "== BuildPlugin =="; echo "Output: $OUT"; echo "Log:    $BUILD_LOG"
"$RUNUAT" BuildPlugin -Plugin="$PLUGIN" -Package="$OUT" -TargetPlatforms=Mac 2>&1 | tee "$BUILD_LOG"
BUILD_RC=${PIPESTATUS[0]}
if [ "$BUILD_RC" -ne 0 ]; then
  echo; echo "== Build failed (exit $BUILD_RC). Distinct errors: =="
  grep -E '(: (fatal )?error:|Undefined symbols|ld: |not a valid platform)' "$BUILD_LOG" | sed -E 's/^[[:space:]]+//' | sort -u | head -60
  exit "$BUILD_RC"
fi

# 5. Inspect what was produced. Installing it stays a separate, deliberate step.
echo; echo "== Result =="
FOUND=0
while IFS= read -r DYLIB; do
  FOUND=$((FOUND + 1))
  echo "$(basename "$DYLIB"): $(lipo -archs "$DYLIB" 2>&1)"
done < <(find "$OUT/Binaries/Mac" -name '*.dylib' 2>/dev/null)
[ "$FOUND" -gt 0 ] || fail "build reported success but produced no dylib under $OUT/Binaries/Mac"
for MODULES in "$OUT"/Binaries/Mac/*.modules; do
  [ -f "$MODULES" ] && /usr/bin/python3 -I -c '
import json,sys
print("BuildId:",json.load(open(sys.argv[1])).get("BuildId"))' "$MODULES" && break
done
echo "Package ready: $OUT"
