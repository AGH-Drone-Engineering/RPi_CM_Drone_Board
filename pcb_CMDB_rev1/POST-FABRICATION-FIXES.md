# Post-Fabrication Fixes — +5VCC PSU (AP64501SP-13 buck)

Rework on the already-fabricated board. No respin; every change reuses an existing pad.

## Changes

| Ref | Was | Now |
|-----|-----|-----|
| VIN (input pads) | — | **1000 µF** electrolytic, ≥35 V (50 V preferred) |
| CP1 | 10 µF tantalum (6032) | **AVX 12105C106KAT2A** — 10 µF / 50 V / X7R / 1210 |
| CP2, CP3 | 2 × 22 µF tantalum 10 V (3528) | **2 × Murata GRM32ER71E226KE15L** — 22 µF / 25 V / X7R / 1210 |
| CP8 | Not populated (DNP) | **Yageo CC0805JRNPO9BN330** — 33 pF / 50 V / C0G / 0805 |

## Why

This converter was underperforming in corner-cases, so these changes harden it without a
respin. The 1000 µF bulk on VIN is the primary fix, giving low-frequency hold-up so the
output no longer follows dips on the input rail. CP1 becomes a 50 V ceramic to add the
high-frequency input bypass the layout lacks near the IC, with enough voltage margin to
survive DC-bias derating at 6S. CP2/CP3 move to 25 V ceramic for lower ESR, better hold-up,
and to drop the marginal voltage derating of the old 10 V tantalums; the AP64501 is peak
current-mode and stays stable with a low-ESR ceramic output. Populating CP8 adds a
feed-forward zero (~42 kHz across RP1) that speeds the feedback response and improves phase
margin. The inductor was left unchanged — a higher-value part in the same footprint cannot
carry the 5 A target.

## Validation (LTspice, `sim/`)

- **Idle, as-built:** VOUT disturbance ≈ 186 mV pp.
- **Idle, with fixes:** VOUT disturbance ≈ 10 mV pp (~18× improvement).
- **Full load 5 A, with fixes:** stable at 4.96 V, ~12 mV ripple, no ringing.

Caveat: model control constants are representative, not datasheet — absolute mV are
approximate, but the relative improvement and stability conclusion are robust.
