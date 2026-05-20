//! State machine that maintains the cumulative button mask, decodes
//! incoming KMBox Net packets, and emits Streamcheats packets to the
//! serial-writer thread via an mpsc channel. Spawns short-lived worker
//! threads for `automove` (linear) and `bezier_move` (cubic) interpolation.

use std::sync::mpsc::Sender;
use std::sync::{Arc, Mutex};
use std::thread;
use std::time::{Duration, Instant};

use tracing::{debug, info, warn};

use crate::kmbox_net::{
    cmd_name, Header, SoftMouse, CMD_BAZER_MOVE, CMD_CONNECT, CMD_DEBUG, CMD_KEYBOARD_ALL,
    CMD_MASK_MOUSE, CMD_MONITOR, CMD_MOUSE_AUTOMOVE, CMD_MOUSE_LEFT, CMD_MOUSE_MIDDLE,
    CMD_MOUSE_MOVE, CMD_MOUSE_RIGHT, CMD_MOUSE_WHEEL, CMD_REBOOT, CMD_SETCONFIG, CMD_SETVIDPID,
    CMD_SHOWPIC, CMD_TRACE_ENABLE, CMD_UNMASK_ALL, HEADER_LEN,
};
use crate::streamcheats::{
    self, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, PACKET_LEN,
};

/// One outgoing Streamcheats packet bound for the serial worker.
pub type SerialPacket = [u8; PACKET_LEN];

/// What the translator hands to the serial writer thread: the packet plus
/// the `Instant` at which we want latency to be measured against. For
/// directly-translated commands (mouse_move, button toggles, wheel) the
/// origin is `Instant::now()` at the start of `handle_packet`, i.e. very
/// close to when the UDP datagram arrived — `latency` then approximates
/// end-to-end host-app → wire delay. For interpolation worker packets
/// the origin is `Instant::now()` at the worker's send moment so the
/// latency reflects channel + write cost only, not the intentional
/// `duration_ms` the host asked for.
pub type SerialEnvelope = (Instant, SerialPacket);

/// Wall-clock step interval (milliseconds) used by the interpolation
/// workers. Each tick emits one delta packet. 4 ms ≈ 250 Hz, which keeps
/// motion smooth without saturating the serial pipe.
const STEP_MS: u64 = 4;

/// Per-process state machine that decodes incoming KMBox Net packets and
/// emits Streamcheats packets onto the serial-writer channel.
///
/// `Translator` owns the cumulative mouse button mask (shared with the
/// interpolation workers via `Arc<Mutex<u8>>`), which is needed because
/// per-button `mouse_left` / `mouse_right` / `mouse_middle` commands only
/// touch a single bit at a time but the Streamcheats packet always
/// re-transmits the full state.
pub struct Translator {
    expected_mac: u32,
    enable_timing: bool,
    button_mask: Arc<Mutex<u8>>,
    serial_tx: Sender<SerialEnvelope>,
}

impl Translator {
    /// Build a new translator.
    ///
    /// * `expected_mac` — the 4-byte device identifier read from
    ///   `config.json`. Packets whose [`Header::mac`](crate::kmbox_net::Header::mac)
    ///   does not equal this value are silently dropped.
    /// * `enable_timing` — when `true`, each `IN (KMBOX NET): cmd=...`
    ///   line gets a `parse=Nµs` suffix showing how long the
    ///   header-parse → dispatch → channel-send leg took.
    /// * `serial_tx` — sender half of the mpsc channel the main serial
    ///   writer thread reads from. The translator and every interpolation
    ///   worker it spawns share clones of this sender. The channel carries
    ///   [`SerialEnvelope`]s (packet + origin instant) so the writer can
    ///   compute per-packet latency at log time.
    pub fn new(
        expected_mac: u32,
        enable_timing: bool,
        serial_tx: Sender<SerialEnvelope>,
    ) -> Self {
        Self {
            expected_mac,
            enable_timing,
            button_mask: Arc::new(Mutex::new(0)),
            serial_tx,
        }
    }

    /// Returns ` parse=Nµs` (with a leading space) when timing logs are
    /// enabled, or an empty string when they're off. Used as a suffix on
    /// every `IN (KMBOX NET):` log line.
    fn parse_suffix(&self, recv_at: Instant) -> String {
        if self.enable_timing {
            format!(" parse={}µs", recv_at.elapsed().as_micros())
        } else {
            String::new()
        }
    }

    /// Process one incoming UDP datagram end-to-end.
    ///
    /// Returns the 16-byte reply the caller should send back to the peer,
    /// or `None` when the packet was silently dropped (wrong MAC, too
    /// short to parse). Side effects — sending Streamcheats packets to
    /// the serial worker, mutating the cumulative button mask, spawning
    /// interpolation worker threads — happen before the reply is built.
    pub fn handle_packet(&self, datagram: &[u8]) -> Option<[u8; HEADER_LEN]> {
        // Capture as close to recv_from as we reasonably can — the only
        // intermediate work is the call indirection. This timestamp
        // propagates into the channel envelope so the writer thread can
        // compute end-to-end latency for the OUT log line.
        let recv_at = Instant::now();

        let header = match Header::parse(datagram) {
            Ok(h) => h,
            Err(e) => {
                warn!("malformed UDP packet ({} bytes): {}", datagram.len(), e);
                return None;
            }
        };

        if header.mac != self.expected_mac {
            debug!(
                "dropping packet with wrong mac=0x{:08X} (expected 0x{:08X})",
                header.mac, self.expected_mac
            );
            return None;
        }

        let body = &datagram[HEADER_LEN..];
        self.dispatch(&header, body, recv_at);
        Some(header.reply())
    }

    fn dispatch(&self, header: &Header, body: &[u8], recv_at: Instant) {
        match header.cmd {
            CMD_CONNECT => {
                *self.button_mask.lock().unwrap() = 0;
                info!(
                    "IN (KMBOX NET): cmd=connect (reset button mask){}",
                    self.parse_suffix(recv_at)
                );
            }
            CMD_MOUSE_MOVE => {
                if let Some(m) = self.parse_mouse(body, header.cmd) {
                    let mask = *self.button_mask.lock().unwrap();
                    info!(
                        "IN (KMBOX NET): cmd=mouse_move x={} y={} mask=0x{:02X}{}",
                        m.x, m.y, mask, self.parse_suffix(recv_at)
                    );
                    self.send_packet(streamcheats::build_packet(mask, m.x, m.y, 0), recv_at);
                }
            }
            CMD_MOUSE_LEFT => self.handle_button_cmd(body, header.cmd, BTN_LEFT, "left", recv_at),
            CMD_MOUSE_RIGHT => self.handle_button_cmd(body, header.cmd, BTN_RIGHT, "right", recv_at),
            CMD_MOUSE_MIDDLE => self.handle_button_cmd(body, header.cmd, BTN_MIDDLE, "middle", recv_at),
            CMD_MOUSE_WHEEL => {
                if let Some(m) = self.parse_mouse(body, header.cmd) {
                    let mask = *self.button_mask.lock().unwrap();
                    info!(
                        "IN (KMBOX NET): cmd=mouse_wheel wheel={} mask=0x{:02X}{}",
                        m.wheel, mask, self.parse_suffix(recv_at)
                    );
                    self.send_packet(streamcheats::build_packet(mask, 0, 0, m.wheel), recv_at);
                }
            }
            CMD_MOUSE_AUTOMOVE => {
                if let Some(m) = self.parse_mouse(body, header.cmd) {
                    let duration_ms = m.point[0].max(0) as u64;
                    info!(
                        "IN (KMBOX NET): cmd=mouse_automove x={} y={} ms={} (worker){}",
                        m.x, m.y, duration_ms, self.parse_suffix(recv_at)
                    );
                    self.spawn_automove(m.x, m.y, duration_ms);
                }
            }
            CMD_BAZER_MOVE => {
                if let Some(m) = self.parse_mouse(body, header.cmd) {
                    let duration_ms = m.point[0].max(0) as u64;
                    let x1 = m.point[1];
                    let y1 = m.point[2];
                    let x2 = m.point[3];
                    let y2 = m.point[4];
                    info!(
                        "IN (KMBOX NET): cmd=bezier_move x={} y={} ms={} ctl=({},{})({},{}) (worker){}",
                        m.x, m.y, duration_ms, x1, y1, x2, y2, self.parse_suffix(recv_at)
                    );
                    self.spawn_bezier(m.x, m.y, duration_ms, x1, y1, x2, y2);
                }
            }
            CMD_KEYBOARD_ALL => {
                info!("IN (KMBOX NET): cmd=keyboard_all (ack only, no serial)");
            }
            CMD_REBOOT | CMD_MONITOR | CMD_MASK_MOUSE | CMD_UNMASK_ALL | CMD_SETCONFIG
            | CMD_SETVIDPID | CMD_DEBUG | CMD_SHOWPIC | CMD_TRACE_ENABLE => {
                info!("IN (KMBOX NET): cmd={} (ack only)", cmd_name(header.cmd));
            }
            other => {
                warn!(
                    "IN (KMBOX NET): cmd=unknown(0x{:08X}) (ack only, replying with echo)",
                    other
                );
            }
        }
    }

    fn parse_mouse(&self, body: &[u8], cmd: u32) -> Option<SoftMouse> {
        match SoftMouse::parse(body) {
            Ok(m) => Some(m),
            Err(e) => {
                warn!("cmd={} body parse failed: {}", cmd_name(cmd), e);
                None
            }
        }
    }

    fn handle_button_cmd(
        &self,
        body: &[u8],
        cmd: u32,
        bit: u8,
        label: &str,
        recv_at: Instant,
    ) {
        let Some(m) = self.parse_mouse(body, cmd) else {
            return;
        };
        let pressed = m.button != 0;
        // KMBox Net sends each button as a separate command (`mouse_left`,
        // `mouse_right`, `mouse_middle`) but the Streamcheats serial
        // packet always carries the *full* button bitmask. So we mutate
        // a single bit here and re-emit the cumulative mask on the wire,
        // which means a held left button stays held while right is
        // toggling.
        let mask = {
            let mut g = self.button_mask.lock().unwrap();
            if pressed {
                *g |= bit;
            } else {
                *g &= !bit;
            }
            *g
        };
        info!(
            "IN (KMBOX NET): cmd=mouse_{} state={} mask=0x{:02X}{}",
            label, pressed as u8, mask, self.parse_suffix(recv_at)
        );
        self.send_packet(streamcheats::build_packet(mask, 0, 0, 0), recv_at);
    }

    fn send_packet(&self, pkt: SerialPacket, origin: Instant) {
        if let Err(e) = self.serial_tx.send((origin, pkt)) {
            warn!("serial channel send failed: {}", e);
        }
    }

    fn spawn_automove(&self, target_x: i32, target_y: i32, duration_ms: u64) {
        let mask = self.button_mask.clone();
        let tx = self.serial_tx.clone();
        thread::spawn(move || {
            interp_linear(target_x, target_y, duration_ms, mask, tx);
        });
    }

    #[allow(clippy::too_many_arguments)]
    fn spawn_bezier(
        &self,
        target_x: i32,
        target_y: i32,
        duration_ms: u64,
        x1: i32,
        y1: i32,
        x2: i32,
        y2: i32,
    ) {
        let mask = self.button_mask.clone();
        let tx = self.serial_tx.clone();
        thread::spawn(move || {
            interp_bezier(
                target_x,
                target_y,
                duration_ms,
                x1,
                y1,
                x2,
                y2,
                mask,
                tx,
            );
        });
    }
}

/// Linear interpolation worker: emits incremental moves so the cumulative
/// delta equals `(target_x, target_y)` over `duration_ms`, in [`STEP_MS`]
/// increments. A duration shorter than one step is rounded up to a
/// single-step move so a `automove(..., 0)` still moves the cursor.
fn interp_linear(
    target_x: i32,
    target_y: i32,
    duration_ms: u64,
    mask: Arc<Mutex<u8>>,
    tx: Sender<SerialEnvelope>,
) {
    let dur_ms = duration_ms.max(STEP_MS);
    let steps = ((dur_ms + STEP_MS - 1) / STEP_MS).max(1) as i64;
    // Anchor every step's target time to the worker's start instant
    // rather than the previous step's wake time. That way an occasional
    // long sleep can't accumulate into drift across a long automove —
    // we just skip the sleep on a step we're already late for, and the
    // final emitted total still lands on the requested target.
    let start = Instant::now();
    let mut emitted_x: i64 = 0;
    let mut emitted_y: i64 = 0;
    let tx_i = target_x as i64;
    let ty_i = target_y as i64;

    for i in 1..=steps {
        let want_x = tx_i * i / steps;
        let want_y = ty_i * i / steps;
        let dx = (want_x - emitted_x) as i32;
        let dy = (want_y - emitted_y) as i32;
        emitted_x = want_x;
        emitted_y = want_y;

        let current_mask = *mask.lock().unwrap();
        let pkt = streamcheats::build_packet(current_mask, dx, dy, 0);
        // Origin = send moment, not worker start. We want the writer to
        // log channel + write latency per step, not the deliberate
        // duration_ms-paced gap from the original UDP arrival.
        if tx.send((Instant::now(), pkt)).is_err() {
            return;
        }

        let target_t = start + Duration::from_millis(STEP_MS * i as u64);
        if let Some(sleep_for) = target_t.checked_duration_since(Instant::now()) {
            thread::sleep(sleep_for);
        }
    }
}

/// Cubic bezier interpolation between (0,0) and (target) via control
/// points (x1,y1) and (x2,y2). Emits delta moves on `STEP_MS` cadence.
#[allow(clippy::too_many_arguments)]
fn interp_bezier(
    target_x: i32,
    target_y: i32,
    duration_ms: u64,
    x1: i32,
    y1: i32,
    x2: i32,
    y2: i32,
    mask: Arc<Mutex<u8>>,
    tx: Sender<SerialEnvelope>,
) {
    let dur_ms = duration_ms.max(STEP_MS);
    let steps = ((dur_ms + STEP_MS - 1) / STEP_MS).max(1) as u64;
    let start = Instant::now();
    let mut emitted_x: f64 = 0.0;
    let mut emitted_y: f64 = 0.0;

    let p0 = (0.0f64, 0.0f64);
    let p1 = (x1 as f64, y1 as f64);
    let p2 = (x2 as f64, y2 as f64);
    let p3 = (target_x as f64, target_y as f64);

    for i in 1..=steps {
        let t = i as f64 / steps as f64;
        let u = 1.0 - t;
        let bx = u * u * u * p0.0
            + 3.0 * u * u * t * p1.0
            + 3.0 * u * t * t * p2.0
            + t * t * t * p3.0;
        let by = u * u * u * p0.1
            + 3.0 * u * u * t * p1.1
            + 3.0 * u * t * t * p2.1
            + t * t * t * p3.1;

        let dx = (bx - emitted_x).round() as i32;
        let dy = (by - emitted_y).round() as i32;
        emitted_x += dx as f64;
        emitted_y += dy as f64;

        let current_mask = *mask.lock().unwrap();
        let pkt = streamcheats::build_packet(current_mask, dx, dy, 0);
        // Origin = send moment, not worker start. We want the writer to
        // log channel + write latency per step, not the deliberate
        // duration_ms-paced gap from the original UDP arrival.
        if tx.send((Instant::now(), pkt)).is_err() {
            return;
        }

        let target_t = start + Duration::from_millis(STEP_MS * i);
        if let Some(sleep_for) = target_t.checked_duration_since(Instant::now()) {
            thread::sleep(sleep_for);
        }
    }
}
