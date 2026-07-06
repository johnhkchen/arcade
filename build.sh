#!/usr/bin/env bash
# Build an arcade cartridge to WASM. Usage: ./build.sh penalty
set -euo pipefail

GAME="${1:-penalty}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
RAYLIB_SRC="${RAYLIB_SRC:-$HOME/raylib/src}"

# shellcheck disable=SC1090
source "$HOME/emsdk/emsdk_env.sh"

# Build raylib's web static lib once.
if [ ! -f "$RAYLIB_SRC/libraylib.web.a" ] && [ ! -f "$RAYLIB_SRC/libraylib.a" ]; then
  echo ">> building raylib (PLATFORM_WEB)…"
  make -C "$RAYLIB_SRC" PLATFORM=PLATFORM_WEB -B
fi
RAYLIB_LIB="$RAYLIB_SRC/libraylib.web.a"
[ -f "$RAYLIB_LIB" ] || RAYLIB_LIB="$RAYLIB_SRC/libraylib.a"

mkdir -p "$ROOT/dist/$GAME"
echo ">> compiling $GAME → dist/$GAME/index.html"
emcc -o "$ROOT/dist/$GAME/index.html" \
  "$ROOT/src/$GAME.c" "$ROOT/src/ballstrike.c" "$ROOT/src/jumbotron.c" "$ROOT/src/svg.c" \
  -I"$RAYLIB_SRC" \
  "$RAYLIB_LIB" \
  -DPLATFORM_WEB \
  -s USE_GLFW=3 \
  -s ASYNCIFY \
  -s INITIAL_MEMORY=67108864 \
  --shell-file "$ROOT/web/shell.html" \
  -O2

echo ">> done: dist/$GAME/index.html"
