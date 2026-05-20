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

use tracing::{info, warn};

use crate::format::hex_bytes;
use crate::util::translator::SerialEnvelope;

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
pub(crate) fn serial_writer_loop(
    serial: &serial2::SerialPort,
    port_name: &str,
    rx: Receiver<SerialEnvelope>,
    running: Arc<AtomicBool>,
    enable_timing: bool,
) {
    while running.load(Ordering::SeqCst) {
        match rx.recv_timeout(Duration::from_millis(50)) {
            Ok((origin, pkt)) => {
                let dequeued = Instant::now();
                match serial.write_all(&pkt) {
                    Ok(()) => {
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
                        warn!("serial write failed ({} bytes): {}", pkt.len(), e);
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
