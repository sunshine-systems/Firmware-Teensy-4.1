# USB Host Proxy — Teensy 4.1

**USB Host Proxy** is a transparent USB HID mouse proxy for the Teensy 4.1. The
Teensy sits between a physical mouse and the host PC and presents a bit-for-bit
clone of the mouse — same VID/PID, descriptors, polling rate (up to 8 kHz), and
USB speed (Low / Full / High).

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
├── KMBox Net Translator/             PC-side Rust UDP -> serial bridge
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

## KMBox Net Translator

`KMBox Net Translator/` is a small PC-side Rust program that lets host
applications which speak the **KMBox Net** UDP protocol drive this firmware.
The firmware itself does not implement KMBox Net; it speaks the compact
9-byte Sunshine binary protocol over serial. The translator bridges the gap.

```
host app  --UDP (KMBox Net)-->  translator  --serial (Sunshine 9-byte)-->  Teensy  --USB HID-->  PC
```

See [`KMBox Net Translator/README.md`](./KMBox%20Net%20Translator/README.md)
for build, config (`config.json`), and logging details.

### Why UDP rather than KMBox B+ text over serial

The serial link to the Teensy (Serial4 @ 115200 baud) is a finite pipe, and
the firmware already uses it for other things — debug/log output (channel
gated via `SunBoxLogger`, see
[`firmware/libraries/USBHostProxy/README.md`](firmware/libraries/USBHostProxy/README.md#sunboxlogger))
and delta logging from the movement sanitization pipeline
([`firmware/libraries/USBHostProxy/examples/SunshineUSBProxy/MOVEMENT_SANITIZATION.md`](firmware/libraries/USBHostProxy/examples/SunshineUSBProxy/MOVEMENT_SANITIZATION.md)).

That pipe is not cheap. From the dev guide:

> Serial4 logging at 115200 baud adds ~4.3ms per 50-char message. Logging in
> the enumeration hot path caused a 50ms regression.
> — [`firmware/libraries/USBHostProxy/usb-proxy-dev-guide.md`](firmware/libraries/USBHostProxy/usb-proxy-dev-guide.md) (Lessons Learned #11)

At ~11.5 KB/s wire bandwidth, every byte costs ~87 µs. A single KMBox B+
text command like `km.move(123,-45)\r\n` is typically 15–20 bytes — well over
1 ms of serial time per command, before the firmware has even parsed it. At
mouse-grade update rates that is the dominant source of pressure on the link
and starves out diagnostic logging.

The Sunshine binary protocol the firmware speaks natively is **9 bytes per
frame**, fixed-width, no parsing — roughly an order of magnitude less serial
traffic for the same information. Moving the KMBox Net command volume off
the serial line entirely (UDP on the PC side, binary on the wire) keeps the
serial pipe free for logging and for the firmware's own delta stream.

Run from the same folder so it finds/writes `config.json`:

```
cd "KMBox Net Translator"
cargo build --release
target\release\kmbox_net_translator.exe
```

On first run with no `config.json` present, the translator writes a default
config and exits with a message telling you to edit `listen_addr` and
`com_port`.

## Contact

Issues, questions, contributions: **jtonna@proton.me**

## License

See `LICENSE` (add one before publishing).
