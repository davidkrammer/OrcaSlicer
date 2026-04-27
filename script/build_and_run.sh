#!/usr/bin/env bash
set -euo pipefail

MODE="run"
REFRESH_BUNDLE=0

for arg in "$@"; do
    case "$arg" in
        run)
            MODE="run"
            ;;
        --no-launch|no-launch)
            MODE="no-launch"
            ;;
        --debug|debug)
            MODE="debug"
            ;;
        --logs|logs)
            MODE="logs"
            ;;
        --verify|verify)
            MODE="verify"
            ;;
        --copy-downloads|copy-downloads|--refresh-bundle|refresh-bundle)
            REFRESH_BUNDLE=1
            ;;
        --help|-h|help)
            echo "usage: $0 [run|--no-launch|--debug|--logs|--verify] [--copy-downloads]"
            exit 0
            ;;
        *)
            echo "unknown argument: $arg" >&2
            echo "usage: $0 [run|--no-launch|--debug|--logs|--verify] [--copy-downloads]" >&2
            exit 2
            ;;
    esac
done

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ARCH="${ARCH:-arm64}"
CONFIG="${CONFIG:-Release}"
JOBS="${JOBS:-$(sysctl -n hw.logicalcpu)}"
BUILD_DIR="$PROJECT_DIR/build/$ARCH"
LOCAL_CMAKE="$PROJECT_DIR/.codex-tools/cmake-3.31.7-macos-universal/CMake.app/Contents/bin/cmake"
CMAKE_BIN="${CMAKE:-cmake}"

if [[ -x "$LOCAL_CMAKE" ]]; then
    CMAKE_BIN="$LOCAL_CMAKE"
fi

PACKAGED_APP="$BUILD_DIR/Snapmaker_Orca/Snapmaker Orca.app"
PACKAGED_BIN="$PACKAGED_APP/Contents/MacOS/Snapmaker_Orca"
BUILT_BIN="$BUILD_DIR/src/$CONFIG/Snapmaker_Orca.app/Contents/MacOS/Snapmaker_Orca"
DOWNLOADS_DIR="$HOME/Downloads/OrcaSlicer-colored-obj-cmy-build"
APP_DISPLAY_NAME="${APP_DISPLAY_NAME:-Snapmaker Orca Color}"
APP_BUNDLE_ID="${APP_BUNDLE_ID:-com.davidkrammer.snapmaker-orca-color}"
DOWNLOADS_APP="$DOWNLOADS_DIR/$APP_DISPLAY_NAME.app"
DOWNLOADS_BIN="$DOWNLOADS_APP/Contents/MacOS/Snapmaker_Orca"
DOWNLOADS_FRAMEWORKS="$DOWNLOADS_APP/Contents/Frameworks"
ZSTD_DYLIB="${ZSTD_DYLIB:-/opt/homebrew/opt/zstd/lib/libzstd.1.dylib}"

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo "Missing configured build directory: $BUILD_DIR" >&2
    echo "Run the full setup once first: ./build_release_macos.sh -a $ARCH -x -s -b" >&2
    exit 1
fi

pkill -x Snapmaker_Orca >/dev/null 2>&1 || true

"$CMAKE_BIN" --build "$BUILD_DIR" --config "$CONFIG" --target Snapmaker_Orca --parallel "$JOBS"

if [[ ! -f "$BUILT_BIN" ]]; then
    echo "Built binary not found: $BUILT_BIN" >&2
    exit 1
fi

if [[ ! -d "$PACKAGED_APP" ]]; then
    echo "Packaged app not found: $PACKAGED_APP" >&2
    echo "Run the full packaging step once first: ./build_release_macos.sh -a $ARCH -x -s -b" >&2
    exit 1
fi

if [[ "$REFRESH_BUNDLE" == "1" || ! -d "$DOWNLOADS_APP" ]]; then
    mkdir -p "$DOWNLOADS_DIR"
    rm -rf "$DOWNLOADS_APP"
    ditto --norsrc "$PACKAGED_APP" "$DOWNLOADS_APP"
fi

cp "$BUILT_BIN" "$DOWNLOADS_BIN"
if [[ -f "$ZSTD_DYLIB" ]]; then
    mkdir -p "$DOWNLOADS_FRAMEWORKS"
    cp "$ZSTD_DYLIB" "$DOWNLOADS_FRAMEWORKS/libzstd.1.dylib"
    chmod u+w "$DOWNLOADS_FRAMEWORKS/libzstd.1.dylib" 2>/dev/null || true
    install_name_tool -id "@executable_path/../Frameworks/libzstd.1.dylib" "$DOWNLOADS_FRAMEWORKS/libzstd.1.dylib" 2>/dev/null || true
    install_name_tool -change "$ZSTD_DYLIB" "@executable_path/../Frameworks/libzstd.1.dylib" "$DOWNLOADS_BIN" 2>/dev/null || true
fi
install_name_tool -change "@rpath/libsentry.dylib" "@executable_path/../Frameworks/libsentry.dylib" "$DOWNLOADS_BIN" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleName $APP_DISPLAY_NAME" "$DOWNLOADS_APP/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleDisplayName $APP_DISPLAY_NAME" "$DOWNLOADS_APP/Contents/Info.plist" 2>/dev/null || true
/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $APP_BUNDLE_ID" "$DOWNLOADS_APP/Contents/Info.plist" 2>/dev/null || true
find "$DOWNLOADS_APP" -name '.DS_Store' -delete
xattr -cr "$DOWNLOADS_APP"
xattr -d com.apple.FinderInfo "$DOWNLOADS_APP" 2>/dev/null || true
codesign --force --sign - "$DOWNLOADS_FRAMEWORKS/libzstd.1.dylib" 2>/dev/null || true
codesign --force --deep --sign - "$DOWNLOADS_APP" >/dev/null

APP_TO_OPEN="$DOWNLOADS_APP"

open_app() {
    /usr/bin/open -n "$APP_TO_OPEN"
}

case "$MODE" in
    run)
        open_app
        ;;
    no-launch)
        echo "Built test app: $DOWNLOADS_APP"
        ;;
    debug)
        lldb -- "$PACKAGED_BIN"
        ;;
    logs)
        open_app
        /usr/bin/log stream --info --style compact --predicate 'process == "Snapmaker_Orca"'
        ;;
    verify)
        open_app
        sleep 2
        pgrep -x Snapmaker_Orca >/dev/null
        echo "Snapmaker_Orca is running from: $APP_TO_OPEN"
        ;;
esac
