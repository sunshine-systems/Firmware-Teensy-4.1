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

The repo lives at the Arduino Teensy core install path
(`%LOCALAPPDATA%\Arduino15\packages\teensy\hardware\avr\1.59.0`). The
`firmware/` folder holds the canonical sources. `deploy.bat` pushes
`firmware/` into the surrounding folder so the Arduino IDE picks up your
changes when it scans for cores.

## Quick start

1. Clone this repo directly over the Teensy 1.59.0 install path. The
   path is auto-detected by Arduino IDE at
   `%LOCALAPPDATA%\Arduino15\packages\teensy\hardware\avr\1.59.0`.
2. Run `deploy.bat`.
3. Choose **[1] Backup** the first time — this snapshots the pristine
   vendor core to `teensy_core_backup.zip` so you can revert later.
4. Edit anything you need under `firmware/`.
5. Choose **[2] Deploy** to push `firmware/` into the live core path. The
   script closes the Arduino IDE, clears its caches, copies the files,
   and reopens the IDE if it was running.
6. Choose **[3] Restore** at any time to revert the live core to the
   pristine state captured in step 3.

`deploy.bat` resolves all paths relative to the active Windows user —
nothing is hardcoded.

## What deploy.bat actually touches

| Path                                     | When                  |
|------------------------------------------|-----------------------|
| `<repo>\cores`, `<repo>\libraries`, …    | overwritten on deploy/restore |
| `%APPDATA%\Arduino IDE`                  | deleted before deploy/restore |
| `%APPDATA%\arduino-ide`                  | deleted before deploy/restore |
| `Arduino IDE.exe` process                | killed if running, relaunched after |

Everything else at the repo root (`.git/`, `CLAUDE.md`, `README.md`,
`firmware/`, `deploy.bat`, `.gitattributes`, `.gitignore`,
`teensy_core_backup.zip`) is never touched.

## Building a sketch

The example lives at
`firmware/libraries/USBHostProxy/examples/SunshineUSBProxy/SunshineUSBProxy.ino`
(open it after a deploy). Select **Tools → Board → Teensy 4.1** and
upload as normal.

## Contact

Issues, questions, contributions: **jtonna@proton.me**

## License

See `LICENSE` (add one before publishing).
