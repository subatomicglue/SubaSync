SubaSync
========

SubaSync is a C++17 peer-to-peer mesh client for resilient digital libraries.  Each peer exposes a catalog of shared files, discovers other peers, and keeps directories in sync by downloading from any available source in the mesh. The project targets simple command-line workflows while remaining extensible for future transports and interfaces.

Key Features
------------
- Mesh-aware peer discovery and connection management via Asio.
- Content-addressed file sharing: every file is tracked by its SHA-256 hash and can be fetched from any peer that advertises it.
- Round-robin, multi-provider downloads with chunk hashing to detect corrupted transfers.
- Configurable directory "watchers" that keep remote folders mirrored locally.
- CLI interface.

Project Layout
--------------
- `src/` – All application sources (kept flat for now). Notable entries:
  - `main.cpp` boots the networking stack and CLI.
  - `PeerManager` orchestrates connections, listings, and chunk transfers.
  - `MeshCLI` implements the interactive shell, watch scheduler, and sync logic.
- `SUMMARY.md` – Architectural notes and dependency versions.
- `Makefile` – Convenience targets wrapping CMake/Conan builds and LLDB.
- `run.sh` / `kill.sh` – Helper scripts to launch or stop multi-peer demos, locally.
- `build/Release/sync`, `build/Debug/sync` – CMake build outputs.

Build Instructions
------------------
1. Install Conan 2.19.1 and CMake 3.27+ (the project currently uses 4.0.3).
2. From the repository root run:
   ```bash
   make       # configures via Conan + CMake and builds debug and release
   ```
3. To debug with LLDB:
   ```bash
   make lldb
   ```

You can also run the provided `run.sh` script to spawn three CLI instances that emulate separate peers. Use `kill.sh` to terminate them.

Using the CLI
-------------
Start the application (`build/Release/sync` or `build/Debug/sync`) and interact at the `>` prompt.

- `peers` – List known peers and their states.
- `list`, `ls`, `l` – Display local or remote listings. Supply `peer:/path`, a hash, or leave blank for local.
- `la` – Alias for `list all`; optionally append extra arguments (e.g., `la peerA:/docs`).
- `watch add <peer:/path> [dest/]` – Configure a periodic sync. Watches now queue their first background run after the default interval, avoiding duplicate "sync failed" messages caused by overlapping jobs.
- `watch list` / `watch remove` / `watch interval <seconds>` – Manage existing watches.
- `sync` – Force all configured watches to run immediately; `sync <target>` still performs a one-off pull from the specified peer/hash.
- `conflict [list|accept|ignore]` – Inspect pending download conflicts, overwrite with approval, or persistently skip specific paths/hashes/dir GUIDs.
- `conflict stage|view|unstage <target>` – Stage a conflicting file for inspection, open it with the OS viewer, or discard the staged preview.
- `ignore [list|add|remove] <target>` – Manage global ignore filters (paths, hashes, directory GUIDs).
- `settings [list|get|set|save|load]` – Inspect or modify runtime configuration; `--save` persists CLI overrides to disk.
- `bell` – Trigger the terminal bell (useful for testing audio notifications).
- `share <path>` – Add local content to the mesh catalog.
- `dirs` – Display share and staging directories.
- `quit` – Exit.

Configuration
-------------
- Startup order: defaults → `share/.config/settings.json` → command-line overrides.
- Use `--save` (or `settings save`) to persist the current configuration.
- Settings live in `share/.config/settings.json` and can be adjusted on-the-fly via the `settings` command.

Conflict Handling
-----------------
- When a download would overwrite a local file with different contents, the file is skipped and a conflict is queued instead of overwriting blindly.
- Use `conflict list` to review queued items, `conflict accept <path|hash|dir-guid>` to approve overwrites (the old copy is archived under `share/.archive/`), or `conflict ignore ...` to persistently suppress future sync attempts for that identifier.
- `conflict stage <target>` downloads the remote candidate into `share/.conflict-stage/` with a timestamped filename; `conflict view` opens the staged file via the OS, and `conflict unstage` removes it.
- The standalone `ignore` command manipulates the same rules stored in `share/.config/ignore.json` and can be used outside the conflict workflow.

Development Notes
-----------------
- Code style uses 2-space indentation with same-line braces.
- Networking is implemented with standalone Asio (via Conan). JSON handling relies on `nlohmann::json`, and SHA-256 hashing leverages OpenSSL.
- The share root is `share/`, with in-progress downloads staged under `share/.sync_tmp/`. Avoid modifying these directories manually while the app is running.
- Potential overwrites are moved into `share/.archive/`, while long-term conflict ignores live in `share/.config/ignore.json`.
- Conflict previews are stored in `share/.conflict-stage/` until they are accepted or explicitly unstaged.

Planned Directions
------------------
- Refactor to support "transports".
  - HTTP-based transports for RESTful endpoints, and an HTML interface layered over the existing `PeerManager` model. The current design aims to keep those options open while maintaining a lean CLI experience.
- Explore a consensus threshold so overwrites can be auto-approved only when multiple peers advertise matching hashes.
- Add an interactive startup helper to prompt for missing or invalid settings before the node launches.
- document: add comments throughout
- clean:  analyze the code and restructure
