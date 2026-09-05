# KenshiMMO

A LAN/Internet multiplayer mod for Kenshi (Lo-Fi Games). Consists of a client plugin
(`KenshiMMO.dll`) injected alongside the vanilla game, and a standalone server
(`KenshiMMO.Server.exe`). The server is authoritative: account logins, world
synchronization, and per-account save blobs all live server-side.

Requires a **licensed** copy of Kenshi. Distributed without game assets; nothing
from the retail install is included in this repository.

## Repository layout

```
Makefile                 Builds both targets (mingw cross-compile)
run_kenshi.sh            Linux launcher example for the client
src/                     Client plugin sources (dll)
server/                  Server sources (exe)
KenshiMMO.dll            Prebuilt client plugin  (md5 e6fff526d050ec3e4eee6744aca5ce9f)
KenshiMMO.Server.exe     Prebuilt server
```

## Build (requires mingw-w64)

```sh
make            # needs x86_64-w64-mingw32-g++
make clean
```

## Deploy

### 1. Server (Linux/Wine or Windows)

Does **not** need Kenshi installed. On Linux:

```sh
/usr/lib/wine/wine64 KenshiMMO.Server.exe
```

- Listens on `0.0.0.0:25565`.
- `accounts.dat` is optional. If missing it is created lazily; the first
  `REGISTER <user> <pass>` seeds a fresh account with a default wandering spawn.
- Saves are written per user to `saves/<user>/save.blob` (`saves/` is created
  automatically). Log output goes to stdout (`KenshiMMO.Server.log`).
- Firewall: open TCP 25565 to players.

### 2. Client

Copy the plugin into the vanilla install:

```
<Kenshi>/mods/KenshiMMO/KenshiMMO.dll
```

Then launch Kenshi with the `KenshiMMO` mod enabled. The plugin shows an
in-game login panel (`REGISTER <user> <pass>` for new accounts, `LOGIN` for
existing). First login uploads your local save as the world state.

### 3. Server address

The server host is compiled in: edit `KMMO_SERVER_HOST` in `src/version.h`
(defaults to the author's public server), rebuild the DLL, redeploy. Changing
the port is `KMMO_SERVER_PORT`.

## Linux client layout (Wine example)

```
~/.wine/drive_c/Kenshi/
├── Kenshi.exe
├── data/                      # vanilla assets (not in this repo)
├── mods/
│   └── KenshiMMO/
│       └── KenshiMMO.dll
└── KenshiMMO.log              # client log, written next to the exe
```

Run via `./run_kenshi.sh`.

## Notes

- Server is the single source of truth; the client never uses local saves after
  first upload.
- Protocol `OK KenshiMMO server v0.1. Type REGISTER <user> <pass> or LOGIN <user> <pass>`.