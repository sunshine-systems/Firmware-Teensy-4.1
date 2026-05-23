# USB Host Proxy — Teensy 4.1

**USB Host Proxy** is a transparent USB HID mouse proxy for the Teensy 4.1. The
Teensy sits between a physical mouse and the host PC and presents a bit-for-bit
clone of the mouse — same VID/PID, descriptors, polling rate (up to 8 kHz), and
USB speed (Low / Full / High).

## Requirements

For official builds, this Teensy core is controlled by the **StreamCheats Core**
application — used to test, debug, control, and flash firmware to the device.
See: https://github.com/sunshine-systems/streamcheats-core

If you are doing a DIY or unofficial build (fixing bugs, adding device support,
or other modifications), continue with the guide below.

## Start here

The actual project lives at:

```
firmware/libraries/USBHostProxy/
```

Open that folder first. It contains the library source, the example sketch
(`examples/SunshineUSBProxy/`), and the in-tree design docs.

This whole repository **is** the full Teensy 1.59.0 Arduino core. The proxy
needed core-level modifications (notably an early-startup hook patched into
`firmware/cores/teensy4/startup.c`, plus board/platform tweaks), so shipping
the modified core wholesale is simpler and more reliable than a patch series
against the vendor install.

For the in-depth architecture reference — data flow, descriptors, ZLT bit,
PHY configuration, EEPROM layout, verified devices, troubleshooting — see
[`CLAUDE.md`](CLAUDE.md).

## Repository layout

```
.
├── firmware/                         Modified Teensy 1.59.0 Arduino core
│   ├── boards.txt                    Board metadata
│   ├── cores/                        Core sources (modified startup.c)
│   ├── libraries/
│   │   └── USBHostProxy/             *** The actual project ***
│   └── platform.txt                  Build/upload recipes
├── deploy.bat                        Interactive deploy / backup / restore
├── CLAUDE.md                         Full library reference
├── README.md                         You are here
├── .gitignore
└── .gitattributes
```

The `firmware/` folder is the source of truth. `deploy.bat` pushes it into
`%LOCALAPPDATA%\Arduino15\packages\teensy\hardware\avr\<version>` so the
Arduino IDE picks it up.

## Quick start (install the modified core)

1. Install the Teensy boards package via Arduino IDE
   (Boards Manager → "Teensy by Paul Stoffregen"). This creates the install
   path the script will target.
2. Clone this repo to any folder (e.g. `Documents\GitHub\Teensy-Core-1.59.0`).
3. Run `deploy.bat`. If multiple Teensy core versions are installed, the
   script will ask which one to target.
4. Choose **[1] Backup** the first time — this snapshots the pristine vendor
   core to `teensy_core_backup.zip` so you can revert later.
5. Edit anything under `firmware/`.
6. Choose **[2] Deploy** to push `firmware/` into the live install path.
   The script closes any running Arduino processes, clears the IDE caches,
   copies the files, and re-launches the IDE if it was open.
7. Choose **[3] Restore** at any time to revert the live install to the
   pristine state captured in step 4.

All paths are resolved relative to the active Windows user — nothing is
hardcoded.

After deploying, open
`firmware/libraries/USBHostProxy/examples/SunshineUSBProxy/SunshineUSBProxy.ino`,
select **Tools → Board → Teensy 4.1**, and upload.

## What `deploy.bat` actually touches

| Path                                                                                              | When                                                              |
|---------------------------------------------------------------------------------------------------|-------------------------------------------------------------------|
| `<install>\cores`, `<install>\libraries`, `boards.txt`, etc.                                      | overwritten on deploy / restore                                   |
| `%APPDATA%\Arduino IDE`, `%APPDATA%\arduino-ide`                                                  | deleted before deploy / restore                                   |
| Any process whose exe path contains `arduino` (IDE, arduino-cli, discovery tools, language server) | killed before deploy / restore                                    |
| `Arduino IDE.exe` (detached relaunch via `Start-Process`)                                         | re-launched after, only if the IDE was open when the script ran   |

Repo files themselves (`.git/`, `CLAUDE.md`, `README.md`, `firmware/`,
`deploy.bat`, `.gitattributes`, `.gitignore`, `teensy_core_backup.zip`) are
never touched.

## Contact

Issues, questions, contributions: **jtonna@proton.me**

## License

See `LICENSE` (add one before publishing).
