import os
import sys

plugin_dir = os.path.dirname(os.path.abspath(__file__))
if plugin_dir not in sys.path:
    sys.path.insert(0, plugin_dir)

import logging

import decky

from py_modules.autohdr.config_service import ConfigurationService
from py_modules.autohdr.install_service import InstallationService

logger = logging.getLogger(__name__)

config_service = ConfigurationService()
install_service = InstallationService()


class Plugin:
    async def _main(self):
        logger.info("Decky AutoHDR loaded")

    async def _unload(self):
        logger.info("Decky AutoHDR unloaded")

    async def _uninstall(self):
        install_service.uninstall_layer()

    async def install_autohdr_vk(self):
        return install_service.install_layer()

    async def is_autohdr_installed(self):
        return install_service.is_installed()

    async def get_launch_option(self):
        return install_service.get_launch_option()

    async def get_global_settings(self):
        return config_service.get_global_settings()

    async def update_global_settings(self, values: dict):
        return config_service.update_global_settings(values)

    async def list_app_profiles(self):
        return config_service.list_app_profiles()

    async def get_app_settings(self, app_key: str):
        return config_service.get_app_settings(app_key)

    async def update_app_settings(self, app_key: str, values: dict):
        return config_service.update_app_settings(app_key, values)

    async def list_user_presets(self):
        return config_service.list_user_presets()

    async def apply_user_preset(self, preset_id: str, app_key: str | None = None):
        return config_service.apply_user_preset(preset_id, app_key)

    async def steam_app_key(self, steam_app_id: str):
        return config_service.steam_app_key(steam_app_id)

    async def log_info(self, info):
        logger.info(info)
        decky.logger.info(str(info))
