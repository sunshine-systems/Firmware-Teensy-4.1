//! KMBox Net wire-format types and command codes.
//!
//! All fields are little-endian, no padding. The wire packet is a
//! 16-byte [`Header`] followed by a command-specific body — usually
//! [`SoftMouse`], occasionally [`SoftKeyboard`].

pub const HEADER_LEN: usize = 16;
pub const SOFT_MOUSE_LEN: usize = 4 * 4 + 4 * 10; // 16 + 40 = 56
#[allow(dead_code)]
pub const SOFT_KEYBOARD_LEN: usize = 1 + 1 + 10; // 12

// ------------------------------------------------------------------
// Command codes (from official SDK headers)
// ------------------------------------------------------------------
pub const CMD_CONNECT: u32 = 0xAF3C2828;
pub const CMD_MOUSE_MOVE: u32 = 0xAEDE7345;
pub const CMD_MOUSE_LEFT: u32 = 0x9823AE8D;
pub const CMD_MOUSE_RIGHT: u32 = 0x238D8212;
pub const CMD_MOUSE_MIDDLE: u32 = 0x97A3AE8D;
pub const CMD_MOUSE_WHEEL: u32 = 0xFFEEAD38;
pub const CMD_MOUSE_AUTOMOVE: u32 = 0xAEDE7346;
pub const CMD_BAZER_MOVE: u32 = 0xA238455A;
pub const CMD_KEYBOARD_ALL: u32 = 0x123C2C2F;
pub const CMD_REBOOT: u32 = 0xAA8855AA;
pub const CMD_MONITOR: u32 = 0x27388020;
pub const CMD_MASK_MOUSE: u32 = 0x23234343;
pub const CMD_UNMASK_ALL: u32 = 0x23344343;
pub const CMD_SETCONFIG: u32 = 0x1D3D3323;
pub const CMD_SETVIDPID: u32 = 0xFFED3232;
pub const CMD_DEBUG: u32 = 0x27382021;
pub const CMD_SHOWPIC: u32 = 0x12334883;
pub const CMD_TRACE_ENABLE: u32 = 0xBBCDDDAC;

// ------------------------------------------------------------------
// Types
// ------------------------------------------------------------------

#[derive(Debug, Clone, Copy)]
pub struct Header {
    pub mac: u32,
    pub rand: u32,
    pub indexpts: u32,
    pub cmd: u32,
}

#[derive(Debug, Clone, Copy)]
pub struct SoftMouse {
    pub button: i32,
    pub x: i32,
    pub y: i32,
    pub wheel: i32,
    pub point: [i32; 10],
}

#[allow(dead_code)]
#[derive(Debug, Clone, Copy)]
pub struct SoftKeyboard {
    pub ctrl: i8,
    pub resvel: i8,
    pub button: [i8; 10],
}

/// Human-readable label for a command code, used in logs.
pub fn cmd_name(cmd: u32) -> &'static str {
    match cmd {
        CMD_CONNECT => "connect",
        CMD_MOUSE_MOVE => "mouse_move",
        CMD_MOUSE_LEFT => "mouse_left",
        CMD_MOUSE_RIGHT => "mouse_right",
        CMD_MOUSE_MIDDLE => "mouse_middle",
        CMD_MOUSE_WHEEL => "mouse_wheel",
        CMD_MOUSE_AUTOMOVE => "mouse_automove",
        CMD_BAZER_MOVE => "bezier_move",
        CMD_KEYBOARD_ALL => "keyboard_all",
        CMD_REBOOT => "reboot",
        CMD_MONITOR => "monitor",
        CMD_MASK_MOUSE => "mask_mouse",
        CMD_UNMASK_ALL => "unmask_all",
        CMD_SETCONFIG => "setconfig",
        CMD_SETVIDPID => "setvidpid",
        CMD_DEBUG => "debug",
        CMD_SHOWPIC => "showpic",
        CMD_TRACE_ENABLE => "trace_enable",
        _ => "unknown",
    }
}
