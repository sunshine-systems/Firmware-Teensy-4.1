//! KMBox Net wire-format types and command codes.
//!
//! All fields are little-endian, no padding. The wire packet is a
//! 16-byte [`Header`] followed by a command-specific body — currently
//! always a [`SoftMouse`] (keyboard packets are acknowledged but not
//! decoded).

/// Size in bytes of the fixed [`Header`] that prefixes every UDP packet.
pub const HEADER_LEN: usize = 16;
/// Size in bytes of a [`SoftMouse`] body: four `i32` headline fields plus
/// ten `i32` slots in `point` (used for `automove`/`bezier` duration and
/// control points). `4*4 + 10*4 = 56`.
pub const SOFT_MOUSE_LEN: usize = 4 * 4 + 4 * 10; // 16 + 40 = 56

// ------------------------------------------------------------------
// Command codes (from official SDK headers)
//
// These literal `u32` values come straight from `kmbox_net.h` in the
// upstream vendor SDK (see <https://github.com/kvmaibox/kmboxnet>) and
// must NOT be changed — host apps send them verbatim.
// ------------------------------------------------------------------

/// Initial handshake. Host apps send this once on startup; we use it to
/// reset the cumulative button mask back to zero.
pub const CMD_CONNECT: u32 = 0xAF3C2828;
/// Relative mouse move. Body's `x` and `y` are signed deltas in HID units.
pub const CMD_MOUSE_MOVE: u32 = 0xAEDE7345;
/// Left mouse button state. Body's `button` is 0 (release) or non-zero (press).
pub const CMD_MOUSE_LEFT: u32 = 0x9823AE8D;
/// Right mouse button state. Same encoding as [`CMD_MOUSE_LEFT`].
pub const CMD_MOUSE_RIGHT: u32 = 0x238D8212;
/// Middle mouse button state. Same encoding as [`CMD_MOUSE_LEFT`].
pub const CMD_MOUSE_MIDDLE: u32 = 0x97A3AE8D;
/// Mouse wheel delta. Body's `wheel` is the signed scroll delta.
pub const CMD_MOUSE_WHEEL: u32 = 0xFFEEAD38;
/// Linearly-interpolated move from current position to `(x, y)` over
/// `point[0]` milliseconds. Translator spawns a worker thread.
pub const CMD_MOUSE_AUTOMOVE: u32 = 0xAEDE7346;
/// Cubic-bezier interpolated move. `point[0]` is duration_ms;
/// `point[1..=4]` are control-point coordinates `(x1, y1, x2, y2)`.
pub const CMD_BAZER_MOVE: u32 = 0xA238455A;
/// Full keyboard state report. Currently acknowledged but not decoded
/// or forwarded — the body is dropped on the floor.
pub const CMD_KEYBOARD_ALL: u32 = 0x123C2C2F;
/// Firmware reboot request. Ack-only on the translator side.
pub const CMD_REBOOT: u32 = 0xAA8855AA;
/// Toggle vendor "monitor" mode. Ack-only.
pub const CMD_MONITOR: u32 = 0x27388020;
/// Mask out specific input axes/buttons at the firmware. Ack-only.
pub const CMD_MASK_MOUSE: u32 = 0x23234343;
/// Clear all masks. Ack-only.
pub const CMD_UNMASK_ALL: u32 = 0x23344343;
/// Set firmware operating configuration. Ack-only.
pub const CMD_SETCONFIG: u32 = 0x1D3D3323;
/// Override emulated USB VID/PID. Ack-only.
pub const CMD_SETVIDPID: u32 = 0xFFED3232;
/// Toggle vendor debug mode. Ack-only.
pub const CMD_DEBUG: u32 = 0x27382021;
/// Push an image to the device's screen (KMBox B+ feature). Ack-only.
pub const CMD_SHOWPIC: u32 = 0x12334883;
/// Toggle internal trace logging. Ack-only.
pub const CMD_TRACE_ENABLE: u32 = 0xBBCDDDAC;

// ------------------------------------------------------------------
// Types
// ------------------------------------------------------------------

/// Fixed 16-byte header that prefixes every KMBox Net UDP packet.
///
/// Field layout (all little-endian, no padding):
///
/// | Offset | Size | Field      | Meaning                                              |
/// |-------:|-----:|------------|------------------------------------------------------|
/// |    0   |   4  | `mac`      | Device MAC last-4-bytes identifier; must match config. |
/// |    4   |   4  | `rand`     | Per-packet nonce; echoed back unchanged in the reply. |
/// |    8   |   4  | `indexpts` | Monotonic packet index; reply is `indexpts + 1`.     |
/// |   12   |   4  | `cmd`      | One of the `CMD_*` codes.                            |
#[derive(Debug, Clone, Copy)]
pub struct Header {
    /// Device identifier (last 4 bytes of the device MAC). Packets whose
    /// `mac` does not match the translator's configured value are dropped.
    pub mac: u32,
    /// Per-packet nonce supplied by the host app. Echoed unchanged in the
    /// reply so the host can correlate request/response.
    pub rand: u32,
    /// Monotonic packet index supplied by the host app. The reply carries
    /// `indexpts.wrapping_add(1)`, matching the vendor SDK.
    pub indexpts: u32,
    /// Command code. See the `CMD_*` constants in this module.
    pub cmd: u32,
}

/// Mouse-shaped command body (56 bytes — see [`SOFT_MOUSE_LEN`]).
///
/// The same struct carries different meanings depending on the command
/// code in the [`Header`]:
///
/// * `mouse_move` / `mouse_wheel` — `x`, `y`, `wheel` are the deltas; the
///   `button` and `point` fields are unused.
/// * `mouse_left` / `_right` / `_middle` — `button` is 0 (released) or
///   non-zero (pressed); `x`/`y`/`wheel` are unused.
/// * `mouse_automove` — `x`, `y` are the target offset; `point[0]` is the
///   duration in milliseconds.
/// * `bezier_move` — `x`, `y` are the target offset; `point[0]` is the
///   duration in milliseconds; `point[1..=4]` are the cubic-bezier
///   control points `(x1, y1, x2, y2)`.
#[derive(Debug, Clone, Copy)]
pub struct SoftMouse {
    /// Button state for the dedicated button commands; 0 = released, non-zero = pressed.
    pub button: i32,
    /// X-axis delta or target offset depending on the command.
    pub x: i32,
    /// Y-axis delta or target offset depending on the command.
    pub y: i32,
    /// Mouse-wheel delta for `mouse_wheel`; ignored by other commands.
    pub wheel: i32,
    /// Auxiliary slots used by interpolated motion: duration_ms in `[0]`,
    /// bezier control-point coordinates in `[1..=4]`. The remaining slots
    /// are reserved by the vendor SDK and ignored here.
    pub point: [i32; 10],
}

/// Map a `cmd` code to its short snake_case label (e.g. `mouse_move`).
/// Used purely for human-readable log lines. Returns `"unknown"` for any
/// code not in the table.
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
