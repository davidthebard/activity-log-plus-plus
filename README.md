# Activity Log++

A Nintendo 3DS homebrew application that lets you view, sync, and back up your Activity Log play data across multiple 3DS systems over local Wi-Fi.

The official Activity Log app only shows play history for titles installed on that device. Activity Log++ gives you a complete, combined view of your play history across all your systems.

## Features

- **Full play history viewer** — Browse all titles with playtime, launch count, session count, streak tracking, and date range
- **Local Wi-Fi sync** — Transfer and merge play data between two 3DS systems on the same network (UDP discovery + TCP transfer)
- **Backup and restore** — Create timestamped backups of your play data on the SD card (up to 10, oldest auto-pruned)
- **Rankings** — Top 10 charts for playtime, launches, sessions, and most recently played
- **Pie and bar charts** — Visual breakdown of playtime distribution across your library
- **Game icons** — Fetched from GameTDB and cached on the SD card
- **Title name lookup** — Names resolved from the system's AM service, a built-in database, and synced between devices
- **Sort and filter** — Sort by last played, playtime, launches, sessions, first played, or name; filter between games, games + system apps, or all titles

## How It Works

The 3DS Activity Log stores play data in a system save file (`pld.dat`) containing:
- A **session log** of up to 50,000 hour-granularity play sessions (title ID, timestamp, seconds played)
- A **summary table** of up to 256 per-title aggregates (total playtime, launch count, date range)

Activity Log++ reads this data from NAND (read-only), merges it with data from other systems or previous backups, and stores the combined result as `merged.dat` on the SD card. NAND is never written to.

### Sync Flow

1. One system hosts, the other connects as client (UDP broadcast discovery on the local network)
2. Session logs and summary tables are exchanged over TCP
3. Records are merged: matching sessions sum their playtime (capped at 3600s/hour), new sessions are appended
4. Title names are exchanged so both systems can display names for each other's installed titles
5. The merged result is saved to `sdmc:/3ds/activity-log-pp/merged.dat`

## Controls

| Button | Main List | Detail View | Rankings |
|--------|-----------|-------------|----------|
| Up/Down | Scroll (hold to repeat) | Scroll sessions | Scroll |
| A | Open detail view | — | Open detail view |
| B | — | Back to list | Back to list |
| X | Rankings | — | — |
| Y | Create backup | — | — |
| L/R | Cycle sort mode | — | Cycle tab |
| START | Open menu | — | — |

## Building

### Requirements

- [devkitARM](https://devkitpro.org/) (devkitPro toolchain for ARM)
- libctru, citro2d, citro3d (installed via devkitPro pacman)

### Build

```bash
make clean && make
```

Produces `activity-log-pp.3dsx` for use with a homebrew launcher.

## Installation

1. Copy `activity-log-pp.3dsx` to the `3ds/` folder on your SD card
2. Launch via the Homebrew Launcher
3. On first run with Wi-Fi available, game icons will be downloaded and cached

## File Structure on SD

```
sdmc:/3ds/activity-log-pp/
    merged.dat                          Combined play data
    title_names.dat                     Cached title names
    icons/                              Cached game icons
        {TitleID}.bin
    pld_backup_YYYYMMDD_HHMMSS.dat      Timestamped backups (up to 10)
```

## Third-Party

- [stb_image](https://github.com/nothings/stb) by Sean Barrett — public domain / MIT
- Game icons sourced from [GameTDB](https://www.gametdb.com/)

## License

This project is released into the public domain under the [Unlicense](LICENSE).
