from __future__ import annotations

import json
import os
import shutil
import subprocess
from pathlib import Path

from shared_config import LAUNCH_OPTION, WRAPPER_PATH


class InstallationService:
    def __init__(self) -> None:
        self.home = Path.home()
        self.lib_dir = self.home / ".local/lib"
        self.layer_dir = self.home / ".local/share/vulkan/implicit_layer.d"
        self.share_dir = self.home / ".local/share/autohdr-vk"
        self.repo_root = Path(__file__).resolve().parents[3].parent
        self.vk_build = self.repo_root / "autohdr-vk" / "build"

    def is_installed(self) -> bool:
        return (self.lib_dir / "libautohdr-vk.so").is_file() and (self.home / "autohdr").is_file()

    def install_layer(self) -> dict[str, str]:
        self.lib_dir.mkdir(parents=True, exist_ok=True)
        self.layer_dir.mkdir(parents=True, exist_ok=True)
        self.share_dir.mkdir(parents=True, exist_ok=True)

        lib_candidates = [
            self.vk_build / "libautohdr-vk.so",
            self.repo_root / "autohdr-vk" / "build" / "libautohdr-vk.so",
        ]
        lib_source = next((path for path in lib_candidates if path.is_file()), None)
        if lib_source is None:
            build_script = self.repo_root / "autohdr-vk" / "CMakeLists.txt"
            if build_script.is_file():
                build_dir = self.vk_build
                build_dir.mkdir(parents=True, exist_ok=True)
                subprocess.run(["cmake", "-S", str(build_script.parent), "-B", str(build_dir)], check=True)
                subprocess.run(["cmake", "--build", str(build_dir)], check=True)
                lib_source = build_dir / "libautohdr-vk.so"
        if lib_source is None or not lib_source.is_file():
            raise FileNotFoundError("autohdr-vk shared library not found; build autohdr-vk first")

        shutil.copy2(lib_source, self.lib_dir / "libautohdr-vk.so")

        manifest_src = self.repo_root / "autohdr-vk" / "layer" / "VkLayer_AUTOHDR.json"
        manifest_dst = self.layer_dir / "VkLayer_AUTOHDR.json"
        manifest = json.loads(manifest_src.read_text(encoding="utf-8"))
        manifest["layer"]["library_path"] = "../../../lib/libautohdr-vk.so"
        manifest_dst.write_text(json.dumps(manifest, indent=4) + "\n", encoding="utf-8")

        shader_dir = lib_source.parent / "shaders"
        for name in ("fullscreen.vert.spv", "autohdr.frag.spv"):
            src = shader_dir / name
            if src.is_file():
                shutil.copy2(src, self.share_dir / name)

        self._write_wrapper()
        return {
            "library": str(self.lib_dir / "libautohdr-vk.so"),
            "manifest": str(manifest_dst),
            "wrapper": str(self.home / "autohdr"),
        }

    def uninstall_layer(self) -> None:
        for path in (
            self.lib_dir / "libautohdr-vk.so",
            self.layer_dir / "VkLayer_AUTOHDR.json",
            self.home / "autohdr",
        ):
            if path.is_file():
                path.unlink()

    def _write_wrapper(self) -> None:
        wrapper = self.home / "autohdr"
        wrapper.write_text(
            """#!/usr/bin/env bash
export ENABLE_AUTOHDR_VK=1
export ENABLE_GAMESCOPE_WSI=1
export DISABLE_VK_LAYER_VALVE_steam_fossilize_1=1
export AUTOHDR_VK_VERT_SPIRV=\"${AUTOHDR_VK_VERT_SPIRV:-$HOME/.local/share/autohdr-vk/fullscreen.vert.spv}\"
export AUTOHDR_VK_FRAG_SPIRV=\"${AUTOHDR_VK_FRAG_SPIRV:-$HOME/.local/share/autohdr-vk/autohdr.frag.spv}\"
exec \"$@\"
""",
            encoding="utf-8",
        )
        os.chmod(wrapper, 0o755)

    def get_launch_option(self) -> str:
        return LAUNCH_OPTION

    def get_wrapper_path(self) -> str:
        return os.path.expanduser(WRAPPER_PATH)
