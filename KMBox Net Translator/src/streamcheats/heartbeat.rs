//! Heartbeat thread: emits a benign 9-byte settings packet every
//! [`HEARTBEAT_INTERVAL`] so the USB-serial chip and the Windows COM
//! driver never enter an idle low-power state. Matches
//! `FirmwareInterface.py`'s `_send_heartbeat`.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use tracing::info;

use super::{build_settings_packet, DeviceSettings, PACKET_LEN};
use crate::util::translator::SerialEnvelope;

/// Heartbeat interval — matches `FirmwareInterface.py`'s 2.5 s. The
/// hardware works at this cadence in Python; if it doesn't in Rust the
/// fix isn't to spam the chip with a faster heartbeat, it's to find
/// what's different about the write path.
pub(crate) const HEARTBEAT_INTERVAL: Duration = Duration::from_millis(2500);

/// 9-byte heartbeat packet — a [`DeviceSettings::FirmwareVersion`] read
/// with value `0`. Triggers the firmware's `V: x.xx` reply line on its
/// serial output and has no HID side-effect. Byte-for-byte identical to
/// Python's `create_settings_report("FIRMWARE_VERSION", 0, 0)`. Pinned
/// in `streamcheats::device_settings::tests::heartbeat_packet_is_firmware_version_zero`.
pub(crate) const HEARTBEAT_PACKET: [u8; PACKET_LEN] =
    build_settings_packet(DeviceSettings::FirmwareVersion, 0);

/// Heartbeat loop: every [`HEARTBEAT_INTERVAL`] sends a single
/// [`HEARTBEAT_PACKET`] through the same mpsc channel the translator
/// and interpolation workers use. Matches `FirmwareInterface.py`'s
/// `_send_heartbeat` so the USB-serial chip / driver pipeline stays
/// out of any idle low-power state.
///
/// Polls in 100 ms ticks rather than sleeping for the full interval so
/// Ctrl+C shutdown is honoured within ~100 ms instead of up to 2.5 s.
pub(crate) fn heartbeat_loop(tx: Sender<SerialEnvelope>, running: Arc<AtomicBool>) {
    let mut next_at = Instant::now() + HEARTBEAT_INTERVAL;
    while running.load(Ordering::SeqCst) {
        let now = Instant::now();
        if now >= next_at {
            info!("Sending heartbeat (firmware version request)");
            if tx.send((Instant::now(), HEARTBEAT_PACKET)).is_err() {
                // Writer thread has exited — nothing more to do.
                break;
            }
            next_at = now + HEARTBEAT_INTERVAL;
        } else {
            let remaining = next_at.saturating_duration_since(now);
            thread::sleep(remaining.min(Duration::from_millis(100)));
        }
    }
}
