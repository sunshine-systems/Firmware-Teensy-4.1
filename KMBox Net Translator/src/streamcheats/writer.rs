//! Serial writer thread: drains the mpsc channel and writes each
//! [`SerialEnvelope`]'s 9-byte packet to the serial port. Emits the
//! `OUT (COMx):` log line, with an optional latency suffix when timing
//! is enabled.
//!
//! [`SerialEnvelope`]: crate::util::translator::SerialEnvelope

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::Arc;
use std::time::{Duration, Instant};

use tracing::{error, info, warn};

use super::format::hex_bytes;
use super::heartbeat::HEARTBEAT_PACKET;
use crate::util::translator::{SerialEnvelope, SerialTxHolder};

/// Number of consecutive **heartbeat** `write_all` failures before the
/// writer declares the session dead and tears it down. Heartbeats fire
/// every 2.5 s, so 3 strikes is a deterministic ~7.5 s grace window
/// independent of how active the user is — a failed mouse packet is
/// logged and dropped but never counts toward the disconnect threshold.
/// The heartbeat is the canonical "is the device alive" signal so the
/// counter follows it rather than total write traffic.
const HEARTBEAT_FAILURE_THRESHOLD: u32 = 3;

/// Writer loop: pulls envelopes (origin instant + 9-byte packet) off the
/// mpsc channel and writes each packet to the serial port. Logs successful
/// writes as `OUT (<port>): <hex>` with an optional timing suffix
/// (`lat=X.Yms q=A.Bms w=C.Dms`) when `enable_timing` is `true`. The lat
/// component measures the full origin → wire delay; q is queue wait
/// before this thread dequeued; w is the `write_all` call duration.
///
/// Polls the receiver with a 50 ms timeout so the thread stays responsive
/// to a shutdown request without busy-spinning. We do NOT call `flush()`
/// per packet — `write_all` already blocks until the bytes are out of
/// the OS buffer; explicitly flushing on top of that just adds latency
/// for no benefit (pyserial doesn't flush either).
///
/// On reaching [`HEARTBEAT_FAILURE_THRESHOLD`] the writer runs the
/// disconnect SOP — clear `holder` first so the translator, the
/// heartbeat, and any interpolation worker stop pushing fresh packets,
/// then drain the receiver so anything queued during the failure window
/// doesn't surface on the next session. Channel disconnect (all senders
/// dropped, only happens at program shutdown) is a clean exit with no
/// SOP needed. Non-heartbeat write failures (mouse packets, settings)
/// are logged and dropped but never count toward the threshold — only
/// heartbeats decide session liveness.
pub(crate) fn serial_writer_loop(
    serial: &serial2::SerialPort,
    port_name: &str,
    rx: Receiver<SerialEnvelope>,
    holder: SerialTxHolder,
    running: Arc<AtomicBool>,
    enable_timing: bool,
) {
    let mut consecutive_heartbeat_failures: u32 = 0;

    while running.load(Ordering::SeqCst) {
        match rx.recv_timeout(Duration::from_millis(50)) {
            Ok((origin, pkt)) => {
                let dequeued = Instant::now();
                let is_heartbeat = pkt == HEARTBEAT_PACKET;
                match serial.write_all(&pkt) {
                    Ok(()) => {
                        if is_heartbeat {
                            consecutive_heartbeat_failures = 0;
                        }
                        let written = Instant::now();
                        if enable_timing {
                            let lat_ms = written.duration_since(origin).as_secs_f64() * 1000.0;
                            let q_ms = dequeued.duration_since(origin).as_secs_f64() * 1000.0;
                            let w_ms = written.duration_since(dequeued).as_secs_f64() * 1000.0;
                            info!(
                                "OUT ({}): {} (lat={:.2}ms q={:.2}ms w={:.2}ms)",
                                port_name,
                                hex_bytes(&pkt),
                                lat_ms,
                                q_ms,
                                w_ms
                            );
                        } else {
                            info!("OUT ({}): {}", port_name, hex_bytes(&pkt));
                        }
                    }
                    Err(e) => {
                        if is_heartbeat {
                            consecutive_heartbeat_failures += 1;
                            warn!(
                                "heartbeat write failed on {} ({}/{}): {}",
                                port_name,
                                consecutive_heartbeat_failures,
                                HEARTBEAT_FAILURE_THRESHOLD,
                                e
                            );
                            if consecutive_heartbeat_failures >= HEARTBEAT_FAILURE_THRESHOLD {
                                error!(
                                    "{} consecutive heartbeat failures on {} — ending session",
                                    HEARTBEAT_FAILURE_THRESHOLD, port_name
                                );
                                // SOP: clear holder *first* so every
                                // sender (translator, heartbeat, interp
                                // workers) sees `None` on its next
                                // attempt and stops.
                                *holder.lock().unwrap() = None;
                                // Then drain any envelopes already in
                                // flight so the next session doesn't
                                // inherit stale packets the moment its
                                // writer starts up.
                                let mut drained = 0usize;
                                while rx.try_recv().is_ok() {
                                    drained += 1;
                                }
                                if drained > 0 {
                                    info!(
                                        "Dropped {} pending packet(s) on disconnect",
                                        drained
                                    );
                                }
                                return;
                            }
                        } else {
                            // Non-heartbeat failure: log and drop the
                            // packet. Liveness is the heartbeat's call,
                            // not ours.
                            warn!(
                                "serial write failed on {} (mouse/settings packet dropped): {}",
                                port_name, e
                            );
                        }
                    }
                }
            }
            Err(mpsc::RecvTimeoutError::Timeout) => {}
            Err(mpsc::RecvTimeoutError::Disconnected) => break,
        }
    }
    // One flush on shutdown so any tail bytes leave the hardware FIFO.
    let _ = serial.flush();
}
