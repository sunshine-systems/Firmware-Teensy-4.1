# KMBox Net Translator

Bridges third-party **KMBox Net** UDP host apps to the Teensy USB Host Proxy
firmware, which speaks the **Sunshine binary 9-byte serial protocol**.

```
host app  --KMBox Net UDP-->  THIS TRANSLATOR  --Sunshine binary serial-->  Teensy  --USB HID-->  PC
```

## Build

From inside this folder:

```
cargo build --release
```

The resulting binary is `target/release/kmbox_net_translator.exe`.

## Run

```
kmbox_net_translator.exe
```

On first run with no `config.json` present, the program writes a default
`config.json` to the working directory and exits with a message telling you
to edit `listen_addr` and `com_port`. If the existing `config.json` fails to
parse or is missing required fields, it is deleted and replaced with a
fresh default.

## `config.json`

```json
{
  "listen_addr": "0.0.0.0",
  "udp_port": 8888,
  "com_port": "COM7",
  "baud_rate": 115200,
  "device_mac": "01FBC068"
}
```

| Field         | Type   | Required | Notes                                                       |
|---------------|--------|----------|-------------------------------------------------------------|
| `listen_addr` | string | yes      | Any local IP, e.g. `0.0.0.0` or `127.0.0.1`.                |
| `udp_port`    | u16    | no       | Defaults to `8888`.                                         |
| `com_port`    | string | yes      | Windows COM name, e.g. `COM7`.                              |
| `baud_rate`   | u32    | no       | Defaults to `115200`.                                       |
| `device_mac`  | string | no       | 8 hex chars. Defaults to `01FBC068`. Must match the host app. |

## Logging

Set `RUST_LOG=debug` for verbose logs. Default level is `info`.

## Tests

```
cargo test
```
