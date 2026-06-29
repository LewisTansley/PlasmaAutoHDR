#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EFFECT_DIR="$ROOT/kwin4-effect-autohdr"
BUILD_INSTALL="$EFFECT_DIR/build-install.sh"
CHECK_VERSION="$EFFECT_DIR/check-plugin-version.sh"

SKIP_DEPS=0
YES_RESTART=0
NO_RESTART=0
BUILD_ARGS=()

usage() {
    cat <<EOF
Usage: $(basename "$0") [OPTIONS] [BUILD OPTIONS]

Install PlasmaAutoHDR: system dependencies, build, and install the KWin effect.

Options:
  --skip-deps       Skip package-manager dependency installation
  -y, --yes-restart Restart KWin after install without prompting
  -n, --no-restart  Do not offer to restart KWin after install
  -h, --help        Show this help

Build options (passed to build-install.sh):
  --clean           Delete build/ and reconfigure from scratch
  --reconfigure     Re-run cmake without deleting build/
  --no-install      Build only, skip install
  --no-auto-clean   Do not auto-delete build/ when KWin version changes
  --no-version-check Skip post-install plugin version verification
  -j N              Parallel make jobs (default: nproc)
EOF
}

verify_plugin_version() {
    if [[ ! -x "$CHECK_VERSION" ]]; then
        echo "Warning: version check script not found: $CHECK_VERSION" >&2
        return 0
    fi

    if "$CHECK_VERSION"; then
        return 0
    fi

    cat <<EOF >&2

The installed plugin does not match your system KWin version.
AutoHDR will not appear in Desktop Effects until you rebuild against the
current KWin headers.

Re-run with --clean, for example:
  ./install.sh --skip-deps --clean -y

EOF
    return 1
}

kwin_session_available() {
    local qdbus
    qdbus="$(command -v qdbus6 || command -v qdbus || true)"
    [[ -n "$qdbus" ]] && "$qdbus" org.kde.KWin /KWin org.freedesktop.DBus.Introspectable.Introspect &>/dev/null
}

detect_distro_family() {
    if [[ ! -f /etc/os-release ]]; then
        echo unknown
        return
    fi

    # shellcheck disable=SC1091
    source /etc/os-release

    case "${ID:-}" in
        arch|cachyos|manjaro|endeavouros|garuda)
            echo arch
            return
            ;;
        debian|ubuntu|linuxmint|pop|elementary|zorin)
            echo debian
            return
            ;;
        fedora|rhel|centos|rocky|almalinux)
            echo fedora
            return
            ;;
    esac

    if [[ "${ID_LIKE:-}" == *arch* ]]; then
        echo arch
    elif [[ "${ID_LIKE:-}" == *debian* ]] || [[ "${ID_LIKE:-}" == *ubuntu* ]]; then
        echo debian
    elif [[ "${ID_LIKE:-}" == *fedora* ]] || [[ "${ID_LIKE:-}" == *rhel* ]]; then
        echo fedora
    else
        echo unknown
    fi
}

install_dependencies() {
    local family
    family="$(detect_distro_family)"

    case "$family" in
        arch)
            if ! command -v pacman >/dev/null 2>&1; then
                echo "Error: pacman not found but distro looks like Arch." >&2
                exit 1
            fi
            echo "Installing dependencies via pacman..."
            sudo pacman -S --needed --noconfirm \
                base-devel cmake extra-cmake-modules \
                qt6-base qt6-tools kwin \
                kconfig kconfigwidgets kcmutils kcoreaddons kglobalaccel ki18n \
                pyside6
            ;;
        debian)
            if ! command -v apt-get >/dev/null 2>&1; then
                echo "Error: apt-get not found but distro looks like Debian/Ubuntu." >&2
                exit 1
            fi
            echo "Installing dependencies via apt..."
            sudo apt-get update
            sudo apt-get install -y \
                build-essential cmake extra-cmake-modules \
                qt6-base-dev qt6-base-dev-tools qt6-tools-dev kwin-dev \
                libkf6config-dev libkf6configwidgets-dev libkf6kcmutils-dev \
                libkf6coreaddons-dev libkf6globalaccel-dev libkf6i18n-dev \
                python3-pyside6.qtwidgets
            ;;
        fedora)
            if ! command -v dnf >/dev/null 2>&1; then
                echo "Error: dnf not found but distro looks like Fedora/RHEL." >&2
                exit 1
            fi
            echo "Installing dependencies via dnf..."
            sudo dnf install -y \
                cmake gcc-c++ extra-cmake-modules \
                qt6-qtbase-devel qt6-qttools-devel kwin-devel \
                kf6-kconfig-devel kf6-kconfigwidgets-devel kf6-kcmutils-devel \
                kf6-kcoreaddons-devel kf6-kglobalaccel-devel kf6-ki18n-devel \
                python3-pyside6
            ;;
        unknown)
            cat <<EOF
Unsupported or unrecognized Linux distribution.

Install these packages manually, then re-run with --skip-deps:

Arch / CachyOS / Manjaro:
  base-devel cmake extra-cmake-modules qt6-base qt6-tools kwin \\
  kconfig kconfigwidgets kcmutils kcoreaddons kglobalaccel ki18n pyside6

Debian / Ubuntu / Mint:
  build-essential cmake extra-cmake-modules qt6-base-dev qt6-base-dev-tools \\
  qt6-tools-dev kwin-dev libkf6config-dev libkf6configwidgets-dev \\
  libkf6kcmutils-dev libkf6coreaddons-dev libkf6globalaccel-dev \\
  libkf6i18n-dev python3-pyside6.qtwidgets

Fedora / RHEL:
  cmake gcc-c++ extra-cmake-modules qt6-qtbase-devel qt6-qttools-devel \\
  kwin-devel kf6-kconfig-devel kf6-kconfigwidgets-devel kf6-kcmutils-devel \\
  kf6-kcoreaddons-devel kf6-kglobalaccel-devel kf6-ki18n-devel python3-pyside6
EOF
            read -r -p "Continue without installing dependencies? [y/N] " reply
            case "${reply,,}" in
                y|yes) ;;
                *)
                    echo "Aborted."
                    exit 1
                    ;;
            esac
            ;;
    esac
}

refresh_kde_caches() {
    if command -v kbuildsycoca6 >/dev/null 2>&1; then
        echo "Refreshing KDE service cache..."
        kbuildsycoca6 --noincremental || echo "Warning: kbuildsycoca6 failed (non-fatal)." >&2
    fi
}

restart_kwin() {
    local kwin_bin=""

    if [[ "${XDG_SESSION_TYPE:-}" == "wayland" ]] || pgrep -x kwin_wayland >/dev/null 2>&1; then
        kwin_bin="$(command -v kwin_wayland || true)"
    elif [[ "${XDG_SESSION_TYPE:-}" == "x11" ]] || pgrep -x kwin_x11 >/dev/null 2>&1; then
        kwin_bin="$(command -v kwin_x11 || true)"
    else
        kwin_bin="$(command -v kwin_wayland || command -v kwin_x11 || true)"
    fi

    if [[ -n "$kwin_bin" ]]; then
        echo "Restarting KWin ($kwin_bin --replace)..."
        echo "Your windows should stay open; the screen may flicker briefly."
        nohup "$kwin_bin" --replace >/dev/null 2>&1 &
        disown
        return 0
    fi

    local qdbus
    qdbus="$(command -v qdbus6 || command -v qdbus || true)"
    if [[ -n "$qdbus" ]] && "$qdbus" org.kde.KWin /KWin reconfigure; then
        echo "KWin reconfigured via D-Bus (full restart was not available)."
        echo "If the effect does not appear, log out and back in."
        return 0
    fi

    echo "Warning: could not restart or reconfigure KWin." >&2
    echo "Log out and back in to load the new effect." >&2
    return 1
}

prompt_restart_kwin() {
    if [[ "$NO_RESTART" -eq 1 ]]; then
        return 0
    fi

    if [[ "$YES_RESTART" -eq 1 ]]; then
        restart_kwin
        return 0
    fi

    read -r -p "Restart KWin now to load the new effect? [y/N] " reply
    case "${reply,,}" in
        y|yes)
            restart_kwin
            ;;
        *)
            echo "Skipped KWin restart."
            ;;
    esac
}

print_post_install_instructions() {
    cat <<EOF

Installation complete.

Next steps:
  1. Enable AutoHDR in System Settings → Desktop Effects
  2. Optional shortcuts:
       Meta+Shift+H  Toggle AutoHDR for the active window
       Meta+Ctrl+H   Open the AutoHDR calibration engine

EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --skip-deps) SKIP_DEPS=1; shift ;;
        -y|--yes-restart) YES_RESTART=1; shift ;;
        -n|--no-restart) NO_RESTART=1; shift ;;
        --clean|--reconfigure|--no-install|--no-auto-clean|--no-version-check)
            BUILD_ARGS+=("$1")
            shift
            ;;
        -j)
            BUILD_ARGS+=("$1" "$2")
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

if [[ ! -f "$EFFECT_DIR/CMakeLists.txt" ]]; then
    echo "Error: expected effect sources at $EFFECT_DIR" >&2
    exit 1
fi

if [[ ! -x "$BUILD_INSTALL" ]]; then
    echo "Error: build script not found or not executable: $BUILD_INSTALL" >&2
    exit 1
fi

if ! kwin_session_available; then
    echo "Warning: KWin does not appear to be running in this session." >&2
    echo "         Installation can continue, but restarting KWin may require a Plasma session." >&2
fi

if [[ "$SKIP_DEPS" -eq 0 ]]; then
    install_dependencies
else
    echo "Skipping dependency installation (--skip-deps)."
fi

echo "Building and installing the KWin effect..."
"$BUILD_INSTALL" "${BUILD_ARGS[@]}"

if ! verify_plugin_version; then
    exit 1
fi

refresh_kde_caches
prompt_restart_kwin
print_post_install_instructions
