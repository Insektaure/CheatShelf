# CheatShelf

Ultrahand overlay that loads Atmosphere cheats from any number of `.txt` files, while the game runs.

Atmosphere only reads one cheat file per game build and holds at most 127 cheats. CheatShelf keeps as many files as you want on the SD card and adds or removes individual cheats in dmnt on demand, so a game with hundreds of codes (Pokémon Legends Z-A item codes, for example) can be used without merging files by hand.

It talks to dmnt through `dmnt:cht` (`AddCheat`, `RemoveCheat`, `ToggleCheat`, `SetMasterCheat`). No sysmodule and no Atmosphere changes.

## Install

1. Atmosphere with cheats enabled, [nx-ovlloader](https://github.com/ppkantorski/nx-ovlloader) and [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay).
2. Copy `out/switch/.overlays/cheatshelf.ovl` to `sdmc:/switch/.overlays/`.
3. Put cheat files in `sdmc:/config/cheatshelf/cheats/<game name> [<title id>]/<build id>/`. Only the `[<title id>]` at the end counts, the name is yours to pick (a folder named just `<title id>` works too). `config/cheatshelf/` itself also holds the overlay's UI settings. Subfolders are allowed and show up as folders.

```
sdmc:/config/cheatshelf/cheats/Pokemon Legends Z-A [0100F43008C44000]/B1F12FD919EAE86A/
├── items/
│   ├── berries.txt
│   ├── mega_stones.txt
│   └── tm_items.txt
└── pokemon_encounter_codes.txt
```

The overlay shows the running game's build ID in its header. With no folder for the game yet, it shows the path to create, named after the game (`FINAL FANTASY RESONANCE [0100606026F12000]/84E182F72EE1DA4F/`), and **Create this folder** makes it.

Set `dmnt_cheats_enabled_by_default = u8!0x0` in `atmosphere/config/system_settings.ini` so cheats from Atmosphere's own `<build id>.txt` do not all start on.

## Use

| View | A | Y |
|---|---|---|
| File | load / unload the cheat (loads it off) | turn on / off (loads it if needed) |
| Loaded cheats | turn on / off | unload / load again |

- **Load all** / **Unload all** at the top of a file or section act on that file or section only.
- **Loaded cheats** lists everything dmnt holds, including cheats from Atmosphere's `.txt` and from other tools.
- **Atmosphere cheat file** opens the game's `atmosphere/contents/<title id>/cheats/<build id>.txt` like any other file, to load its cheats again after unloading them (**Load all**, or one by one). It is the only file whose `{Master}` can be set from CheatShelf (A on **Master: ...**), for when the master code differs or is gone.
- `{Master}` codes in a file are not loaded, see [below](#with-another-debugger).
- Folders: EdiZon-Overlay sections (`[--SectionStart:Name--]` ... `[--SectionEnd:Name--]`) and Breeze/EdiZon-SE markers (`[Name]` `20000000` ... `[Name end]` `20000001`), in files and in **Loaded cheats** (Atmosphere loads markers as cheats, so the folders of its file are kept). A start without a matching end (a credit line) is shown as text.
- **Load all** loads the file, or the folder, in file order with its markers, so its folders show in **Loaded cheats** too. **Unload all** removes the markers as well.
- **Unload the ones that are off** also removes the markers of every folder left with no cheat on inside it (subfolders included); folders that still hold an active cheat stay.
- Text that is not cheat syntax (readme lines at the top of a file) is skipped instead of rejecting the file like Atmosphere does. A cheat with a malformed code line is dropped whole.

Values next to an item: `ON`, `OFF` (loaded), nothing (not loaded), or why an action failed:

| Value | Meaning |
|---|---|
| `Full` | 127 cheats already loaded. Unload some first. |
| `Too big` | Turning it on would push the code of all enabled cheats past 1024 dwords. dmnt would then silently run none of them, so CheatShelf refuses. |
| `Err XXXX` | dmnt returned an error code. |

Loaded cheats live in dmnt until the game closes. Atmosphere does not save them back to the `.txt`.

## With another debugger

The kernel allows one debugger per game. dmnt attaches at launch only to games that have an Atmosphere cheat file (`atmosphere/contents/<title id>/cheats/<build id>.txt`), so for any other game a debugger that attaches later locks dmnt out, and CheatShelf shows *dmnt can't attach to the game* (with the game's folder and **Create this folder** still there: pm and the loader identify the game without dmnt).

Keep a normal Atmosphere cheat file for every game you use with CheatShelf: dmnt then holds the game from launch, the other debugger is the one left out (it can only read the game through dmnt:cht, if it supports that), and CheatShelf adds cheats from its own files or unloads the Atmosphere ones as needed.

- Atmosphere rejects the whole file on any line that is not cheat syntax (readme text, for example), and then does not attach. Keep that file clean; CheatShelf's files can be messy.
- The master code (`{...}`) is the one from the Atmosphere file, loaded at launch. CheatShelf only sets it from the Atmosphere file itself; a `{...}` in its own files shows as **Master: ...** with `matches`, `DIFFERENT` or `NOT LOADED`. Cheats written for another master code can hook into code it never installed, so only load those when it matches.
- The overlay gives its dmnt:cht session back while hidden: dmnt:cht has two, and another tool may keep one while it reads through dmnt.

## Build

Needs devkitPro (devkitA64, libnx, switch-curl, switch-zlib, switch-minizip, switch-mbedtls), or Docker:

```sh
git submodule update --init
docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src devkitpro/devkita64:20260219 make
```

Output: `out/switch/.overlays/cheatshelf.ovl`.

`libs/dmntcht` is the `dmnt:cht` client from Atmosphere (`libraries/libstratosphere/source/dmnt`), GPLv2.

## Credits

- [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) (Atmosphère-NX): the cheat engine CheatShelf drives, and the `dmnt:cht` client in `libs/dmntcht`. GPLv2.
- [libultrahand](https://github.com/ppkantorski/libultrahand) (ppkantorski), built on [libtesla](https://github.com/WerWolv/libtesla) (WerWolv): the overlay UI. GPLv2.
- [libnx](https://github.com/switchbrew/libnx) (switchbrew) and [devkitPro](https://devkitpro.org): the toolchain and system library.
- [sphaira](https://github.com/ITotalJustice/sphaira) (ITotalJustice and contributors): how compressed game names (HOS 21.0.0+) are read.
- [EdiZon-Overlay](https://github.com/proferabg/EdiZon-Overlay) (WerWolv, proferabg, ppkantorski) and [Breeze](https://github.com/tomvita/Breeze-Beta) / EdiZon-SE (tomvita): the folder conventions in cheat files.
- [zlib](https://zlib.net) (Jean-loup Gailly, Mark Adler).
- The cheat makers whose files this is meant to load.

## License

GPLv2, see [LICENSE](LICENSE). CheatShelf includes GPLv2 code (libultrahand, libtesla, Atmosphère's `dmnt:cht` client), so it is distributed under the same license.

## Disclaimer

- CheatShelf is an unofficial homebrew tool, not affiliated with or endorsed by Nintendo, the Atmosphère project or any game publisher. Game names and trademarks belong to their owners.
- It ships no cheats. Cheats change a game's memory while it runs: they can crash the game, corrupt or ruin save data, or break progress. Back up your saves first.
- Do not use cheats online. Modified consoles and games can be detected, and accounts or consoles banned.
- Use it at your own risk. It is provided as is, without any warranty; the author is not responsible for any damage, data loss or ban.
