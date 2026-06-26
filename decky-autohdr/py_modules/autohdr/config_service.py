from __future__ import annotations

import configparser
import os
from pathlib import Path
from typing import Any

import sys
from pathlib import Path

PLUGIN_ROOT = Path(__file__).resolve().parents[2]
if str(PLUGIN_ROOT) not in sys.path:
    sys.path.insert(0, str(PLUGIN_ROOT))

from shared_config import CALIBRATION_FIELDS, field_defaults


class ConfigurationService:
    def __init__(self) -> None:
        self.config_path = Path(os.path.expanduser("~/.config/kwin4effectautohdr"))

    def _ensure_config(self) -> configparser.ConfigParser:
        parser = configparser.ConfigParser()
        if self.config_path.is_file():
            parser.read(self.config_path)
        if "Settings" not in parser:
            parser["Settings"] = {}
        return parser

    def _write(self, parser: configparser.ConfigParser) -> None:
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        with self.config_path.open("w", encoding="utf-8") as handle:
            parser.write(handle)

    def get_global_settings(self) -> dict[str, Any]:
        parser = self._ensure_config()
        settings = field_defaults()
        section = parser["Settings"]
        for field in CALIBRATION_FIELDS:
            if field.key in section:
                raw = section[field.key]
                if field.field_type == "int":
                    settings[field.key] = int(float(raw))
                elif field.field_type == "float":
                    settings[field.key] = float(raw)
                else:
                    settings[field.key] = raw
        if "ToneCurveUserPresetId" in section:
            settings["ToneCurveUserPresetId"] = section["ToneCurveUserPresetId"]
        return settings

    def update_global_settings(self, values: dict[str, Any]) -> dict[str, Any]:
        parser = self._ensure_config()
        section = parser["Settings"]
        for key, value in values.items():
            section[key] = str(value)
        self._write(parser)
        return self.get_global_settings()

    def list_app_profiles(self) -> list[dict[str, str]]:
        parser = self._ensure_config()
        if "Applications" not in parser or "AppList" not in parser["Applications"]:
            return []
        keys = [item.strip() for item in parser["Applications"]["AppList"].split(",") if item.strip()]
        profiles: list[dict[str, str]] = []
        for key in keys:
            group = f"App {key}"
            if group not in parser:
                continue
            section = parser[group]
            profiles.append(
                {
                    "key": key,
                    "displayName": section.get("DisplayName", key),
                    "steamAppId": section.get("SteamAppId", ""),
                }
            )
        return profiles

    def get_app_settings(self, app_key: str) -> dict[str, Any]:
        parser = self._ensure_config()
        group = f"App {app_key}"
        if group not in parser:
            return self.get_global_settings()
        settings = field_defaults()
        section = parser[group]
        for field in CALIBRATION_FIELDS:
            if field.key in section:
                raw = section[field.key]
                if field.field_type == "int":
                    settings[field.key] = int(float(raw))
                elif field.field_type == "float":
                    settings[field.key] = float(raw)
                else:
                    settings[field.key] = raw
        if "ToneCurveUserPresetId" in section:
            settings["ToneCurveUserPresetId"] = section["ToneCurveUserPresetId"]
        return settings

    def update_app_settings(self, app_key: str, values: dict[str, Any]) -> dict[str, Any]:
        parser = self._ensure_config()
        apps = parser.setdefault("Applications", {})
        app_list = [item.strip() for item in apps.get("AppList", "").split(",") if item.strip()]
        if app_key not in app_list:
            app_list.append(app_key)
            apps["AppList"] = ",".join(app_list)

        group = f"App {app_key}"
        if group not in parser:
            parser[group] = {}
        section = parser[group]
        if "DisplayName" not in section:
            section["DisplayName"] = app_key
        for key, value in values.items():
            section[key] = str(value)
        self._write(parser)
        return self.get_app_settings(app_key)

    def list_user_presets(self) -> list[dict[str, str]]:
        parser = self._ensure_config()
        if "UserPresets" not in parser or "PresetList" not in parser["UserPresets"]:
            return []
        presets = []
        for preset_id in parser["UserPresets"]["PresetList"].split(","):
            preset_id = preset_id.strip()
            if not preset_id:
                continue
            group = f"Preset {preset_id}"
            display = parser[group].get("DisplayName", preset_id) if group in parser else preset_id
            presets.append({"id": preset_id, "displayName": display})
        return presets

    def apply_user_preset(self, preset_id: str, app_key: str | None = None) -> dict[str, Any]:
        values = {
            "ToneCurvePreset": "user",
            "ToneCurveUserPresetId": preset_id,
        }
        if app_key:
            return self.update_app_settings(app_key, values)
        return self.update_global_settings(values)

    def steam_app_key(self, steam_app_id: str) -> str:
        return f"steam_app_{steam_app_id.strip()}"
