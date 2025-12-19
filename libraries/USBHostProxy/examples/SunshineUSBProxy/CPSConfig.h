#ifndef CPS_CONFIG_H
#define CPS_CONFIG_H

/*
 * CPS (Clicks Per Second) Auto-Clicker Configuration
 *
 * This file contains all tunable parameters for the CPS feature.
 * Values are based on real human click data analysis (ClickPulse export).
 *
 * Real human click data (5 sessions, 165 clicks):
 * - CPS: 6.64 avg (range 6.20-7.00)
 * - Hold: avg 89.4ms, normal range 60-113ms, outliers 209-224ms (~3%)
 * - Gap:  avg 59.4ms, normal range 23-86ms, outliers 186-243ms (~1-2%)
 *
 * Uses pseudo-gaussian distribution (sum of 3 randoms / 3) for natural
 * bell-curve variance instead of uniform distribution.
 */

// =============================================================================
// HOLD TIME RANGE (absolute milliseconds, not percentages)
// =============================================================================
// Real hold range: 60-113ms (normal), with 200+ms outliers handled by hesitation
// Using absolute ranges avoids quantization artifacts from percentage math

const int CPS_HOLD_MIN_MS = 62;   // Minimum hold time (human low ~60ms)
const int CPS_HOLD_MAX_MS = 113;  // Maximum hold time (human normal high ~113ms)

// =============================================================================
// GAP TIME RANGE (absolute milliseconds, not percentages)
// =============================================================================
// Real gap range: 23-86ms (normal), with 180-240ms outliers handled by fatigue

const int CPS_GAP_MIN_MS = 25;    // Minimum gap time (human low ~23ms)
const int CPS_GAP_MAX_MS = 85;    // Maximum gap time (human normal high ~86ms)

// =============================================================================
// MICRO-JITTER (final randomness layer)
// =============================================================================
// Adds small random offset to break up any remaining patterns
// Applied on top of all timing calculations

const int CPS_JITTER_MS = 5;      // +/- 5ms random jitter on all values

// =============================================================================
// FATIGUE SIMULATION (affects GAP, not hold!)
// =============================================================================
// Real data shows rare but large fatigue gaps: 186ms, 243ms (~1-2% of clicks)
// Using absolute range instead of multipliers for cleaner values

// Chance (0-100) of a fatigue pause occurring on each click
const int CPS_FATIGUE_CHANCE_PERCENT = 2;

// Minimum clicks before fatigue can occur (burst protection)
const int CPS_MIN_CLICKS_BEFORE_FATIGUE = 5;

// Fatigue GAP range (absolute milliseconds)
// When fatigue triggers, gap is set to a random value in this range
const int CPS_FATIGUE_GAP_MIN_MS = 170;   // Min fatigue gap (human low ~186ms)
const int CPS_FATIGUE_GAP_MAX_MS = 250;   // Max fatigue gap (human high ~243ms)

// =============================================================================
// HESITATION SIMULATION (occasional long holds)
// =============================================================================
// Real data shows occasional long holds: 209ms, 217ms, 218ms, 224ms (~3% of clicks)
// These are NOT fatigue (which affects gap) - these are the finger staying down longer

// Chance (0-100) of a hesitation occurring on each hold
const int CPS_HESITATION_CHANCE_PERCENT = 3;

// Hesitation hold time range (milliseconds)
// When hesitation triggers, hold time is set to a random value in this range
const int CPS_HESITATION_HOLD_MIN_MS = 180;  // Min hesitation hold
const int CPS_HESITATION_HOLD_MAX_MS = 230;  // Max hesitation hold

#endif // CPS_CONFIG_H
