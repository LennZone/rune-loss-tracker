# Rune Loss Tracker

A HUD overlay mod for Elden Ring that tracks your **permanently lost runes** and **total deaths** — displayed directly above the vanilla rune counter in the same style as the base game UI.

## What it does

Every time you die without recovering your bloodstain, those runes are gone forever. This mod keeps a running total so you always know the true cost of your journey.

```
[✦]   1.337
[☠]      42
```

Two panels sit directly above the vanilla rune counter. They only appear when your character is loaded into the world — never in the main menu or character select screen. Progress is saved per character slot, so every character is tracked independently.

## Requirements

- [EldenModLoader](https://www.nexusmods.com/eldenring/mods/117) (place `dinput8.dll` in your game folder)

## Installation

1. Download `RuneLossTracker.zip` and extract it
2. Copy `RuneLossTracker.dll` directly into your `mods/` folder (no subfolder)
3. Launch the game — done

Your mod folder should look like this:

```
mods/
└── RuneLossTracker.dll
```

A `RuneLossTracker.ini` config file and per-character save files are created automatically on first launch in the same `mods/` folder.

## Configuration

Edit `RuneLossTracker.ini` in your `mods/` folder (created on first launch):

```ini
; Show the death counter panel above the lost-runes panel.
show_death_overlay=true

; Write a log file for debugging (RuneLossTracker.log).
log=false
```

Changes take effect on next game launch.

## Save files

Progress is saved automatically per character slot in your `mods/` folder:
```
mods/RuneLossTracker_slot0_save.txt
mods/RuneLossTracker_slot1_save.txt
...
```

If you start a new character in an existing slot, the tracker detects this automatically and resets.

## Compatibility

- Works alongside **Seamless Co-op** (launch via `ersc_launcher.exe`)
- **Offline only** — disable Easy Anti-Cheat before using any mods
- Does not modify any game files

## How it works

The mod runs as a DLL loaded by EldenModLoader. It reads game memory directly (in-process, no external tool needed) and hooks the D3D12 Present call to render the overlay via Dear ImGui — the same approach used by most Elden Ring HUD mods.

Bloodstain recovery is detected directly from game memory: the game itself signals when a bloodstain is picked up, so there are no false positives from enemy drops.
