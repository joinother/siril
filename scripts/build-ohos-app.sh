#!/usr/bin/env bash
#
# Build the SIRIL engine for HarmonyOS and package it into a HAP.
#
#   scripts/build-ohos-app.sh native   # cross-compile libsiril-ohos.so
#   scripts/build-ohos-app.sh hap      # package the HAP (unsigned)
#   scripts/build-ohos-app.sh all      # both (default)
#
# Required environment (defaults point at the sibling build tree):
#   SIRIL_OHOS_BUILD   build root that holds sysroot/ and tmp/
#   SIRIL_OHOS_PREFIX  cross-compiled install prefix (default: $SIRIL_OHOS_BUILD/sysroot)
#   OHOS_NATIVE_SDK    DevEco native SDK (default: DevEco Studio 26 path)
#
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${SIRIL_OHOS_BUILD:-$(dirname "$REPO_ROOT")/siril-ohos-build}"
PREFIX="${SIRIL_OHOS_PREFIX:-$BUILD_ROOT/sysroot}"
DEVECO="${DEVECO_HOME:-/Applications/DevEco-Studio.app/Contents}"
SDK="${OHOS_NATIVE_SDK:-$DEVECO/sdk/default/openharmony/native}"
MODE="${1:-all}"

export PATH="$BUILD_ROOT/.venv/bin:$DEVECO/tools/node/bin:/opt/homebrew/bin:$PATH"
export SIRIL_OHOS_PREFIX="$PREFIX"

HVIGOR="$DEVECO/tools/hvigor/bin/hvigorw"
HARMONY_DIR="$REPO_ROOT/harmonyos"
RAWFILE_DATA="$HARMONY_DIR/entry/src/main/resources/rawfile/siril-data"
LIBS_DIR="$HARMONY_DIR/entry/libs/arm64-v8a"

log() { printf '\033[1;36m==>\033[0m %s\n' "$*"; }

build_catalog() {
  log "generating command catalog"
  mkdir -p "$RAWFILE_DATA"
  python3 "$REPO_ROOT/scripts/gen-command-catalog.py" \
    -o "$RAWFILE_DATA/commands.json"
}

build_native() {
  build_catalog
  log "configuring SIRIL (headless + ohos bridge)"
  if [ ! -d "$BUILD_ROOT/tmp/siril-build" ]; then
    meson setup "$BUILD_ROOT/tmp/siril-build" "$REPO_ROOT" \
      --cross-file "$BUILD_ROOT/ohos-cross.ini" \
      --prefix="$PREFIX" --buildtype=release \
      -Dgtk=false -Dohos_bridge=true -Drelocatable-bundle=yes \
      -Dopenmp=false -Dlibcurl=false -Dsqlite=false -Dlibheif=false \
      -Dffms2=false -Dffmpeg=false -Dlibjxl=false -DlibXISF=false \
      -Dlibgit2=false -Dlibtiff=true -Dlibjpeg=true -Dlibpng=true \
      -Dexiv2=true -Dlibraw=true -Darch_native=false -Db_staticpic=true
  fi
  log "compiling SIRIL engine"
  # Only the embeddable engine is needed on HarmonyOS; siril-cli is a desktop
  # companion and links network code that the headless OHOS build disables.
  meson compile -C "$BUILD_ROOT/tmp/siril-build" src/libsiril-ohos.so -j"${JOBS:-6}"
  cp "$BUILD_ROOT/tmp/siril-build/src/libsiril-ohos.so" "$PREFIX/lib/"
  log "engine: $PREFIX/lib/libsiril-ohos.so"
}

package_hap() {
  log "collecting native libraries"
  mkdir -p "$LIBS_DIR"
  python3 "$REPO_ROOT/scripts/collect-ohos-libs.py" \
    --root "$PREFIX/lib/libsiril-ohos.so" \
    --prefix "$PREFIX" \
    --sysroot "$SDK/sysroot" \
    --out "$LIBS_DIR"

  log "staging SIRIL data files"
  mkdir -p "$RAWFILE_DATA"
  cp "$REPO_ROOT"/data/*.csv "$RAWFILE_DATA"/
  cp "$RAWFILE_DATA/commands.json" "$RAWFILE_DATA/commands.json"

  log "building HAP"
  cd "$HARMONY_DIR"
  if [ ! -d "$HARMONY_DIR/oh_modules" ]; then
    "$DEVECO/tools/ohpm/bin/ohpm" install --all
  fi
  "$HVIGOR" --mode module -p product=default -p buildMode=debug assembleHap --no-daemon
  cd "$REPO_ROOT"

  local hap
  hap="$(find "$HARMONY_DIR/entry/build" -name '*unsigned.hap' -o -name '*.hap' | head -1)"
  if [ -n "$hap" ]; then
    log "HAP: $hap"
    printf '%s\n' "$hap" > "$BUILD_ROOT/last-hap.txt"
  else
    echo "HAP not found under harmonyos/entry/build" >&2
    exit 1
  fi
}

case "$MODE" in
  native) build_native ;;
  hap) package_hap ;;
  all) build_native; package_hap ;;
  *) echo "usage: $0 [native|hap|all]" >&2; exit 1 ;;
esac
