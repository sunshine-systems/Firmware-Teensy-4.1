# KMBox Net Translator

A Windows-side bridge that lets any host application speaking the
[KMBox Net](https://github.com/kvmaibox/kmboxnet) UDP protocol drive
the Streamcheats Teensy 4.1 USB Host Proxy firmware instead of a real
KMBox hardware device. The host app sees normal KMBox Net replies; the
target gaming PC sees normal HID mouse events from a transparently
proxied physical mouse.

```
host app  --UDP (KMBox Net)-->  TRANSLATOR (Rust daemon)
                                    |
                                    +-- serial (9-byte binary) --> Teensy USB Host Proxy --HID--> target PC
                                    |
                                    +-- HTTP (127.0.0.1)        --> Electron UI (optional)
```

## Five-minute orientation

Three top-level pieces, each independently buildable:

| Folder       | Stack             | Role                                                              |
|--------------|-------------------|-------------------------------------------------------------------|
| `backend/`   | Rust              | The daemon: UDP listener, serial supervisor, HTTP bug-report API. |
| `frontend/`  | Next.js + React   | Static-export UI loaded inside the Electron shell.                |
| `electron/`  | Electron (Node)   | System-tray shell that hosts `frontend/` and supervises `backend/`. |
| `tests/`     | Python + Markdown | Vendor SDK demo scripts and the protocol [`COMPATIBILITY_CHECKLIST.md`](tests/COMPATIBILITY_CHECKLIST.md). |

The daemon is the canonical entry point — the GUI and the bug-report
endpoint are conveniences built on top of it. You can run the daemon
standalone with nothing else installed and it will function fully
against any third-party KMBox Net host app.

## Build

Each tier builds independently. Typical dev flow:

```powershell
# 1. Rust daemon
cd backend
cargo build --release     # produces target/release/kmbox_net_translator.exe
cargo run --release       # spawn it directly (writes config.json on first run)

# 2. Next.js frontend (dev server, hot reload)
cd ../frontend
pnpm install
pnpm dev                  # http://localhost:3000

# 3. Electron shell (loads the dev server above)
cd ../electron
pnpm install
pnpm start                # opens the tray + window; spawns backend if not already running
```

Portable build (one self-contained `.exe` for client preview):

```powershell
cd frontend && pnpm build        # static export to frontend/out/
cd ../backend && cargo build --release
cd ../electron && pnpm dist      # produces electron/dist-new/win-unpacked/
```

The resulting `dist-new/win-unpacked/KMBox Net Translator.exe` bundles
the daemon (`resources/kmbox_net_translator.exe`) and the static
frontend (`resources/frontend/`). Launching it spawns the daemon as a
child if one is not already running.

## Where to read next

* [`ARCHITECTURE.md`](ARCHITECTURE.md) — peer-protocol design, thread
  layout, event flow, state machine, HTTP surface, daemon lifecycle.
* [`backend/README.md`](backend/README.md) — config-file reference,
  log formats, supported commands, threading model.
* [`tests/COMPATIBILITY_CHECKLIST.md`](tests/COMPATIBILITY_CHECKLIST.md) —
  per-opcode protocol status (verified / partial / not implemented).
