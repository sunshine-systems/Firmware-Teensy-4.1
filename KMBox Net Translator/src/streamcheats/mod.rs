//! Streamcheats firmware binary protocol — 9-byte fixed-width serial
//! packets emitted to the Teensy.
//!
//! Format details and the always-extended-form rationale live in [`packet`].

pub mod packet;

pub use packet::{build_packet, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, PACKET_LEN};

// `BTN_SIDE1` / `BTN_SIDE2` are defined in `packet` for future use but
// not re-exported here — the current KMBox Net command set has no
// dedicated side-button commands (side buttons can still arrive via
// `mouse_all`'s bitmask).
