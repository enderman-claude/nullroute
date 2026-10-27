# NULLROUTE

A hacking game for Linux and Windows, built on a fake POSIX machine.

You are **ghost**, a freelance operator with a rig above a noodle bar in the
old harbour district. A job arrives through a dead drop: crack a bakery's
point-of-sale server, follow its sync to a logistics gateway, and work your
way through a courier fleet to the evidence behind a reroute that puts a
hospital's insulin deliveries on a residential street. The story is told in
mail, logs and briefs you read through the in-game terminal and notepad.
[docs/STORY.md](docs/STORY.md) has the full arc.

The game takes no arguments. Start it, pick **New game** or **Continue**, and
play. Everything else happens inside the window.

Inspired by Hacknet's network-map loop and Uplink's trace pressure.

## What is inside the window

- **Terminal.** A POSIX-style shell with quoting, variables, `$?`, pipelines,
  redirections, `&&`/`||`/`;`, `if`/`while`/`until`/`for`, arithmetic, command
  substitution, globbing and shell scripts (`sh FILE ARGS`). It runs against a
  virtual file system, never the real one.
- **Coreutils.** The GNU coreutils subset a Linux box ships with: `cat cp cut
  date dirname echo env false head hostname id ls mkdir mv printenv printf pwd
  rm rmdir seq sleep sort tail tee test touch tr true uname uniq wc whoami
  basename chmod`. There is no grep, find or nano. Those are deliberately left
  out.
- **Hacking toolkit.** Game programs that only exist on the rig and the
  machines you have taken: `scan`, `portscan`, `crack`, `ssh`, `scp`,
  `exploit`, `notepad`, `brief`, `status`, `publish`, `sell`, `help`.
- **Notepad.** A real text editor window. `notepad FILE` opens any file on the
  current machine; Ctrl+S saves it back. There is no nano.
- **Network map.** Hosts appear as you scan them. Rooted hosts are marked.
- **Trace.** Every command on a remote machine raises the trace. At 100% the
  owner finds your rig and drops the connection.
- **Saves.** Progress saves automatically. Esc opens the pause menu, which can
  save and return to the menu.

## Controls

| Key | Action |
|---|---|
| Enter | run the command / confirm |
| Up / Down | command history (Down/Up in menus to move selection) |
| Tab | complete commands and file names |
| Ctrl+L | clear the terminal |
| Ctrl+C | cancel the current line |
| F2 | switch focus between terminal and notepad |
| Ctrl+S | save the file in the notepad |
| Esc | pause menu |

Mouse: click the menu buttons, click a panel to focus it, scroll the terminal
with the wheel.

## Playing

The first moves are always the same:

```
scan                                      find what the rig can reach
crack crumbco-pos clerk                   break the clerk's password
scp crumbco-pos:/home/clerk/cashdrawer.log ~/loot/
```

`scp` copies run from the rig, so `exit` back to the rig before copying. The
chapter brief (`brief`) lists the next steps; the story is in the mail
(`mail`-style: `cat ~/mail/001.txt`, or `notepad ~/mail/001.txt`).

Chapters complete when their file arrives in `~/loot`. After the last one you
choose between `publish` and `sell`.

## Building

You need CMake 3.16 or newer and a C compiler. raylib 5.5 is fetched and
built automatically.

On Linux, install the windowing headers first (Debian/Ubuntu):

```sh
sudo apt-get install libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/nullroute
```

On Windows, the release archives contain a ready-to-run `nullroute.exe`. To
build it yourself, use a MinGW-w64 cross-compiler from Linux:

```sh
cmake -S . -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-win
```

The game needs `assets/` next to the executable. The build copies it there.

## Tests

The core has no dependency on the window, so its tests run headless:

```sh
ctest --test-dir build --output-on-failure
```

They cover path normalisation, the virtual file system, the shell language,
the coreutils, the hacking mechanics, the trace, a full playthrough to the
ending, and save and load.

## Layout

| Path | Contents |
|---|---|
| `src/core/` | the fake machine: VFS, shell, coreutils, tools, game state, world loader. Plain C99, no dependencies |
| `src/gui/` | the window: menus, screens, terminal, notepad and network map on raylib |
| `assets/world.txt` | hosts, file systems, missions, story text and the wordlist, in plain text |
| `assets/fonts/` | DejaVu Sans Mono (see its licence file) |
| `tests/test_core.c` | the core test suite |
| `cmake/` | MinGW-w64 cross-compile toolchain |
| `docs/STORY.md` | the story |

To add a host or a chapter, edit `assets/world.txt`; the format is described
at the top of `src/core/world.c`.

## Limits

- The shell has no subshells `( )`, background jobs `&`, here-documents, case
  or functions. Those are reported as syntax errors.
- Passwords are hashed with FNV-1a. It is not secure, and it is not meant to
  be: the wordlist attack is the game.
- The Windows build is a console-subsystem executable, so a console window
  appears behind the game.

## Licence

MIT, see [LICENSE](LICENSE). raylib is zlib-licensed. DejaVu Sans Mono is
licensed under the Bitstream Vera licence, see `assets/fonts/DejaVu-LICENSE.txt`.
