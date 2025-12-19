#ifndef CPS_CONFIG_H
#define CPS_CONFIG_H

/*
 * CPS (Clicks Per Second) Auto-Clicker Configuration
 *
 * This file contains all tunable parameters for the CPS feature.
 * Values are blended from multiple human click data sessions.
 *
 * Human peak data (5 sessions, ~175 clicks):
 * - CPS: 7.08 avg (range 6.80-7.20)
 * - Hold: avg 90.5ms, normal range 58-117ms, outliers 191-242ms (~3%)
 * - Gap:  avg 50.0ms, normal range 19-75ms, outliers 177-192ms (~1-2%)
 *
 * Uses pseudo-gaussian distribution (sum of 3 randoms / 3) for natural
 * bell-curve variance instead of uniform distribution.
 */

// =============================================================================
// HOLD TIME RANGE (absolute milliseconds, not percentages)
// =============================================================================
// Blended range: Human peak shows 58-117ms normal holds
// Using absolute ranges avoids quantization artifacts from percentage math

const int CPS_HOLD_MIN_MS = 58;   // Minimum hold time (human peak low ~58ms)
const int CPS_HOLD_MAX_MS = 118;  // Maximum hold time (human peak high ~117ms)

// =============================================================================
// GAP TIME RANGE (absolute milliseconds, not percentages)
// =============================================================================
// Blended range: Human peak shows burst gaps as low as 19ms

const int CPS_GAP_MIN_MS = 18;    // Minimum gap time (human burst ~19ms)
const int CPS_GAP_MAX_MS = 75;    // Maximum gap time (human normal high ~75ms)

// =============================================================================
// MICRO-JITTER (final randomness layer)
// =============================================================================
// Adds small random offset to break up any remaining patterns
// Applied on top of all timing calculations

const int CPS_JITTER_MS = 6;      // +/- 6ms random jitter on all values

// =============================================================================
// FATIGUE SIMULATION (affects GAP, not hold!)
// =============================================================================
// Real data shows rare but large fatigue gaps: 177-192ms (~1-2% of clicks)
// Using absolute range instead of multipliers for cleaner values

// Chance (0-100) of a fatigue pause occurring on each click
const int CPS_FATIGUE_CHANCE_PERCENT = 2;

// Minimum clicks before fatigue can occur (burst protection)
const int CPS_MIN_CLICKS_BEFORE_FATIGUE = 5;

// Fatigue GAP range (absolute milliseconds)
// When fatigue triggers, gap is set to a random value in this range
const int CPS_FATIGUE_GAP_MIN_MS = 165;   // Min fatigue gap
const int CPS_FATIGUE_GAP_MAX_MS = 240;   // Max fatigue gap

// =============================================================================
// HESITATION SIMULATION (occasional long holds)
// =============================================================================
// Real data shows occasional long holds: 191ms, 194ms, 204ms, 210ms, 221ms (~3%)
// These are NOT fatigue (which affects gap) - these are the finger staying down longer

// Chance (0-100) of a hesitation occurring on each hold
const int CPS_HESITATION_CHANCE_PERCENT = 3;

// Hesitation hold time range (milliseconds)
// When hesitation triggers, hold time is set to a random value in this range
const int CPS_HESITATION_HOLD_MIN_MS = 185;  // Min hesitation hold
const int CPS_HESITATION_HOLD_MAX_MS = 245;  // Max hesitation hold

#endif // CPS_CONFIG_H
