#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$ROOT/build"
KWIN_VERSION_STAMP="$BUILD_DIR/.kwin_version"
CHECK_VERSION="$ROOT/check-plugin-version.sh"

CLEAN=0
RECONFIGURE=0
NO_INSTALL=0
RELOAD=0
NO_VERSION_CHECK=0
NO_AUTO_CLEAN=0
JOBS="$(nproc)"

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Build and install the kwin4-effect-autohdr KWin effect.

For a full install with dependencies and an interactive KWin restart prompt,
use ../install.sh from the repository root instead.

Options:
  --clean             Delete build/ and reconfigure from scratch
  --reconfigure       Re-run cmake without deleting build/
  --no-install        Build only, skip install
  --reload            Reconfigure KWin after install (D-Bus only; see note below)
  --no-version-check  Skip post-install plugin version verification
  --no-auto-clean     Do not auto-delete build/ when system KWin version changes
  -j N                Parallel make jobs (default: nproc)
  -h, --help          Show this help

Note: --reload calls KWin reconfigure over D-Bus, which does not reload newly
installed .so files. After install, restart KWin fully (kwin_wayland --replace)
or use ../install.sh -y.
EOF
}

get_system_kwin_version() {
    if [[ -f /usr/include/kwin/config-kwin.h ]]; then
        awk -F'"' '/^#define KWIN_PLUGIN_VERSION_STRING/ { print $2; exit }' /usr/include/kwin/config-kwin.h
        return 0
    fi

    if command -v kwin_wayland >/dev/null 2>&1; then
        kwin_wayland --version 2>/dev/null | awk 'NF {print $NF; exit}'
        return 0
    fi

    if command -v kwin_x11 >/dev/null 2>&1; then
        kwin_x11 --version 2>/dev/null | awk 'NF {print $NF; exit}'
        return 0
    fi

    echo ""
}

maybe_auto_clean_for_kwin_upgrade() {
    if [[ "$NO_AUTO_CLEAN" -eq 1 || "$CLEAN" -eq 1 ]]; then
        return 0
    fi

    local system_version
    system_version="$(get_system_kwin_version)"
    if [[ -z "$system_version" ]]; then
        return 0
    fi

    if [[ ! -f "$KWIN_VERSION_STAMP" ]]; then
        return 0
    fi

    local cached_version
    cached_version="$(<"$KWIN_VERSION_STAMP")"
    if [[ "$cached_version" != "$system_version" ]]; then
        echo "System KWin changed ($cached_version -> $system_version); removing stale build cache."
        rm -rf "$BUILD_DIR"
        CLEAN=1
    fi
}

write_kwin_version_stamp() {
    local system_version
    system_version="$(get_system_kwin_version)"
    if [[ -n "$system_version" ]]; then
        printf '%s\n' "$system_version" >"$KWIN_VERSION_STAMP"
    fi
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --clean) CLEAN=1; shift ;;
        --reconfigure) RECONFIGURE=1; shift ;;
        --no-install) NO_INSTALL=1; shift ;;
        --reload) RELOAD=1; shift ;;
        --no-version-check) NO_VERSION_CHECK=1; shift ;;
        --no-auto-clean) NO_AUTO_CLEAN=1; shift ;;
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

maybe_auto_clean_for_kwin_upgrade

if [[ "$CLEAN" -eq 1 ]]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

if [[ "$CLEAN" -eq 1 || "$RECONFIGURE" -eq 1 || ! -f "$BUILD_DIR/Makefile" ]]; then
    echo "Configuring with cmake..."
    cmake -S "$ROOT" -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr
    write_kwin_version_stamp
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

    if [[ "$NO_VERSION_CHECK" -eq 0 ]]; then
        if [[ ! -x "$CHECK_VERSION" ]]; then
            echo "Warning: version check script not executable: $CHECK_VERSION" >&2
        elif ! "$CHECK_VERSION"; then
            echo "Install aborted: plugin KWin version does not match the system." >&2
            exit 1
        fi
    fi
fi

if [[ "$RELOAD" -eq 1 ]]; then
    echo "Reloading KWin (D-Bus reconfigure only)..."
    echo "Note: this does not reload newly installed effect plugins."
    echo "      Use kwin_wayland --replace or ../install.sh -y for a full restart."
    QDBUS="$(command -v qdbus6 || command -v qdbus || true)"
    if [[ -n "$QDBUS" ]] && "$QDBUS" org.kde.KWin /KWin reconfigure; then
        echo "KWin reconfigured."
    else
        echo "Warning: failed to reload KWin (qdbus not found or not in a Plasma session?)" >&2
    fi
fi

echo "Done."
