//! Windows PC-side bridge between third-party KMBox Net host apps and the
//! Teensy USB Host Proxy firmware.
//!
//! Listens for KMBox Net UDP commands on a configurable address/port,
//! translates each one into the Streamcheats firmware's 9-byte binary
//! serial protocol, and forwards the resulting packets to a Teensy 4.1
//! over a configured COM port. The Teensy injects HID mouse events into
//! the target PC; this process is invisible to the host app, which
//! continues to see normal KMBox Net replies.
//!
//! ```text
//! host app  --UDP (KMBox Net)-->  THIS TRANSLATOR  --serial (9-byte binary)-->  Teensy proxy
//! ```
//!
//! # Module layout
//!
//! * [`kmbox_net`] — incoming wire format (16-byte LE header + body) and
//!   parsing. Start in [`kmbox_net::schema`] for the type and command
//!   tables, [`kmbox_net::parser`] for the decoders.
//! * [`streamcheats`] — outgoing 9-byte mouse packet builder for the
//!   Teensy firmware. The byte-by-byte layout lives at the top of
//!   [`streamcheats::packet`].
//! * [`util`] — runtime glue: persisted [`util::settings`] loader and the
//!   [`util::translator::Translator`] state machine that holds the
//!   cumulative button mask and dispatches each incoming command.
//!
//! # Log channels
//!
//! All structured log lines emitted by the translator carry one of three
//! channel prefixes so the direction of every event is unambiguous:
//!
//! * `IN (KMBOX NET):` — a UDP datagram arrived from a host app and was
//!   accepted. The remainder names the decoded command and its arguments.
//! * `OUT (COMx):` — a 9-byte Streamcheats packet was written to the
//!   serial port. The remainder is the raw hex.
//! * `IN (COMx):` — a newline-terminated line was received from the
//!   firmware. Non-printable bytes are escaped as `\xHH`.

mod kmbox_net;
mod streamcheats;
mod util;

use std::env;
use std::net::{SocketAddr, UdpSocket};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver};
use std::sync::Arc;
use std::thread;
use std::time::Duration;

use anyhow::{Context, Result};
use tracing::{error, info, warn};
use tracing_subscriber::EnvFilter;

use crate::streamcheats::PACKET_LEN;
use crate::util::settings::{load_or_create, LoadOutcome, Settings};
use crate::util::translator::Translator;

/// Initialise `tracing_subscriber` with an `info`-level default filter and
/// enable ANSI escape-sequence processing on Windows consoles so colour
/// codes render rather than printing literally. `RUST_LOG` overrides the
/// default filter.
fn init_logging() {
    // Enable VT escape-sequence processing on Windows so the ANSI colors
    // tracing-subscriber emits actually render instead of printing
    // literally as `[2m...[0m`. No-op on non-Windows.
    #[cfg(windows)]
    let _ = enable_ansi_support::enable_ansi_support();

    let filter =
        EnvFilter::try_from_default_env().unwrap_or_else(|_| EnvFilter::new("info"));
    tracing_subscriber::fmt()
        .with_env_filter(filter)
        .with_target(false)
        .with_ansi(true)
        .init();
}

fn main() -> ExitCode {
    init_logging();

    let cwd = match env::current_dir() {
        Ok(p) => p,
        Err(e) => {
            eprintln!("could not resolve current directory: {}", e);
            return ExitCode::from(1);
        }
    };

    let settings = match load_or_create(&cwd) {
        Ok(LoadOutcome::Loaded(s)) => s,
        Ok(LoadOutcome::WroteDefault { path, reason }) => {
            match reason {
                None => {
                    println!(
                        "Created default config.json — please edit listen_addr and com_port, then re-run."
                    );
                }
                Some(r) => {
                    println!(
                        "config.json was structurally unusable ({}). Regenerating defaults.",
                        r
                    );
                    println!(
                        "Wrote fresh default to {} — please edit listen_addr and com_port, then re-run.",
                        path.display()
                    );
                }
            }
            return ExitCode::from(1);
        }
        Ok(LoadOutcome::Invalid { path, reason }) => {
            println!("config.json has an invalid value: {}", reason);
            println!(
                "Edit {} and re-run. (The file was NOT rewritten — your other settings are preserved.)",
                path.display()
            );
            return ExitCode::from(1);
        }
        Err(e) => {
            eprintln!("fatal: {:?}", e);
            return ExitCode::from(1);
        }
    };

    match run(settings) {
        Ok(()) => ExitCode::from(0),
        Err(e) => {
            error!("fatal: {:?}", e);
            ExitCode::from(1)
        }
    }
}

/// Main service loop: opens the serial port, binds the UDP socket, spawns
/// the serial reader and writer threads, and dispatches every incoming
/// UDP datagram through a [`Translator`]. Returns when the Ctrl+C handler
/// flips the shared `running` flag.
fn run(settings: Settings) -> Result<()> {
    // Open serial first — fail fast if it's wrong.
    // 2 second timeout matches the Python reference (sunbox_interface's
    // ArduinoInterface uses `serial.Serial(..., timeout=2)`). It only
    // bounds reads — writes still complete as fast as the OS will allow.
    let mut serial = serialport::new(&settings.com_port, settings.baud_rate)
        .timeout(Duration::from_secs(2))
        .open()
        .with_context(|| {
            format!(
                "opening serial port {} @ {}",
                settings.com_port, settings.baud_rate
            )
        })?;

    // Clone the handle so a reader thread can pull bytes from the firmware
    // independently of the writer thread.
    let mut serial_reader = serial
        .try_clone()
        .with_context(|| format!("cloning serial port handle for {}", settings.com_port))?;

    let bind: SocketAddr = SocketAddr::new(settings.listen_addr, settings.udp_port);
    let socket = UdpSocket::bind(bind)
        .with_context(|| format!("binding UDP socket at {}", bind))?;
    socket
        .set_read_timeout(Some(Duration::from_millis(250)))
        .context("setting UDP read timeout")?;

    info!(
        "Listening on {}:{}, forwarding to {} @ {}, mac={}",
        settings.listen_addr,
        settings.udp_port,
        settings.com_port,
        settings.baud_rate,
        settings.device_mac_str
    );

    // mpsc channel: translator + workers -> serial writer thread.
    let (serial_tx, serial_rx) = mpsc::channel::<[u8; PACKET_LEN]>();

    let running = Arc::new(AtomicBool::new(true));
    {
        let r = running.clone();
        ctrlc::set_handler(move || {
            r.store(false, Ordering::SeqCst);
        })
        .context("installing Ctrl+C handler")?;
    }

    // Spawn serial writer.
    let writer_running = running.clone();
    let writer_port = settings.com_port.clone();
    let writer_thread = thread::spawn(move || {
        serial_writer_loop(&mut *serial, &writer_port, serial_rx, writer_running);
    });

    // Spawn serial reader — logs every line the firmware emits.
    let reader_running = running.clone();
    let reader_port = settings.com_port.clone();
    let reader_thread = thread::spawn(move || {
        serial_reader_loop(&mut *serial_reader, &reader_port, reader_running);
    });

    let translator = Translator::new(settings.device_mac, serial_tx);

    let mut buf = [0u8; 2048];
    while running.load(Ordering::SeqCst) {
        match socket.recv_from(&mut buf) {
            Ok((n, peer)) => {
                let datagram = &buf[..n];
                if let Some(reply) = translator.handle_packet(datagram) {
                    if let Err(e) = socket.send_to(&reply, peer) {
                        warn!("failed to send reply to {}: {}", peer, e);
                    }
                }
            }
            Err(e) => match e.kind() {
                std::io::ErrorKind::WouldBlock | std::io::ErrorKind::TimedOut => {
                    // timeout — loop and re-check `running`.
                }
                _ => {
                    warn!("recv_from error: {}", e);
                }
            },
        }
    }

    info!("shutdown requested — closing serial and exiting");
    drop(translator);
    let _ = writer_thread.join();
    let _ = reader_thread.join();
    Ok(())
}

/// Writer loop: pulls 9-byte packets off the mpsc channel and writes each
/// to the serial port. Logs successful writes as `OUT (<port>): <hex>`.
/// Polls the receiver with a 50 ms timeout so the thread stays responsive
/// to a shutdown request without busy-spinning. See the in-body comment
/// for why we deliberately do NOT call `flush()` per packet on Windows.
fn serial_writer_loop(
    serial: &mut dyn serialport::SerialPort,
    port_name: &str,
    rx: Receiver<[u8; PACKET_LEN]>,
    running: Arc<AtomicBool>,
) {
    // Short poll so the thread stays responsive to shutdown. We do NOT
    // call `serial.flush()` per packet on Windows: serialport-rs's flush
    // maps to `FlushFileBuffers`, which waits for the USB-serial chip's
    // hardware TX FIFO to drain to the wire — that adds ~400-500 ms per
    // 9-byte packet on FT232H/CH340. pyserial's `flush()` on Windows only
    // drains the OS write buffer (a different syscall), so "matching
    // Python" by flushing here would actually be slower than Python.
    // The OS driver pushes bytes out on its own latency timer (~16 ms
    // FTDI default), well within mouse-input requirements.
    while running.load(Ordering::SeqCst) {
        match rx.recv_timeout(Duration::from_millis(50)) {
            Ok(pkt) => {
                match serial.write_all(&pkt) {
                    Ok(()) => {
                        info!("OUT ({}): {}", port_name, hex_bytes(&pkt));
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

/// Reader loop: collects bytes from the firmware, splits on newline, and
/// logs every complete line as `IN (<port>): <text>`. Non-printable bytes
/// are rendered as `\xHH` so binary noise stays readable. A 4 KiB
/// safety cap forces a flush if the firmware ever omits a newline.
fn serial_reader_loop(
    serial: &mut dyn serialport::SerialPort,
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

/// Emit a single firmware line as `IN (<port>): <rendered>`. All-NUL
/// lines (a common transient when the FTDI driver is settling) are
/// discarded so they don't pollute the log.
fn flush_line(port_name: &str, line: &[u8]) {
    if line.iter().all(|&b| b == 0) {
        return; // ignore all-NUL noise
    }
    info!("IN ({}): {}", port_name, render_line(line));
}

/// Render a byte slice as text, escaping non-printable bytes as `\xHH`.
fn render_line(bytes: &[u8]) -> String {
    let mut out = String::with_capacity(bytes.len());
    for &b in bytes {
        if (0x20..=0x7E).contains(&b) {
            out.push(b as char);
        } else {
            out.push_str(&format!("\\x{:02X}", b));
        }
    }
    out
}

/// Render a byte slice as space-separated uppercase hex (e.g. `08 01 00 ...`).
/// Used to log outbound Streamcheats packets.
fn hex_bytes(bytes: &[u8]) -> String {
    let mut s = String::with_capacity(bytes.len() * 3);
    for (i, b) in bytes.iter().enumerate() {
        if i > 0 {
            s.push(' ');
        }
        s.push_str(&format!("{:02X}", b));
    }
    s
}
