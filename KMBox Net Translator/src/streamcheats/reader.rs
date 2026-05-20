//! Serial reader thread: reads from the serial port concurrently with
//! the writer (serial2 supports `&self` on both directions), buffers
//! bytes by newline, and emits one `IN (COMx):` log line per complete
//! firmware response line. Non-printable bytes are escaped as `\xHH`.

use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use tracing::warn;

use super::format::flush_line;

/// Reader loop: collects bytes from the firmware, splits on newline, and
/// logs every complete line as `IN (<port>): <text>`. Non-printable bytes
/// are rendered as `\xHH` so binary noise stays readable. A 4 KiB
/// safety cap forces a flush if the firmware ever omits a newline.
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
                warn!("serial read error on {}: {}", port_name, e);
                // Brief pause to avoid hot-spinning on a broken port.
                thread::sleep(Duration::from_millis(100));
            }
        }
    }

    // Flush any partial trailing line on shutdown.
    if !line.is_empty() {
        flush_line(port_name, &line);
    }
}
