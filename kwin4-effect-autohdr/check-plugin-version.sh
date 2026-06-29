#!/usr/bin/env bash
# Verify the installed AutoHDR KWin effect plugin matches the system KWin version.
set -euo pipefail

PLUGIN_NAME="kwin4_effect_autohdr"
DEFAULT_PLUGIN_PATH="/usr/lib/qt6/plugins/kwin/effects/plugins/${PLUGIN_NAME}.so"

PLUGIN_PATH="$DEFAULT_PLUGIN_PATH"
QUIET=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS]

Compare the KWin version embedded in the installed effect plugin against the
system KWin version. Exit 0 when they match, 1 on mismatch, 2 if the plugin
is missing or versions cannot be determined.

Options:
  --plugin-path PATH  Plugin .so to inspect (default: $DEFAULT_PLUGIN_PATH)
  --quiet             Only print errors; exit with status code
  -h, --help          Show this help
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --plugin-path)
            PLUGIN_PATH="$2"
            shift 2
            ;;
        --quiet)
            QUIET=1
            shift
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

log() {
    if [[ "$QUIET" -eq 0 ]]; then
        echo "$@"
    fi
}

err() {
    echo "$@" >&2
}

get_system_kwin_version() {
    local version=""

    if command -v kwin_wayland >/dev/null 2>&1; then
        version="$(kwin_wayland --version 2>/dev/null | awk 'NF {print $NF; exit}')"
    fi

    if [[ -z "$version" ]] && command -v kwin_x11 >/dev/null 2>&1; then
        version="$(kwin_x11 --version 2>/dev/null | awk 'NF {print $NF; exit}')"
    fi

    if [[ -z "$version" ]] && [[ -f /usr/include/kwin/config-kwin.h ]]; then
        version="$(awk -F'"' '/^#define KWIN_PLUGIN_VERSION_STRING/ { print $2; exit }' /usr/include/kwin/config-kwin.h)"
    fi

    if [[ -z "$version" ]] && command -v pacman >/dev/null 2>&1; then
        version="$(pacman -Q kwin 2>/dev/null | awk '{print $2}' | cut -d- -f1)"
    fi

    if [[ -z "$version" ]] && command -v dpkg-query >/dev/null 2>&1; then
        version="$(dpkg-query -W -f='${Version}' kwin-wayland 2>/dev/null | cut -d- -f1 || true)"
        if [[ -z "$version" ]]; then
            version="$(dpkg-query -W -f='${Version}' kwin-dev 2>/dev/null | cut -d- -f1 || true)"
        fi
    fi

    if [[ -z "$version" ]] && command -v rpm >/dev/null 2>&1; then
        version="$(rpm -q --qf '%{VERSION}' kwin 2>/dev/null || true)"
    fi

    echo "$version"
}

get_plugin_kwin_version() {
    local plugin_path="$1"
    local iid=""

    if [[ ! -f "$plugin_path" ]]; then
        return 1
    fi

    if ! command -v strings >/dev/null 2>&1; then
        return 1
    fi

    iid="$(strings "$plugin_path" | rg -o 'org\.kde\.kwin\.EffectPluginFactory[0-9.]+' | head -1 || true)"
    if [[ -z "$iid" ]]; then
        return 1
    fi

    echo "${iid#org.kde.kwin.EffectPluginFactory}"
}

SYSTEM_VERSION="$(get_system_kwin_version)"
PLUGIN_VERSION="$(get_plugin_kwin_version "$PLUGIN_PATH" || true)"

if [[ ! -f "$PLUGIN_PATH" ]]; then
    err "Error: plugin not found at $PLUGIN_PATH"
    exit 2
fi

if [[ -z "$SYSTEM_VERSION" ]]; then
    err "Error: could not determine system KWin version."
    exit 2
fi

if [[ -z "$PLUGIN_VERSION" ]]; then
    err "Error: could not read KWin version from plugin IID in $PLUGIN_PATH"
    exit 2
fi

if [[ "$PLUGIN_VERSION" == "$SYSTEM_VERSION" ]]; then
    log "Plugin KWin version matches system KWin ($SYSTEM_VERSION)."
    exit 0
fi

err "Error: KWin version mismatch."
err "  System KWin:  $SYSTEM_VERSION"
err "  Plugin built: $PLUGIN_VERSION"
err ""
err "KWin effect plugins must be rebuilt against the installed KWin headers."
err "Run: ./build-install.sh --clean"
err "  or: ./install.sh --clean"
exit 1
