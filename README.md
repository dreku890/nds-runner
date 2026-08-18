# Steel Vanguard

An original side-scrolling run-and-gun homebrew game for the Nintendo DS / DS Lite,
inspired by classic arcade run-and-gun games. Built in C with
[libnds](https://github.com/devkitPro/libnds) (devkitPro / devkitARM).
All sprite and background art is generated procedurally at startup, so no
external assets or converters are needed.

## Gameplay

- Start menu with Start Game, Stage Select (unlocked stages), History
  (high scores + last 8 runs), and Options (sound on/off, erase save)
- Progress, history, and options are saved to `/steel-vanguard.sav` on
  the flashcart's SD card via libfat (skipped gracefully on emulators
  without a FAT image — the menu will show "no save media")
- Side-scrolling stage (~3000 px) with enemy soldiers and aimed turrets
- Run, jump, crouch, aim up, shoot, and throw arcing grenades
- Boss tank battle at the end of the stage
- Score, lives, grenade ammo, and boss health HUD on the bottom screen

### Controls

| Button | Action |
|--------|--------|
| D-Pad left/right | Move |
| D-Pad up | Aim up |
| D-Pad down | Crouch |
| A or Y | Shoot |
| B | Jump |
| R | Throw grenade |
| START | Start / pause / restart |

## Building locally

Install [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the
`nds-dev` package group, then:

```sh
cd nds-runner
make
```

This produces `steel-vanguard.nds`, which runs on emulators (melonDS,
DeSmuME) or on a real DS Lite via a flashcart.

## Building with GitHub Actions

Push this repository to GitHub. The workflow at
`.github/workflows/build-nds.yml` builds the ROM inside the official
`devkitpro/devkitarm` Docker image on every push and uploads
`steel-vanguard.nds` as a downloadable build artifact (Actions tab →
latest run → Artifacts).

## Project layout

```
nds-runner/
├── Makefile          # devkitARM NDS makefile (produces .nds)
├── source/main.c     # entire game: art, level, entities, boss, HUD
└── README.md
```
