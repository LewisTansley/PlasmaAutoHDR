#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PASS=0
FAIL=0

pass() { echo "[PASS] $1"; PASS=$((PASS + 1)); }
fail() { echo "[FAIL] $1"; FAIL=$((FAIL + 1)); }

echo "=== AutoHDR Steam Deck integration checks ==="

if [[ -f "$ROOT/autohdr-core/build/libautohdr_core.a" || -f "$ROOT/autohdr-core/build/lib/libautohdr_core.a" ]]; then
  pass "autohdr-core built"
else
  fail "autohdr-core not built"
fi

if [[ -f "$ROOT/autohdr-vk/build/libautohdr-vk.so" ]]; then
  pass "autohdr-vk layer built"
else
  fail "autohdr-vk layer not built"
fi

if [[ -f "$ROOT/autohdr-vk/build/shaders/autohdr.frag.spv" ]]; then
  pass "autohdr-vk shaders compiled"
else
  fail "autohdr-vk shaders missing"
fi

if [[ -f "$ROOT/autohdr-vk/layer/VkLayer_AUTOHDR.json" ]]; then
  pass "Vulkan layer manifest present"
else
  fail "Vulkan layer manifest missing"
fi

if python3 - <<'PY' "$ROOT/decky-autohdr"
import importlib.util
import sys
from pathlib import Path
root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("shared_config", root / "shared_config.py")
mod = importlib.util.module_from_spec(spec)
sys.modules["shared_config"] = mod
spec.loader.exec_module(mod)
assert mod.LAUNCH_OPTION == "~/autohdr %command%"
print("ok")
PY
then
  pass "Decky shared config valid"
else
  fail "Decky shared config invalid"
fi

if python3 - <<'PY' "$ROOT/decky-autohdr"
import importlib.util
import sys
import tempfile
from pathlib import Path

root = Path(sys.argv[1])
spec = importlib.util.spec_from_file_location("shared_config", root / "shared_config.py")
shared = importlib.util.module_from_spec(spec)
sys.modules["shared_config"] = shared
spec.loader.exec_module(shared)

spec = importlib.util.spec_from_file_location("config_service", root / "py_modules/autohdr/config_service.py")
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)
svc = mod.ConfigurationService()
with tempfile.TemporaryDirectory() as tmp:
    svc.config_path = Path(tmp) / "kwin4effectautohdr"
    svc.update_global_settings({"ReferenceNits": 203, "MaxNits": 1000, "ToneCurvePreset": "scurve"})
    data = svc.get_global_settings()
    assert data["ToneCurvePreset"] == "scurve"
print("ok")
PY
then
  pass "Decky config service read/write"
else
  fail "Decky config service read/write"
fi

if grep -q "colorMode == 1" "$ROOT/autohdr-vk/shaders/autohdr.frag"; then
  pass "HDR scRGB shader path present"
else
  fail "HDR scRGB shader path missing"
fi

if grep -q "colorMode == 2" "$ROOT/autohdr-vk/shaders/autohdr.frag"; then
  pass "HDR PQ shader path present"
else
  fail "HDR PQ shader path missing"
fi

if grep -q "SteamAppId" "$ROOT/autohdr-core/src/config.cpp"; then
  pass "SteamAppId profile resolution in autohdr-core"
else
  fail "SteamAppId profile resolution missing"
fi

echo
echo "Results: $PASS passed, $FAIL failed"
if [[ "$FAIL" -gt 0 ]]; then
  exit 1
fi

echo
echo "Manual Steam Deck checks:"
echo "  1. Install decky-autohdr and run Install autohdr-vk Layer"
echo "  2. Set launch option: ~/autohdr %command%"
echo "  3. Verify desktop-calibrated steam_app_* profile is used via SteamAppId"
echo "  4. Stack test: ~/autohdr ~/lsfg %command%"
echo "  5. Flatpak Steam: grant ~/.config/kwin4effectautohdr and ~/.local/lib access"
