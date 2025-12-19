# CPS Auto-Clicker System

Human-mimicking click generation for USB mouse proxy.

## Overview

The CPS (Clicks Per Second) system generates synthetic left-click patterns that statistically match real human clicking behavior. When enabled, holding the physical left mouse button produces rapid clicks with natural timing variance, fatigue simulation, and occasional hesitation pauses.

### Activation
- **Enable:** Press MMB (middle mouse button), release, then press MB5 (back thumb) within 1200ms
- **Disable:** Press MMB while CPS is active

### Behavior
- When enabled + LMB held: Generates synthetic clicks at ~6.5-7.0 CPS
- Other buttons (RMB, MMB, etc.) pass through normally
- Releasing LMB stops synthetic clicking

## Configuration

All parameters are in `examples/SunshineUSBProxy/CPSConfig.h`:

### Hold Time (button pressed duration)
| Parameter | Default | Description |
|-----------|---------|-------------|
| `CPS_HOLD_MIN_MS` | 62 | Minimum hold time |
| `CPS_HOLD_MAX_MS` | 113 | Maximum hold time (normal clicks) |

### Gap Time (between clicks)
| Parameter | Default | Description |
|-----------|---------|-------------|
| `CPS_GAP_MIN_MS` | 25 | Minimum gap time |
| `CPS_GAP_MAX_MS` | 85 | Maximum gap time (normal clicks) |

### Micro-Jitter
| Parameter | Default | Description |
|-----------|---------|-------------|
| `CPS_JITTER_MS` | 5 | +/- random offset on all timings |

### Fatigue Simulation (long gaps)
| Parameter | Default | Description |
|-----------|---------|-------------|
| `CPS_FATIGUE_CHANCE_PERCENT` | 2 | Probability per click (0-100) |
| `CPS_MIN_CLICKS_BEFORE_FATIGUE` | 5 | Burst protection |
| `CPS_FATIGUE_GAP_MIN_MS` | 170 | Minimum fatigue gap |
| `CPS_FATIGUE_GAP_MAX_MS` | 250 | Maximum fatigue gap |

### Hesitation Simulation (long holds)
| Parameter | Default | Description |
|-----------|---------|-------------|
| `CPS_HESITATION_CHANCE_PERCENT` | 3 | Probability per hold (0-100) |
| `CPS_HESITATION_HOLD_MIN_MS` | 180 | Minimum hesitation hold |
| `CPS_HESITATION_HOLD_MAX_MS` | 230 | Maximum hesitation hold |

## Randomization Algorithm

### Pseudo-Gaussian Distribution
Instead of uniform random, uses sum of 3 random values divided by 3:
```cpp
int gaussianRandom(int minVal, int maxVal) {
    long sum = random(minVal, maxVal + 1) +
               random(minVal, maxVal + 1) +
               random(minVal, maxVal + 1);
    return sum / 3;
}
```
This creates a natural bell-curve distribution where values cluster toward the center with fewer extremes, matching real human behavior.

### Why Not Percentage-Based Variance?
Earlier versions used percentage variance from a base value:
```cpp
// OLD - creates detectable patterns
baseInterval = 62;
variance = random(-50%, +15%);
result = baseInterval * (100 + variance) / 100;
```

**Problems detected:**
1. **Bimodal clustering** - 50% up / 50% down creates two value clusters
2. **Value repetition** - certain values (e.g., 63ms) appear too frequently
3. **Quantization artifacts** - percentage math creates predictable patterns

**Solution: Absolute ranges + gaussian + jitter**
```cpp
// NEW - undetectable distribution
result = gaussianRandom(CPS_GAP_MIN_MS, CPS_GAP_MAX_MS);
result += random(-CPS_JITTER_MS, CPS_JITTER_MS + 1);
```

## Blind Testing Methodology

### Setup
1. Record 5 sessions of real human clicks using ClickPulse tracker
2. Record 5 sessions of machine clicks with same tracker
3. Export timing data (hold times, gap times per click)
4. Provide both datasets to AI agent with swapped/neutral labels
5. Ask agent to identify which is machine based on statistical patterns

### Test 1: Original Algorithm (percentage-based)
**Result: DETECTED**

Agent identified machine because:
- Gap values clustered in two bands (~35-44ms and ~62-71ms)
- Value 63ms appeared ~15 times (target value repetition)
- "Quantized" feel from percentage math

### Test 2: Improved Algorithm (gaussian + absolute + jitter)
**Result: PASSED**

Agent incorrectly identified HUMAN data as machine because:
- Machine data showed natural continuous distribution
- No detectable clustering or repetition patterns
- Human data had "too many outliers" (interpreted as over-engineered)

### Statistical Comparison
| Metric | Human | Old Machine | New Machine |
|--------|-------|-------------|-------------|
| Hesitation rate | 6.7% | 2.4% | 2.4% |
| Gap distribution | Continuous | Bimodal | Continuous |
| Value clustering | Natural | Detected | Not detected |
| Blind test result | "Suspicious" | Detected | **Passed** |

## Files

| File | Purpose |
|------|---------|
| `CPSConfig.h` | All tunable parameters |
| `SunBoxSyntheticHandleOutput.h` | State variables and method declarations |
| `SunBoxSyntheticHandleOutput.cpp` | Implementation (toggle, timing, click generation) |

## Real Human Click Data Reference

Based on ClickPulse analysis of 5 sessions (~165 clicks):

| Metric | Value |
|--------|-------|
| CPS | 6.64 avg (range 6.20-7.00) |
| Hold time | avg 89.4ms, normal range 60-113ms |
| Hold outliers | 209-312ms (~3-7% of clicks) |
| Gap time | avg 59.4ms, normal range 23-86ms |
| Gap outliers | 134-243ms (~1-2% of clicks) |

## Version History

- **v1.0** - Basic CPS with fixed timing
- **v1.1** - Added percentage-based variance
- **v1.2** - Added fatigue and hesitation simulation
- **v2.0** - Gaussian distribution, absolute ranges, micro-jitter (current)
