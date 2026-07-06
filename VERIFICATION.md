# VERIFICATION.md — WatchyDict mega build

Data provenance and verification record. Generated 2026-07-05.
Rule enforced: **no data hand-entered by the AI anywhere** — all data
comes from authoritative sources through the inspectable scripts in
this folder, and all astronomy math was verified against astropy 8.0.1
(the professional reference library) before shipping.

## Data sources

| File | Source | License |
|---|---|---|
| dictionary.cdb | Wordset dictionary (github.com/wordset/wordset-dictionary) | CC BY-SA |
| gazetteer.cdb | CIA World Factbook via github.com/factbook/factbook.json | public domain |
| stars.h (904 stars) | Yale Bright Star Catalogue via github.com/brettonw/YaleBrightStarCatalog | public domain |
| stars.h (743 lines) | d3-celestial constellation figures, github.com/ofrohn/d3-celestial | BSD-3 |
| problems.bin | Tom's existing tsumego pack (Lichess / Cho Chikun) | — |

Build scripts included: `prepare_dict_fat.py`, `make_gazetteer.py`,
`make_stars.py`. Each can be re-run to reproduce its output from source.

## Astronomy verification (sky_math.h vs astropy 8.0.1)

The exact `sky_math.h` shipped in the firmware was compiled with gcc
into a test harness and compared against astropy. Location: 45.3475 N,
75.7566 W (Bells Corners). Screen scale: 1 pixel ≈ 1.2°.

| # | Test | Cases | Worst error | Tolerance | Result |
|---|---|---|---|---|---|
| 1 | Star pipeline: ICRS → FK5 J2033 catalog → firmware alt/az vs astropy full transform | 300 random star/time, 2026–2040 | 0.112° | 0.3° | PASS |
| 2 | Sunrise/sunset vs astropy sun-altitude bisection (−0.833°) | 60 random dates | 0.14 min (8 s) | 2 min | PASS |
| 3a | Moon illuminated fraction | 100 random times | 0.81% | 2% | PASS |
| 3b | Moon position (TETE frame) | 100 random times | 0.067° | 0.6° | PASS |
| 4 | DST rules vs IANA (Toronto, Los Angeles, London, Berlin, Tokyo — NA, EU, and fixed-offset rules) | every day 2026–2040 (5,479) × 5 zones | 0 mismatches | 0 | PASS |
| 5 | Shipped stars.h quantized bytes → firmware math vs astropy | 60 star/time cases | 0.112° | 0.3° | PASS |

After the location became configurable (/location.txt), tests 1–2 were
re-run at Sydney, Reykjavik, Quito, and Auckland in addition to Ottawa:
worst star error 0.11°, worst sunrise error 0.17 min, polar no-sunrise
days correctly reported. The math is location-clean, verified, not
assumed.

Test 5 closes the loop: it decodes the actual quantized bytes in the
generated stars.h, runs them through the actual firmware trigonometry,
and compares against astropy computing from the original catalogue —
end to end, every worst-case error is under a tenth of a pixel.

Verification failures during development (all fixed before shipping):
the first sunrise algorithm missed tolerance (2.7 min) and was replaced
with the NOAA equation-of-time method; two test-side frame errors
(barycentric-vs-geocentric moon comparison, unprecessed star input)
were corrected so the tests measure the true pipeline.

## Data file validation

Both .cdb files were parsed by a byte-exact Python mirror of the
firmware's loadBlockIndex()/decompressBlock(): magic, index integrity,
per-block raw-deflate decompression, alphabetical sort across all
blocks, and entry counts all verified.

- dictionary.cdb (fat): 69,457 words, 216 blocks, 2,454,342 bytes
- gazetteer.cdb: 261 countries/territories, 7 blocks, 80,972 bytes

**Bug found and fixed by this validation:** the original block splitter
aligned block boundaries *forward* past 32,768 bytes, and the fat
dictionary produced a 33,226-byte block — larger than the firmware's
33,000-byte decompression buffer (a RAM overflow on the watch). The
splitter in prepare_dict_fat.py aligns backward instead; every block in
both shipped files is now ≤ 32,768 bytes, re-verified.

## Capacity

Full real payload (fat dictionary + gazetteer + problems.bin + 10
photos + state file) = 2,996,932 bytes, packed successfully into the
0x340000 SPIFFS image with ≥ 60 KB to spare.

## Known limitations (honest notes)

- The fat dictionary is a pure Wordset build. The ~50 KB of curated
  supplement entries / synonym redirects present in the previous
  dictionary.cdb are NOT included (their source list wasn't available
  at build time). Supply the supplement file and re-run the build to
  merge them.
- Moon glyph is an 8-phase approximation (not a continuous terminator).
- Constellation line segments crossing the horizon are dropped rather
  than clipped.
- Sky assumes the RTC date/time is set correctly; the header shows the
  date it's using so an error is visible, not silent.
- EU DST switching is modeled at the common local switch hour; within
  the single transition hour twice a year the label may lead/lag by up
  to an hour for zones far from UTC+1. Daily-noon verification is exact.
