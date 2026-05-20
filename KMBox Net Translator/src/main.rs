//! Windows PC-side bridge between third-party KMBox Net host apps and the
//! Teensy USB Host Proxy firmware.
//!
//! Listens for KMBox Net UDP commands on a configurable address/port,
//! translates each one into the Streamcheats firmware's 9-byte binary
//! serial protocol, and forwards the resulting packets to a Teensy 4.1
//! over an auto-discovered USB-CDC COM port. The Teensy injects HID
//! mouse events into the target PC; this process is invisible to the
//! host app, which continues to see normal KMBox Net replies.
//!
//! ```text
//! host app  --UDP (KMBox Net)-->  THIS TRANSLATOR  --serial (9-byte binary)-->  Teensy proxy
//! ```
//!
//! # Module layout
//!
//! * [`kmbox_net`] — incoming KMBox Net UDP protocol: wire types
//!   ([`kmbox_net::schema`]) and decoders ([`kmbox_net::parser`]).
//! * [`streamcheats`] — outgoing Streamcheats firmware protocol AND the
//!   threads that own the serial port. Packet builders
//!   ([`streamcheats::packet`], [`streamcheats::device_settings`]),
//!   the [`streamcheats::discovery`] auto-finder, plus the serial
//!   [`streamcheats::writer`], [`streamcheats::reader`], and
//!   [`streamcheats::heartbeat`] threads. Log render helpers live in
//!   [`streamcheats::format`].
//! * [`util`] — device-agnostic glue: persisted [`util::settings`]
//!   loader and the [`util::translator::Translator`] state machine that
//!   holds the cumulative button mask and dispatches each incoming
//!   command.
//!
//! # Threading model
//!
//! The UDP socket, the [`Translator`], and the heartbeat thread are
//! **permanent** — they exist for the entire program lifetime regardless
//! of whether a Teensy is currently plugged in. The serial reader and
//! writer are **per-session** — they're spawned each time the supervisor
//! discovers a device and torn down when that device disconnects.
//!
//! At runtime the threads are:
//!
//! * **Main** — owns the UDP socket and the [`Translator`]; runs
//!   `recv_from` in a loop, parses headers, drops on MAC mismatch,
//!   dispatches via `Translator::handle_packet`, and sends the reply.
//!   This thread keeps running across disconnect/reconnect cycles.
//! * **Supervisor** — owns the discovery loop. While running, it
//!   alternates between calling [`streamcheats::discovery::discover_device`]
//!   and spawning a writer + reader pair around the port it finds.
//!   On writer exit (device unplugged) it joins both threads, clears
//!   the translator's serial sender, and rescans.
//! * **Writer** (`serial_writer_loop`) — per-session. Drains the mpsc
//!   channel and `write_all`s to the port. Returns after 3 consecutive
//!   *heartbeat* write failures (~7.5 s of port silence), having first
//!   cleared the [`SerialTxHolder`] and drained any in-flight envelopes
//!   so the next session starts clean. Non-heartbeat write failures
//!   are logged and the packet dropped, but never count toward the
//!   disconnect threshold — only heartbeats decide session liveness.
//! * **Reader** (`serial_reader_loop`) — per-session. Concurrent read
//!   on the same `Arc<SerialPort>` (serial2 supports `&self` on both
//!   directions), buffers by `\n`, emits `IN (COMx):` lines.
//! * **Heartbeat** (`heartbeat_loop`) — permanent. Every
//!   [`HEARTBEAT_INTERVAL`] checks the swappable sender holder and
//!   pushes a benign settings packet through it if a session is
//!   currently active.
//! * **Interpolation workers** — short-lived, spawned per
//!   `cmd_mouse_automove` / `cmd_bezier_move`; emit delta packets at
//!   `STEP_MS = 4 ms` cadence and then exit.
//!
//! The serial sender lives in a [`SerialTxHolder`] (an
//! `Arc<Mutex<Option<Sender<SerialEnvelope>>>>`) that's shared between
//! the translator, the heartbeat, and the supervisor. When the holder
//! is `None`, the translator silently drops outbound serial packets but
//! still returns the UDP reply, so host apps don't stall waiting on a
//! translator whose downstream device has gone away.
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
//!
//! [`HEARTBEAT_INTERVAL`]: crate::streamcheats::heartbeat::HEARTBEAT_INTERVAL
//! [`SerialTxHolder`]: crate::util::translator::SerialTxHolder

mod kmbox_net;
mod streamcheats;
mod util;

use std::env;
use std::net::{SocketAddr, UdpSocket};
use std::process::ExitCode;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use anyhow::{Context, Result};
use tracing::{error, info, warn};
use tracing_subscriber::EnvFilter;

use crate::streamcheats::discovery::discover_device;
use crate::util::settings::{load_or_create, LoadOutcome, Settings};
use crate::util::translator::{SerialEnvelope, SerialTxHolder, Translator};

/// How long [`discover_device`] listens on each port per pass before
/// declaring nothing matched. The firmware sends `I:` info lines roughly
/// once per second and `S:` startup banner immediately on connect, so 5 s
/// is comfortable headroom without making the user wait long when no
/// device is attached.
const DISCOVERY_PROBE_SECS: u64 = 5;

/// Backoff between unsuccessful discovery passes. Polled in 100 ms
/// increments so Ctrl+C is responsive even mid-sleep.
const DISCOVERY_BACKOFF: Duration = Duration::from_secs(10);

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
                        "Created default config.json — please edit listen_addr, then re-run."
                    );
                }
                Some(r) => {
                    println!(
                        "config.json was structurally unusable ({}). Regenerating defaults.",
                        r
                    );
                    println!(
                        "Wrote fresh default to {} — please edit listen_addr, then re-run.",
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

/// Sleep for `total`, but wake every 100 ms to check `running`. Returns
/// `true` if the sleep completed normally, `false` if `running` flipped
/// to `false` mid-sleep (so the caller can break out of its outer loop).
fn interruptible_sleep(total: Duration, running: &Arc<AtomicBool>) -> bool {
    let deadline = Instant::now() + total;
    while running.load(Ordering::SeqCst) {
        let now = Instant::now();
        if now >= deadline {
            return true;
        }
        thread::sleep((deadline - now).min(Duration::from_millis(100)));
    }
    false
}

/// Configure a freshly-opened port the way every session expects:
/// 2 s read timeout, zero write timeout, DTR+RTS asserted. Matches the
/// pyserial defaults (`timeout=2`, `write_timeout=None` →
/// `WriteTotalTimeoutConstant=0` = block until physically out) and keeps
/// USB-serial chips (FT232H, CH340) from entering low-power states.
fn configure_session_port(port: &mut serial2::SerialPort, port_name: &str) -> Result<()> {
    port.set_read_timeout(Duration::from_secs(2))
        .context("setting serial read timeout")?;
    port.set_write_timeout(Duration::ZERO)
        .context("setting serial write timeout")?;
    if let Err(e) = port.set_dtr(true) {
        warn!("could not assert DTR on {}: {}", port_name, e);
    }
    if let Err(e) = port.set_rts(true) {
        warn!("could not assert RTS on {}: {}", port_name, e);
    }
    Ok(())
}

/// Supervisor loop: alternates between discovery (find a Teensy) and
/// session (spawn writer + reader around it) until the shared `running`
/// flag flips. When `discover_device` returns `None`, sleep
/// [`DISCOVERY_BACKOFF`] then try again. When it returns `Some`,
/// configure the port, publish the writer's sender to the holder so the
/// translator and heartbeat start forwarding, wait for the writer to
/// exit (either on disconnect or shutdown), then tear down: clear the
/// holder, signal the reader to stop, join both threads, drop the port.
fn supervisor_loop(
    holder: SerialTxHolder,
    running: Arc<AtomicBool>,
    enable_timing: bool,
) {
    info!("Scanning available COM ports for firmware...");
    while running.load(Ordering::SeqCst) {
        match discover_device(DISCOVERY_PROBE_SECS) {
            None => {
                info!("No device found, will try again in 10 seconds");
                if !interruptible_sleep(DISCOVERY_BACKOFF, &running) {
                    break;
                }
                continue;
            }
            Some((port_name, mut port)) => {
                info!("Found device on {}", port_name);
                if let Err(e) = configure_session_port(&mut port, &port_name) {
                    error!("could not configure {}: {} — rescanning", port_name, e);
                    continue;
                }
                let port = Arc::new(port);
                let writer_port = port.clone();
                let reader_port = port.clone();

                // Per-session running flag for the reader. Flips when
                // the writer has exited (the disconnect signal) OR when
                // the global `running` flag flips for shutdown.
                let session_running = Arc::new(AtomicBool::new(true));

                let (tx, rx) = mpsc::channel::<SerialEnvelope>();
                // Publish the sender so the translator and heartbeat
                // start delivering packets. Done BEFORE spawning the
                // writer so we don't race against the first inbound
                // UDP datagram during port handoff.
                *holder.lock().unwrap() = Some(tx);

                let writer_running = running.clone();
                let writer_port_name = port_name.clone();
                let writer_holder = holder.clone();
                let writer_thread = thread::spawn(move || {
                    crate::streamcheats::writer::serial_writer_loop(
                        &writer_port,
                        &writer_port_name,
                        rx,
                        writer_holder,
                        writer_running,
                        enable_timing,
                    );
                });

                let reader_running = session_running.clone();
                let reader_port_name = port_name.clone();
                let reader_thread = thread::spawn(move || {
                    crate::streamcheats::reader::serial_reader_loop(
                        &reader_port,
                        &reader_port_name,
                        reader_running,
                    );
                });

                // Writer exits on 3 consecutive heartbeat failures
                // (device unplugged, ~7.5 s) or on global shutdown. On
                // the disconnect path it has already run the SOP —
                // cleared the holder and drained the channel — so the
                // line below is a belt-and-braces no-op there. On
                // graceful shutdown the writer leaves the holder alone,
                // and this line is the one that clears it.
                let _ = writer_thread.join();
                *holder.lock().unwrap() = None;

                // Signal the reader to stop and join it. The reader
                // may also have exited already on its own (read error
                // when the device went away); either way the join
                // resolves quickly.
                session_running.store(false, Ordering::SeqCst);
                let _ = reader_thread.join();

                // Drop the Arc<SerialPort> by letting it fall out of
                // scope — both threads have joined so the writer/reader
                // clones are gone; this last one closes the handle.
                drop(port);

                if running.load(Ordering::SeqCst) {
                    info!("Device on {} disconnected — rescanning", port_name);
                }
            }
        }
    }
}

/// Main service loop: binds the UDP socket, builds the permanent
/// [`Translator`], spawns the supervisor + heartbeat threads, and
/// dispatches every incoming UDP datagram through the translator.
/// Returns when the Ctrl+C handler flips the shared `running` flag.
fn run(settings: Settings) -> Result<()> {
    let bind: SocketAddr = SocketAddr::new(settings.listen_addr, settings.udp_port);
    let socket = UdpSocket::bind(bind)
        .with_context(|| format!("binding UDP socket at {}", bind))?;
    socket
        .set_read_timeout(Some(Duration::from_millis(250)))
        .context("setting UDP read timeout")?;

    info!(
        "Listening on {}:{}, mac={}",
        settings.listen_addr, settings.udp_port, settings.device_mac_str
    );

    let running = Arc::new(AtomicBool::new(true));
    {
        let r = running.clone();
        ctrlc::set_handler(move || {
            r.store(false, Ordering::SeqCst);
        })
        .context("installing Ctrl+C handler")?;
    }

    // Swappable serial sender — populated by the supervisor whenever a
    // session is active, cleared on disconnect. The translator and
    // heartbeat both hold clones; while `None`, the translator drops
    // outbound packets silently and the heartbeat skips its tick.
    let serial_tx_holder: SerialTxHolder = Arc::new(Mutex::new(None));

    // Heartbeat is permanent — it lives across all sessions and
    // automatically idles when no session is active.
    let heartbeat_running = running.clone();
    let heartbeat_holder = serial_tx_holder.clone();
    let heartbeat_thread = thread::spawn(move || {
        crate::streamcheats::heartbeat::heartbeat_loop(heartbeat_holder, heartbeat_running);
    });

    // Supervisor owns the discovery + per-session writer/reader threads.
    let supervisor_running = running.clone();
    let supervisor_holder = serial_tx_holder.clone();
    let supervisor_thread = thread::spawn(move || {
        supervisor_loop(supervisor_holder, supervisor_running, settings.enable_timing_logs);
    });

    let translator = Translator::new(
        settings.device_mac,
        settings.enable_timing_logs,
        serial_tx_holder.clone(),
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
    // Clear the holder so any final translator sends are no-ops, then
    // wait for the long-lived threads to wind down.
    *serial_tx_holder.lock().unwrap() = None;
    drop(translator);
    let _ = heartbeat_thread.join();
    let _ = supervisor_thread.join();
    Ok(())
}
