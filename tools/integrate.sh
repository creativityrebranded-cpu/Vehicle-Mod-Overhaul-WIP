#!/bin/bash
# Build the plugin and install it into the game (Windows, Git Bash). One run = one build and, if the game is closed, one install.
#   bash tools/integrate.sh "short note about what is in this build"
# Paths: STARFIELD_DATA (default: the Steam install), SFSE_ROOT (optional: other SFSE headers; source/third_party/sfse is bundled).
# Needs Visual Studio 2022 Build Tools and CMake 3.24+. The first run configures the build folder source/build.
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GAME_DATA="${STARFIELD_DATA:-/c/Program Files (x86)/Steam/steamapps/common/Starfield/Data}"
NOTE="${1:-no note}"
cd "$ROOT/source" || exit 1
EXTRA=()
[ -n "${SFSE_ROOT:-}" ] && EXTRA+=("-DSFSE_ROOT=$SFSE_ROOT")
cmake -S . -B build "${EXTRA[@]}" > /dev/null || { echo "cmake configure failed (Visual Studio 2022 Build Tools and CMake 3.24+ needed)"; exit 1; }
cmake --build build --config Release --target SeamlessVehicle 2>&1 | grep -E "error|SeamlessVehicle.dll"
DLL="build/Release/SeamlessVehicle.dll"
[ -f "$DLL" ] || { echo "no DLL built"; exit 1; }
if tasklist 2>/dev/null | grep -qi starfield.exe; then
  echo "BUILT, not installed: the game is running"; exit 0
fi
mkdir -p "$GAME_DATA/SFSE/Plugins"
cp "$DLL" "$GAME_DATA/SFSE/Plugins/" && echo "installed $DLL ($NOTE)"
for esm in "SeamlessPilots.esm" "Vehicles - Real.esm"; do
  [ -f "$ROOT/Data/$esm" ] && ! cmp -s "$ROOT/Data/$esm" "$GAME_DATA/$esm" && cp "$ROOT/Data/$esm" "$GAME_DATA/$esm" && echo "installed $esm"
done
