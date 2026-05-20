//! Streamcheats firmware binary protocol — the outgoing side of the bridge.
//!
//! Every serial packet sent to the Teensy is exactly 9 bytes wide
//! ([`PACKET_LEN`]) and carries a 1-byte length prefix (`0x08`), a button
//! bitmask, a low-byte/extended-bytes encoding of `(x, y)`, and the wheel
//! delta. The format and the rationale for *always* populating the
//! extended `(x, y)` slots live at the top of [`packet`].
//!
//! Reference: `FirmwareInterface.create_spoofed_hid_report` in the Python
//! `sunbox_interface` package — the Rust [`build_packet`] is byte-for-byte
//! compatible with that reference, with the single intentional difference
//! that byte 4 carries wheel data instead of Python's `sensReduction` flag.

pub mod device_settings;
pub mod packet;

pub use device_settings::{build_settings_packet, DeviceSettings};
pub use packet::{build_packet, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, PACKET_LEN};
