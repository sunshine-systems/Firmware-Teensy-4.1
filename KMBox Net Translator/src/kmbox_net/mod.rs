//! KMBox Net protocol — wire-format types (`schema`) and parsing /
//! reply construction (`parser`).
//!
//! Reference implementation: <https://github.com/kvmaibox/kmboxnet>.

pub mod parser;
pub mod schema;

// Re-export the items most call sites need so consumers can write
// `use crate::kmbox_net::Header;` etc. without reaching into submodules.
pub use schema::{
    cmd_name, Header, SoftMouse, CMD_BAZER_MOVE, CMD_CONNECT, CMD_DEBUG, CMD_KEYBOARD_ALL,
    CMD_MASK_MOUSE, CMD_MONITOR, CMD_MOUSE_AUTOMOVE, CMD_MOUSE_LEFT, CMD_MOUSE_MIDDLE,
    CMD_MOUSE_MOVE, CMD_MOUSE_RIGHT, CMD_MOUSE_WHEEL, CMD_REBOOT, CMD_SETCONFIG, CMD_SETVIDPID,
    CMD_SHOWPIC, CMD_TRACE_ENABLE, CMD_UNMASK_ALL, HEADER_LEN,
};

// `SoftKeyboard`, `SOFT_MOUSE_LEN`, `SOFT_KEYBOARD_LEN` are intentionally
// not re-exported here. They're reachable via
// `crate::kmbox_net::schema::*` and `crate::kmbox_net::parser::*` for any
// future consumer that needs them.
