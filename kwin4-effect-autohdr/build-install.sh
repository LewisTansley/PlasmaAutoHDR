#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"

CLEAN=0
RECONFIGURE=0
NO_INSTALL=0
RELOAD=0
JOBS="$(nproc)"

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build and install the kwin4-effect-autohdr KWin effect.

For a full install with dependencies and an interactive KWin restart prompt,
use ../install.sh from the repository root instead.

Options:
  --clean         Delete build/ and reconfigure from scratch
  --reconfigure   Re-run cmake without deleting build/
  --no-install    Build only, skip install
  --reload        Reload KWin after install (via D-Bus)
  -j N            Parallel make jobs (default: nproc)
  -h, --help      Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --reconfigure) RECONFIGURE=1; shift ;;
        --no-install) NO_INSTALL=1; shift ;;
        --reload) RELOAD=1; shift ;;
        -j)
            JOBS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

if [[ "$CLEAN" -eq 1 ]]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [[ "$CLEAN" -eq 1 || "$RECONFIGURE" -eq 1 || ! -f "$BUILD_DIR/Makefile" ]]; then
    echo "Configuring with cmake..."
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
fi

echo "Building with make -j$JOBS..."
make -C "$BUILD_DIR" -j"$JOBS"

if [[ "$NO_INSTALL" -eq 0 ]]; then
    echo "Installing (requires sudo)..."
    sudo make -C "$BUILD_DIR" install

    USER_EFFECT_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/kwin/effects/autohdr"
    if [[ -d "$USER_EFFECT_DIR" ]]; then
        echo "Removing stale user-local effect data that shadows /usr/share:"
        echo "  $USER_EFFECT_DIR"
        rm -rf "$USER_EFFECT_DIR"
    fi

    USER_PLUGIN="${XDG_DATA_HOME:-$HOME/.local}/lib/qt6/plugins/kwin/effects/plugins/kwin4_effect_autohdr.so"
    if [[ -f "$USER_PLUGIN" ]]; then
        echo "Removing stale user-local plugin that shadows /usr/lib:"
        echo "  $USER_PLUGIN"
        rm -f "$USER_PLUGIN"
    fi
fi

if [[ "$RELOAD" -eq 1 ]]; then
    echo "Reloading KWin..."
    QDBUS="$(command -v qdbus6 || command -v qdbus || true)"
    if [[ -n "$QDBUS" ]] && "$QDBUS" org.kde.KWin /KWin reconfigure; then
        echo "KWin reconfigured."
    else
        echo "Warning: failed to reload KWin (qdbus not found or not in a Plasma session?)" >&2
    fi
fi

echo "Done."
