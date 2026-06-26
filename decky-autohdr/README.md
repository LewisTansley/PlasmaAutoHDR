# Decky AutoHDR

Decky Loader plugin for applying Plasma AutoHDR settings in Steam Deck gaming mode.

## Features

- Installs the `autohdr-vk` implicit Vulkan layer
- Reads and writes the shared config at `~/.config/kwin4effectautohdr`
- Provides global sliders and preset selection in gaming mode
- Generates the `~/autohdr` launch wrapper

## Build

```bash
cd decky-autohdr
pnpm install
pnpm run build
```

Copy or symlink this folder into `~/homebrew/plugins/decky-autohdr`.

## Usage

1. Install the plugin in Decky Loader.
2. Open the plugin and click **Install autohdr-vk Layer**.
3. Copy the launch option and add it to a game's Steam launch options:

```bash
~/autohdr %command%
```

4. Stack with lsfg-vk if needed:

```bash
~/autohdr ~/lsfg %command%
```

## Desktop compatibility

Calibrations created in desktop Plasma mode via the KWin effect, KCM, or `plasma-autohdr-calibrate` are reused automatically because both paths share the same config file.
