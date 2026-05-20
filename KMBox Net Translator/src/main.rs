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
//! # Threading model
//!
//! Five thread types are in flight at runtime:
//!
//! * **Main** — owns the UDP socket and the [`Translator`]; runs
//!   `recv_from` in a loop, parses headers, drops on MAC mismatch,
//!   dispatches via `Translator::handle_packet`, and sends the reply.
//! * **Writer** (`serial_writer_loop`) — drains the mpsc channel and
//!   calls `write_all` on the serial port.
//! * **Reader** (`serial_reader_loop`) — concurrently reads from the
//!   same port (`serial2` supports concurrent read+write on `&self`),
//!   buffers by `\n`, and emits `IN (COMx):` lines.
//! * **Heartbeat** (`heartbeat_loop`) — every [`HEARTBEAT_INTERVAL`]
//!   pushes a benign settings packet through the mpsc channel so the
//!   USB-serial chip never goes idle.
//! * **Interpolation workers** — short-lived, spawned per
//!   `cmd_mouse_automove` / `cmd_bezier_move`; emit delta packets at
//!   `STEP_MS = 4 ms` cadence and then exit.
//!
//! The serial port is wrapped in an `Arc<serial2::SerialPort>` and
//! shared by the writer and reader threads — no `try_clone` of OS
//! handles needed. The button mask is the only other shared mutable
//! state, held in `Arc<Mutex<u8>>`.
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
//!
//! With `enable_timing_logs: true` in `config.json`, the `IN (KMBOX NET)`
//! lines also carry a `parse=Nµs` suffix and the `OUT (COMx)` lines
//! carry `(lat=X.YYms q=A.BBms w=C.DDms)` — `lat` total origin → wire,
//! `q` mpsc-queue wait, `w` the `write_all` syscall duration.

mod kmbox_net;
mod streamcheats;
mod util;

use std::env;
use std::net::{SocketAddr, UdpSocket};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, Sender};
use std::sync::Arc;
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use tracing::{error, info, warn};
use tracing_subscriber::EnvFilter;

use crate::streamcheats::PACKET_LEN;
use crate::util::settings::{load_or_create, LoadOutcome, Settings};
use crate::util::translator::{SerialEnvelope, Translator};

/// Heartbeat interval — matches `FirmwareInterface.py`'s 2.5 s. The
/// hardware works at this cadence in Python; if it doesn't in Rust the
/// fix isn't to spam the chip with a faster heartbeat, it's to find
/// what's different about the write path.
const HEARTBEAT_INTERVAL: Duration = Duration::from_millis(2500);

/// 9-byte heartbeat packet: length prefix `0x03` routes the firmware
/// to its settings handler, `setting_id=0` is `FIRMWARE_VERSION` (read
/// only — triggers a `V: x.xx` reply, no HID side-effect), and the
/// remaining bytes are zero-padded. Byte-for-byte identical to Python's
/// `create_settings_report("FIRMWARE_VERSION", 0, 0)`.
const HEARTBEAT_PACKET: [u8; PACKET_LEN] =
    [0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00];

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
    let mut serial = serial2::SerialPort::open(&settings.com_port, settings.baud_rate)
        .with_context(|| {
            format!(
                "opening serial port {} @ {}",
                settings.com_port, settings.baud_rate
            )
        })?;

    // 2 second READ timeout matches the Python reference
    // (`serial.Serial(..., timeout=2)`). Bounds the reader thread's
    // blocking `read()` calls so shutdown is responsive.
    serial
        .set_read_timeout(Duration::from_secs(2))
        .context("setting serial read timeout")?;

    // ZERO write timeout is what pyserial uses by default
    // (`write_timeout=None` → COMMTIMEOUTS WriteTotalTimeoutConstant = 0
    // = no write timeout, blocks until the bytes are physically out).
    // For a 9-byte packet at 115200 baud that's ~780 µs.
    serial
        .set_write_timeout(Duration::ZERO)
        .context("setting serial write timeout")?;

    // Match pyserial's default open behaviour: assert DTR and RTS. USB
    // serial chips (FT232H, CH340) treat DTR low as "host has gone"
    // and may enter low-power states. pyserial's DCB sets
    // fDtrControl / fRtsControl = ENABLE; serial2 leaves them at the
    // driver default. Forcing them HIGH keeps the chip alive.
    if let Err(e) = serial.set_dtr(true) {
        warn!("could not assert DTR on {}: {}", settings.com_port, e);
    }
    if let Err(e) = serial.set_rts(true) {
        warn!("could not assert RTS on {}: {}", settings.com_port, e);
    }

    // serial2 read/write take &self, so we can share one handle across
    // the writer and reader threads via Arc — no try_clone (separate OS
    // handles) needed.
    let serial = Arc::new(serial);
    let writer_serial = serial.clone();
    let reader_serial = serial.clone();

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
    // Carries (Instant, packet) envelopes so the writer can compute
    // per-packet latency at log time.
    let (serial_tx, serial_rx) = mpsc::channel::<SerialEnvelope>();

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
    let writer_timing = settings.enable_timing_logs;
    let writer_thread = thread::spawn(move || {
        serial_writer_loop(
            &writer_serial,
            &writer_port,
            serial_rx,
            writer_running,
            writer_timing,
        );
    });

    // Spawn serial reader — logs every line the firmware emits.
    let reader_running = running.clone();
    let reader_port = settings.com_port.clone();
    let reader_thread = thread::spawn(move || {
        serial_reader_loop(&reader_serial, &reader_port, reader_running);
    });

    // Spawn heartbeat keepalive — emits a benign packet every
    // HEARTBEAT_INTERVAL so the USB-serial chip never goes cold.
    let heartbeat_running = running.clone();
    let heartbeat_tx = serial_tx.clone();
    let heartbeat_thread = thread::spawn(move || {
        heartbeat_loop(heartbeat_tx, heartbeat_running);
    });

    let translator = Translator::new(
        settings.device_mac,
        settings.enable_timing_logs,
        serial_tx,
    );

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
    let _ = heartbeat_thread.join();
    let _ = writer_thread.join();
    let _ = reader_thread.join();
    Ok(())
}

/// Heartbeat loop: every [`HEARTBEAT_INTERVAL`] sends a single
/// [`HEARTBEAT_PACKET`] through the same mpsc channel the translator
/// and interpolation workers use. Matches `FirmwareInterface.py`'s
/// `_send_heartbeat` so the USB-serial chip / driver pipeline stays
/// out of any idle low-power state.
///
/// Polls in 100 ms ticks rather than sleeping for the full interval so
/// Ctrl+C shutdown is honoured within ~100 ms instead of up to 2.5 s.
fn heartbeat_loop(tx: Sender<SerialEnvelope>, running: Arc<AtomicBool>) {
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
fn serial_writer_loop(
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

/// Reader loop: collects bytes from the firmware, splits on newline, and
/// logs every complete line as `IN (<port>): <text>`. Non-printable bytes
/// are rendered as `\xHH` so binary noise stays readable. A 4 KiB
/// safety cap forces a flush if the firmware ever omits a newline.
fn serial_reader_loop(
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
