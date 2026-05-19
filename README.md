# USB Host Proxy — Teensy 4.1

A transparent USB HID mouse proxy for Teensy 4.1. The device appears
identical to the physical mouse to the host PC, supporting polling rates
up to 8 kHz and Low / Full / High USB speeds.

See [`CLAUDE.md`](CLAUDE.md) for the full library reference (architecture,
data flow, descriptors, EEPROM layout, verified devices, etc.).

## Repository layout

```
.
├── firmware/                Source of truth for the modified Teensy core
│   ├── boards.txt
│   ├── cores/
│   ├── installed.json
│   ├── keywords.txt
│   ├── libraries/
│   └── platform.txt
├── deploy.bat               Interactive deploy / backup / restore tool
├── CLAUDE.md                Library reference
├── README.md                You are here
├── .gitignore
└── .gitattributes
```

Clone this repo anywhere on disk — the `firmware/` folder holds the
canonical sources. `deploy.bat` auto-detects your installed Teensy core
at `%LOCALAPPDATA%\Arduino15\packages\teensy\hardware\avr\<version>` and
pushes `firmware/` into it so the Arduino IDE picks up your changes.

## Quick start

1. Install the Teensy boards package via Arduino IDE (Boards Manager →
   "Teensy by Paul Stoffregen"). This creates the install path the
   script will target.
2. Clone this repo to any folder (e.g. `Documents\GitHub\Teensy-Core-1.59.0`).
3. Run `deploy.bat`. If you have multiple Teensy core versions installed
   the script will ask which one to target.
4. Choose **[1] Backup** the first time — this snapshots the pristine
   vendor core (from the install path) to `teensy_core_backup.zip` so
   you can revert later.
5. Edit anything you need under `firmware/`.
6. Choose **[2] Deploy** to push `firmware/` into the live install path.
   The script closes any running Arduino processes, clears the IDE
   caches, copies the files, and re-launches the IDE if it was open.
7. Choose **[3] Restore** at any time to revert the live install to the
   pristine state captured in step 4.

All paths are auto-resolved relative to the active Windows user —
nothing is hardcoded.

## What deploy.bat actually touches

| Path                                                              | When                                  |
|-------------------------------------------------------------------|---------------------------------------|
| `<install>\cores`, `<install>\libraries`, `boards.txt`, etc.       | overwritten on deploy / restore       |
| `%APPDATA%\Arduino IDE`, `%APPDATA%\arduino-ide`                   | deleted before deploy / restore       |
| Any process whose exe path contains `arduino` (IDE, arduino-cli, discovery tools, language server) | killed before deploy / restore        |
| `Arduino IDE.exe` (detached relaunch via `Start-Process`)          | re-launched after, only if the IDE was open when the script ran |

The repo files themselves (`.git/`, `CLAUDE.md`, `README.md`,
`firmware/`, `deploy.bat`, `.gitattributes`, `.gitignore`,
`teensy_core_backup.zip`) are never touched.

## Building a sketch

The example lives at
`firmware/libraries/USBHostProxy/examples/SunshineUSBProxy/SunshineUSBProxy.ino`
(open it after a deploy). Select **Tools → Board → Teensy 4.1** and
upload as normal.

## Contact

Issues, questions, contributions: **jtonna@proton.me**

## License

See `LICENSE` (add one before publishing).
