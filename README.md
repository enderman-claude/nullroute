# NULLROUTE

A terminal hacking game in **POSIX sh**. You are a freelance operator with a
rig, a wordlist and a dead-drop job. Crack passwords, scan the network,
exploit a fleet controller, read the audit trail, and decide what to do with
the evidence at the end.

Inspired by Hacknet's network-map loop and Uplink's trace pressure. The story is in [docs/STORY.md](docs/STORY.md).

## Play

```sh
# Any POSIX shell works. Linux and macOS:
sh game/bin/nullroute

# Or with the native launcher (Linux, Windows):
./nullroute          # Linux
nullroute.exe        # Windows, needs Git for Windows or MSYS2 (sh.exe on PATH)
```

Options:

```
-n        start a new game (erases progress)
-s DIR    keep save data in DIR (default ~/.local/state/nullroute)
-h        help
-V        version
```

Type `objectives` for the brief and `help` for the command list. The first
moves are always the same: `scan`, then `crack crumbco-pos clerk`.

## Commands at a glance

| Command                  | What it does                                                  |
|--------------------------|---------------------------------------------------------------|
| `scan`                   | Reveal hosts linked from the current machine                  |
| `connect HOST USER [PW]` | Log in to a discovered host (saved passwords are reused)      |
| `disconnect` / `exit`    | Leave the remote host and return to your rig                  |
| `portscan HOST`          | List services on a discovered host                            |
| `crack HOST USER`        | Dictionary attack against an SSH account                      |
| `exploit HOST SERVICE`   | Use a vulnerability to get a root shell                       |
| `ls cd pwd cat head tail wc` | Read the remote filesystem                                |
| `grep [-ivn] PAT FILE`   | Search one file                                               |
| `find [PATH] [-name G]`  | Find files                                                    |
| `cp mv rm mkdir touch`   | Change the remote filesystem (protected dirs stay protected)  |
| `download PATH`          | Copy a remote file into `~/loot` on your rig                  |
| `mail [N]`               | Read your mail                                                |
| `status`, `objectives`   | Progress and the current brief                                |
| `publish` / `sell`       | The final choice, after chapter 5                             |

Every remote command raises your **trace**. At 100% you are thrown off the
host, so plan which host you touch and how often.

## How it is built

The game is plain POSIX sh, with no bashisms, split into small libraries:

| File                    | Role                                                    |
|-------------------------|---------------------------------------------------------|
| `game/bin/nullroute`    | Entry point: options, sourcing libraries, starting      |
| `game/lib/core.sh`      | Paths, persistent key=value state, hashing, story I/O   |
| `game/lib/vfs.sh`       | Sandboxed filesystem commands                           |
| `game/lib/net.sh`       | scan, connect, crack, exploit, download, trace          |
| `game/lib/missions.sh`  | Chapter progression, mail, endings                      |
| `game/lib/shell.sh`     | REPL, prompt, dispatch, help                            |
| `game/world/`           | Hosts, wordlist, missions and story (plain text)        |
| `launcher/nullroute.c`  | Native launcher for Windows and Linux (no game logic)   |

Some POSIX choices worth knowing:

- **Sandbox.** Every path is normalised against the session's working
  directory and then mapped under the host's own folder. `..` cannot climb out.
  Real utilities only ever see paths already checked by the sandbox.
- **Whitelisted flags.** Commands that pass arguments to real tools (such as
  `grep`) accept a fixed set of flags. Otherwise `grep -f` could read files
  from your real machine.
- **Save data is plain text.** `state` is key=value lines, `creds` is three
  columns, and the world directory is copied once so that the player's changes
  to remote hosts persist. Everything is editable with a text editor.
- **Hashing.** Passwords are hashed with `cksum`, which is POSIX and
  everywhere. It is not secure, and that is the point of the game: the
  wordlist attack is fast enough to be fun.

## Tests

```sh
make test                     # runs under sh (dash on Ubuntu)
NR_TEST_SHELL="bash --posix" make test
NR_TEST_SHELL="busybox sh"   make test
make lint                     # shellcheck over every script
```

The suite plays scripted sessions: it checks discovery, cracking, sandbox
escapes, permissions, flags, protected folders, trace, persistence, the full
five-chapter walkthrough, and both endings.

## Build

```sh
make launcher                 # build/nullroute (Linux, needs cc)
make launcher-win             # build/nullroute.exe (needs x86_64-w64-mingw32-gcc)
make package                  # dist/ archives for both
```

CI builds both launchers on every push, runs the tests under dash, bash
`--posix` and busybox, and publishes release archives when a `v*` tag is
pushed. See [.github/workflows/ci.yml](.github/workflows/ci.yml).

## Licence

MIT. See [LICENSE](LICENSE).
