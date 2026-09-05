# KenshiMMO

Turn Kenshi into a small online world. Friends log into the **same server**, share the
**same world**, and take over each other's save state instead of keeping separate single-player
runs. The server is authoritative: accounts, the shared world, and per-player save blobs all live
server-side.

- Client: `KenshiMMO.dll` plugin, injected by RE_Kenshi.
- Server: `KenshiMMO.Server.exe` — runs headless, needs **no** Kenshi install.
- Default public-ish server: `82.165.24.196:25565` (compiled in, editable in-game).

> You need a **licensed copy of Kenshi** (Steam or GOG, version 1.0.65 or 1.0.68).
> The retail game and its assets are **not** in this repository.

---

## 1. What's in the repo

| File / folder | What it is |
|---|---|
| `KenshiMMO.dll` | Client plugin. Goes into `mods/KenshiMMO/`. |
| `KenshiMMO.mod` | Kenshi data mod (co-op Wanderer/Multiplayer+ starts). Goes with the DLL. |
| `RE_Kenshi.json` | Plugin config that tells RE_Kenshi to load `KenshiMMO.dll`. |
| `KenshiMMO.Server.exe` | The server. Run it anywhere (Windows or Linux/Wine). |
| `third-party/RE_Kenshi/` | RE_Kenshi installer + licenses (GPL-3.0, by BFrizzleFoShizzle). |
| `src/`, `server/`, `Makefile` | Source code (build with mingw-w64 if you want to rebuild). |
| `DEPLOY.md` | Concise deployment reference. |
| `run_kenshi.sh` | Optional Linux (Wine) launcher for the client. |

You can also just download `KenshiMMO-v0.1.0.zip` from the **Releases** page — it contains the
client mod folder and the server exe, ready to unpack.

---

## 2. Requirements

**Player (client)**
- Kenshi 1.0.65 or 1.0.68 (Steam or GOG).
- RE_Kenshi (included in this repo).
- The three KenshiMMO files installed (step 4).
- Network access to the server's TCP port **25565**.

**Server host**
- Windows, or Linux with Wine (`wine64`).
- If you want friends *outside* your LAN to join: a reachable IP and TCP `25565` open in the
  firewall/router (port-forward `25565` to the server machine).

---

## 3. Install RE_Kenshi (one-time)

RE_Kenshi is the plugin loader that KenshiMMO hangs off of. It's free software (GPL-3.0) and its
installer is bundled in this repo.

1. Extract `third-party/RE_Kenshi/` and run **`RE_Kenshi_v0.3.5.exe`**.
2. Follow the installer. It will ask for your **Kenshi install folder**
   - Steam: right-click Kenshi in your library → Manage → Browse local files.
   - GOG: wherever you installed it (default `C:\GOG Games\Kenshi` or similar).
3. The installer may warn about version 1.0.68 → it creates a **downgraded 1.0.65 build**
   automatically, because RE_Kenshi is based on 1.0.65. This is expected and safe.
4. The installer gives you a shortcut **`RE_Kenshi`** — use it (or the normal Kenshi shortcut;
   RE_Kenshi's just starts ~1s faster).
5. Sanity check: launch Kenshi once, go to **OPTIONS → MODS → RE_KENSHI SETTINGS**. If you see
   the RE_Kenshi menu, the loader is installed.

> **Uninstall / disable RE_Kenshi**: re-run the installer and choose "uninstall", or remove
> `Plugin=RE_Kenshi` from `Plugins_x64.cfg` in the Kenshi install folder. Launch normal Kenshi
> with `--norekenshi` to skip RE_Kenshi for one run.

---

## 4. Install the KenshiMMO client

1. In the Kenshi install folder, create `mods/KenshiMMO/` and copy these three files into it:
   ```
   <Kenshi>/mods/KenshiMMO/KenshiMMO.dll
   <Kenshi>/mods/KenshiMMO/KenshiMMO.mod
   <Kenshi>/mods/KenshiMMO/RE_Kenshi.json
   ```
2. Start the game (via the `RE_Kenshi` shortcut works best).
3. Once in the main menu you should see a small **KenshiMMO** login window.

> It normally "just works". If the game starts but the panel doesn't appear, the DLL wasn't
> loaded — confirm `RE_Kenshi.json` is present, the loader is using this Kenshi install, and
> there were no load errors in `KenshiMMO.log` (written next to Kenshi.exe).

---

## 5. Run a server

The server has **no config files** — run the exe, that's it. It listens on `0.0.0.0:25565`,
creates its `accounts.dat` on first use, and stores each player's world under `saves/<user>/`.

**Windows**

```bat
KenshiMMO.Server.exe
```

**Linux (Wine)**

```sh
wine64 KenshiMMO.Server.exe
# or: /usr/lib/wine/wine64 KenshiMMO.Server.exe
```

First line you see should be:

```
OK KenshiMMO server v0.1. Type REGISTER <user> <pass> or LOGIN <user> <pass>
```

- Leave it running in a terminal (or under `nohup`/systemd).
- Log lines go to stdout (`KenshiMMO.Server.log` if redirected).
- Open TCP **25565** on the server's firewall so clients can reach it.

---

## 6. Play

In the KenshiMMO panel inside the game:

1. Set **Host** and **Port** to the server you're joining (`82.165.24.196` / `25565`, or your own).
2. **First time**: type a username + password and hit **REGISTER**.
   - Registration creates the account and immediately logs you in.
3. **Returning**: hit **LOGIN**.
4. On your **first login ever**, the client uploads your current local save — that becomes the
   shared world for that account. From then on the server is the source of truth; don't reuse the
   local save.
5. You spawn as your co-op Wanderer squad and everyone else on the server exists in the same
   world as you play.

> The credentials are the same account system used by the server: `accounts.dat` stores
> username, random salt, and the salted SHA-256 of the password.

---

## 7. Point the client at your own server (source build)

The compiled-in default is `82.165.24.196:25565`. To make your server the default instead:

1. Edit `KMMO_SERVER_HOST` (and if needed `kProtoPort`) in `src/version.h` / `src/net.h`.
2. Rebuild: `make` (needs `x86_64-w64-mingw32-g++`).
3. Redeploy the new `KenshiMMO.dll` to `mods/KenshiMMO/`.

Or simply type the address into the panel's Host/Port fields (no rebuild needed).

---

## 8. Linux / Wine client (advanced)

The client runs under Wine on Linux too. Typical setup:

```sh
# 1. Install Kenshi + RE_Kenshi into a Wine prefix (runs their installers under wine).
# 2. Drop the three KenshiMMO files into drive_c/Kenshi/mods/KenshiMMO/
# 3. Launch:
./run_kenshi.sh
```

`run_kenshi.sh` just resolves Wine and Kenshi.exe and runs it with a sensible `DISPLAY`.
Environment it respects: `WINE`, `KENSHI_EXE`, `DISPLAY`, `XAUTHORITY`.

**Graphics gotchas on Linux**
- The game needs OpenGL/Vulkan from a **working GPU driver**. On NVIDIA, the kernel module and
  userspace packages must be the same version (Symptoms of mismatch: `nvidia-smi` failing,
  `Failed to enumerate physical devices`, llvmpipe software rendering, and a
  `virtual_setup_exception stack overflow` crash).
- If launched from an SSH session, point `XAUTHORITY` at the X cookie of the logged-in desktop
  (e.g. `/run/user/1000/xauth_xxx`), otherwise the game can't open the display.
- With a hybrid Intel + NVIDIA machine, make sure the Intel `i915` GPU isn't blacklisted if you
  want it as a fallback.

---

## 9. Troubleshooting

| Problem | Likely cause / fix |
|---|---|
| Panel says `Net error (unreachable host:port)` | Server down, wrong IP/port, or firewall. Ping the host; check TCP 25565 is open. |
| `ERR BAD_PASSWORD` | Wrong password, or account doesn't exist (register instead). |
| `ERR USER_NOT_FOUND` | No such account on this server; use REGISTER. |
| Game starts, no panel | DLL not loaded. Check `mods/KenshiMMO/RE_Kenshi.json` exists and RE_Kenshi is active on this install; read `KenshiMMO.log`. |
| Server won't start under Wine | Use `wine64` (a 64-bit prefix). Missing `wine64-preloader`/wrong symlink → "could not exec the wine loader". Invoke `/usr/lib/wine/wine64` directly if the bundled `wine` alias fails. |
| Server reachable from host but not friends | Port-forward TCP 25565 on the router; open it in the server firewall. |
| Crash right after launch (Linux) | Check `nvidia-smi`; if it errors, kernel module and userspace NVIDIA differ — reboot/update to one version. |
| Steam re-downloads Kenshi or update fights RE_Kenshi | RE_Kenshi downgrades to 1.0.65; set Kenshi to not auto-update, or add `--norekenshi` for vanilla runs. |

---

## 10. Security & honesty notes

- This is a hobby project; the server protocol is plaintext on TCP 25565.
- Don't reuse important passwords for accounts (server passwords are stored salted+hashed, but
  transit is not encrypted).
- Play only in worlds/servers you trust. The plugin reads/writes your Kenshi process memory —
  it's a mod, but run it against a genuinely-licensed Kenshi and from sources you trust.

---

## License

- Project sources: see repository (author's own code).
- RE_Kenshi and bundled third-party components: GPL-3.0 / their own licenses, under
  `third-party/RE_Kenshi/Licenses/`.
- Kenshi itself is (c) Lo-Fi Games — **not** distributed here.