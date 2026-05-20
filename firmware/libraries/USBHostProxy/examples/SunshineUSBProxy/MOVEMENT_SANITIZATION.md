# Movement Sanitization System

## Overview

The SunBoxSyntheticHandleOutput blends real USB mouse input with synthetic serial input. Without sanitization, several detectable signatures emerge in the combined output. This system adds four hardware-level safeguards that adapt to the user's real movement in real-time.

## Architecture

A central `MovementProfileTracker` maintains a 32-entry ring buffer of the user's real USB mouse deltas. From this, it continuously derives:
- **avgSpeedX/Y** — average absolute delta per frame
- **avgAccelX/Y** — average frame-to-frame change in delta
- **maxDeltaX/Y** — peak delta in recent history
- **estimatedUsbRateHz** — real device polling rate (packets/sec)

These metrics form the baseline for "what natural movement looks like right now" and drive adaptive thresholds across all safeguards.

## Safeguards

### 1. Output Rate Regulation

**Problem:** Serial-only frames inflate the output packet rate beyond the real device's rate (e.g., real mouse at 125Hz + synthetic at 240Hz = 365Hz output).

**Solution:** `shouldThrottleSerialOnly()` compares the current output rate against `estimatedUsbRateHz * 1.1` (10% margin). When serial-only frames would exceed this:
- Serial deltas are accumulated (not output)
- On the next USB-triggered frame, accumulated deltas are released **smoothly** — capped per frame at the adaptive threshold (`avgSpeed + avgAccel * 2`)
- Accumulated deltas older than 50ms are discarded (prevents stale jumps)

**What it covers:**
- Prevents polling rate spikes that anti-cheat frequency analysis would detect
- Preserves the synthetic input by folding it into the natural polling rhythm

**What it does NOT cover:**
- Does not address burst patterns within a single frame (that's a PC-side interpolation concern)
- Does not smooth the timing intervals between packets (jitter is inherited from the real device)

### 2. Sign Flip Rate Limiter

**Problem:** When serial deltas fight the USB direction, the combined output produces rapid left-right-left oscillations that exceed natural movement patterns (natural: 0-5 flips per movement, anomalous: 33+).

**Solution:** `sanitizeSignFlips()` tracks direction reversals per axis in 1-second tumbling windows. When flips exceed 8/sec:
- Checks if the USB component alone would have caused the flip
- If serial caused it: reverts to USB-only value (serial rejected for that axis, that frame)
- If USB caused it: allows it (real user input)

**What it covers:**
- Prevents oscillating output from serial corrections fighting user movement
- Allows natural direction changes from the real mouse

**What it does NOT cover:**
- Does not limit the magnitude of direction changes (only frequency)
- Does not address smooth curves — a flip that's allowed can still be sharp

### 3. Adaptive Value Spike Clamper

**Problem:** Async serial deltas create single-frame outliers (e.g., `10, 18, 12, 13`) where the spike is detectable via smoothness analysis.

**Solution:** `clampValueSpikes()` compares each combined output value against the previous output, using an adaptive threshold derived from real USB movement:
- `threshold = max(3, avgSpeedX + avgAccelX * 2)`
- If `|finalX - prevOutputX| > threshold`: clamp to `prevOutputX ± threshold`

**Adaptive behavior:**
- User moving fast (avgSpeed=50, avgAccel=10) → threshold=73, big jumps allowed
- User barely moving (avgSpeed=3, avgAccel=1) → threshold=5, tight clamping
- User idle (avgSpeed=0) → threshold=3, minimal movement allowed

**What it covers:**
- Prevents single-frame spikes that break smoothness score
- Enforces gradual acceleration/deceleration curves
- Adapts to whatever the user is currently doing

**What it does NOT cover:**
- Does not enforce specific acceleration curve shapes (bell curve, etc.)
- Does not add micro-corrections or human-like tremor
- Does not address the overall trajectory (path efficiency, curvature)

### 4. Sensitivity Reduction Ramp

**Problem:** When sensitivity lockout activates, movement instantly drops from 100% to the configured reduction (e.g., 40%). This step function is detectable.

**Solution:** `modifyMovementWithSerialData()` now linearly interpolates between the previous effective reduction and the target over 32ms (SENS_RAMP_MS). Applies to both activation (100→target) and deactivation (target→100).

**What it covers:**
- Eliminates the velocity step function when lockout toggles
- Preserves the "locky" feel (32ms is fast enough to feel responsive)

**What it does NOT cover:**
- Does not add noise to the reduction curve (it's a clean linear ramp)
- Does not vary the ramp duration per activation (it's always 32ms)
- The PC-side sensitivity modifier also manages transitions — this is a safety net

## Execution Order

In `process()`, after movement blending:
```
1. updateMovementProfile()     — track real USB movement
2. rate throttle check         — gate serial-only frames
3. fold accumulated deltas     — smooth release into USB frames
4. modifyMovementWithSerialData() — blend with sens ramp
5. sanitizeSignFlips()         — cap direction reversals
6. clampValueSpikes()          — enforce smooth output
7. recordOutput()              — store for next frame's comparison
```

## Constants

All hardcoded (no serial configuration):

| Constant | Value | Purpose |
|----------|-------|---------|
| MOVEMENT_HISTORY_SIZE | 32 | Ring buffer size (~128ms at 250Hz) |
| MAX_SIGN_FLIPS_PER_SECOND | 8 | Direction reversal cap per axis |
| SENS_RAMP_MS | 32 | Sensitivity transition duration |
| OUTPUT_RATE_MARGIN_PCT | 10 | Allowed output rate above USB rate |
| MIN_SPIKE_THRESHOLD | 3 | Minimum allowed per-frame step |
| ACCUMULATED_DECAY_MS | 50 | Discard stale accumulated deltas |

## Resource Usage

- **RAM:** ~310 bytes (ring buffers + trackers + smoother)
- **CPU:** <10 microseconds per cycle (integer math only, no floats)

## What Is NOT Covered (Future Work)

These detection vectors exist but are outside the scope of this firmware-level system:

1. **Frame-locked frequency signatures** — 240Hz component from frame-timed serial input. Must be addressed PC-side with temporal interpolation.
2. **Human-like micro-corrections and tremor** — Real humans overshoot and correct. Adding Perlin noise or micro-oscillations would improve realism.
3. **Bezier/curved movement paths** — Currently linear interpolation. Curved paths would reduce path efficiency detection.
4. **Timing jitter** — Inter-packet timing inherits from the real device. Adding deliberate jitter could mask synthetic additions but risks breaking HID timing requirements.
5. **Statistical distribution of deltas** — Natural movement has high unique-value count and low repeat ratio. The clamper may introduce more repetition by clamping to threshold boundaries.
