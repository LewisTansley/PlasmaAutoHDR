# PlasmaAutoHDR Monorepo

This repository contains the desktop KWin effect and the Steam Deck gaming mode port.

## Packages

| Directory | Description |
|-----------|-------------|
| [kwin4-effect-autohdr](kwin4-effect-autohdr/) | KWin compositor effect for Plasma desktop mode |
| [autohdr-core](autohdr-core/) | Shared tone-curve and config backend |
| [autohdr-vk](autohdr-vk/) | Implicit Vulkan layer for gaming mode |
| [decky-autohdr](decky-autohdr/) | Decky Loader plugin installer and settings UI |

## Shared configuration

All components read and write `~/.config/kwin4effectautohdr`.

Desktop calibrations, user presets, and per-app profiles created in Plasma desktop mode are reused automatically in gaming mode.

## Build

```bash
# Shared core
cmake -S autohdr-core -B autohdr-core/build
cmake --build autohdr-core/build

# Desktop effect
cmake -S kwin4-effect-autohdr -B kwin4-effect-autohdr/build
cmake --build kwin4-effect-autohdr/build

# Gaming mode Vulkan layer
cmake -S autohdr-vk -B autohdr-vk/build
cmake --build autohdr-vk/build
./autohdr-vk/scripts/install-layer.sh

# Decky plugin frontend
cd decky-autohdr && pnpm install && pnpm run build
```

## Gaming mode launch option

```bash
~/autohdr %command%
```

With lsfg-vk:

```bash
~/autohdr ~/lsfg %command%
```

## Integration tests

```bash
./scripts/test-steam-deck-integration.sh
```
