//! Serial reader thread: reads from the serial port concurrently with
//! the writer (serial2 supports `&self` on both directions), buffers
//! bytes by newline, and emits one `IN (COMx):` log line per complete
//! firmware response line. Non-printable bytes are escaped as `\xHH`.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;

use tracing::warn;

use super::format::flush_line;

/// Reader loop: collects bytes from the firmware, splits on newline, and
/// logs every complete line as `IN (<port>): <text>`. Non-printable bytes
/// are rendered as `\xHH` so binary noise stays readable. A 4 KiB
/// safety cap forces a flush if the firmware ever omits a newline.
///
/// Exits when the per-session `running` flag flips to `false` OR when
/// `read` returns a non-recoverable error (which on Windows USB-CDC is
/// the usual "device unplugged" path). Either way the supervisor joins
/// this thread, drops the port, and goes back to scanning.
pub(crate) fn serial_reader_loop(
    serial: &serial2::SerialPort,
    port_name: &str,
    running: Arc<AtomicBool>,
) {
    let mut buf = [0u8; 256];
    let mut line: Vec<u8> = Vec::with_capacity(256);

    while running.load(Ordering::SeqCst) {
        match serial.read(&mut buf) {
            Ok(0) => {}
            Ok(n) => {
                for &b in &buf[..n] {
                    if b == b'\n' {
                        flush_line(port_name, &line);
                        line.clear();
                    } else if b == b'\r' {
                        // swallow — handled together with the following LF
                    } else {
                        line.push(b);
                        // Guard against runaway buffer if the firmware
                        // never sends a newline.
                        if line.len() > 4096 {
                            flush_line(port_name, &line);
                            line.clear();
                        }
                    }
                }
            }
            Err(e) if e.kind() == std::io::ErrorKind::TimedOut => {}
            Err(e) if e.kind() == std::io::ErrorKind::Interrupted => {}
            Err(e) => {
                // Anything else (BrokenPipe, NotConnected, Os{...}) is
                // almost certainly "the user unplugged the Teensy".
                // Don't hot-spin retrying on a dead handle — bail and
                // let the supervisor's session-teardown path reclaim
                // us.
                warn!("serial read error on {}: {} — ending session", port_name, e);
                break;
            }
        }
    }

    // Flush any partial trailing line on shutdown.
    if !line.is_empty() {
        flush_line(port_name, &line);
    }
}
