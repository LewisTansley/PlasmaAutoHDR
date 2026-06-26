#!/usr/bin/env bash
set -euo pipefail

PREFIX="${AUTOHDR_VK_PREFIX:-$HOME/.local}"
LIB_DIR="$PREFIX/lib"
LAYER_DIR="$PREFIX/share/vulkan/implicit_layer.d"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${AUTOHDR_VK_BUILD_DIR:-$SCRIPT_DIR/../build}"

mkdir -p "$LIB_DIR" "$LAYER_DIR"

if [[ -f "$BUILD_DIR/libautohdr-vk.so" ]]; then
  install -m 755 "$BUILD_DIR/libautohdr-vk.so" "$LIB_DIR/libautohdr-vk.so"
elif [[ -f "$BUILD_DIR/libautohdr_vk.so" ]]; then
  install -m 755 "$BUILD_DIR/libautohdr_vk.so" "$LIB_DIR/libautohdr-vk.so"
else
  echo "autohdr-vk shared library not found in $BUILD_DIR" >&2
  exit 1
fi

install -m 644 "$SCRIPT_DIR/../layer/VkLayer_AUTOHDR.json" "$LAYER_DIR/VkLayer_AUTOHDR.json"
python3 - <<'PY'
import json
from pathlib import Path
manifest = Path.home() / ".local/share/vulkan/implicit_layer.d/VkLayer_AUTOHDR.json"
data = json.loads(manifest.read_text())
data["layer"]["library_path"] = "../../../lib/libautohdr-vk.so"
manifest.write_text(json.dumps(data, indent=4) + "\n")
PY

WRAPPER="$HOME/autohdr"
cat > "$WRAPPER" <<'EOF'
#!/usr/bin/env bash
export ENABLE_AUTOHDR_VK=1
export ENABLE_GAMESCOPE_WSI=1
export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
export AUTOHDR_VK_VERT_SPIRV="${AUTOHDR_VK_VERT_SPIRV:-$HOME/.local/share/autohdr-vk/fullscreen.vert.spv}"
export AUTOHDR_VK_FRAG_SPIRV="${AUTOHDR_VK_FRAG_SPIRV:-$HOME/.local/share/autohdr-vk/autohdr.frag.spv}"
exec "$@"
EOF
chmod +x "$WRAPPER"

SHARE_DIR="$HOME/.local/share/autohdr-vk"
mkdir -p "$SHARE_DIR"
if [[ -f "$BUILD_DIR/shaders/fullscreen.vert.spv" ]]; then
  install -m 644 "$BUILD_DIR/shaders/fullscreen.vert.spv" "$SHARE_DIR/fullscreen.vert.spv"
  install -m 644 "$BUILD_DIR/shaders/autohdr.frag.spv" "$SHARE_DIR/autohdr.frag.spv"
fi

echo "Installed autohdr-vk to $LIB_DIR"
echo "Launch wrapper: $WRAPPER %command%"
