# WatchyDict

Offline everything-firmware for the Watchy V2 (ESP32, 200x200 e-ink, 4MB flash).
No wifi, no phone, weeks of battery.

- **Dictionary** — 69,457 words, uncapped definitions, letter-grid search
- **Gazetteer** — 261 countries from the CIA World Factbook, same search UI
- **Tsumego** — 11,814 go problems (Lichess + Cho Chikun), progress in NVS
- **Sky** — star chart for your location (configurable): 904 stars, constellation lines, moon
  phase, sunrise/sunset (see VERIFICATION.md for how the math was checked)
- **Photos** — 1-bit image viewer (up to 10 images)
- **Clock** — watchface that deep sleeps between minutes and ticks on the
  PCF8563 alarm interrupt; deep-sleep wake returns to whatever mode you
  were in

## Repo layout

```
WatchyDict.ino    main firmware
tsumego.h         go puzzle engine
sky.h             star chart display module
sky_math.h        astronomy math (pure C, desktop-testable)
stars.h           GENERATED star/line data (make_stars.py)
DictEntry.h       shared entry struct
partitions.csv    640KB app / 3.25MB SPIFFS
dictionary.cdb    GENERATED (prepare_dict_fat.py)
gazetteer.cdb     GENERATED (make_gazetteer.py)
problems.bin      tsumego pack (tsumego_pack.py)
tools/            build scripts + spiffsgen.py
```

## Build

Arduino IDE, board **ESP32 Dev Module**, Flash Size **4MB**. Put
`partitions.csv` next to the sketch and select the custom partition
scheme. Needs GxEPD2.

Note for .ino hacking: keep all function definitions *below* the
includes — the IDE injects auto-generated prototypes at the first
function definition, and if that's above your includes you get
"'DictEntry' does not name a type" on innocent code.

## Flash the data

Make a `data/` folder containing `dictionary.cdb`, `gazetteer.cdb`,
`problems.bin`, optional `img_00.bin`..`img_09.bin`, and `location.txt`
for your city (defaults to Ottawa if the file is missing):

```
lat=45.3475
lon=-75.7566
utc=-5          # standard-time UTC offset
dst=NA          # NA | EU | NONE
tz=est,edt      # optional labels for the sky header
```

The sky chart, sunrise/sunset, and the clock's DST handling all follow
this file. Then:

```
python tools/spiffsgen.py 0x340000 data spiffs.bin --page-size 256 --block-size 4096
python -m esptool write-flash 0xB0000 spiffs.bin
```

SPIFFS offset is **0xB0000** for this partition table. Use spiffsgen.py,
not mkspiffs — the Windows mkspiffs build fails on images past ~2.1MB.

## Controls

Dictionary/gazetteer: TR search, TL tap random / hold menu, BR/BL
next/prev. Search grid: BR right, BL down, TR add/GO, TL delete/exit
(grid includes `-` for country names). Clock: TR set time/date, TL back.
Sky: BR/BL scrub +/-30 min, TR now, TL back. Set time: hold BR/BL to
scrub fast.

## Rebuilding the data

Every byte on the watch traces to a public source through a rerunnable
script — nothing hand-entered:

| data | source | license |
|---|---|---|
| dictionary | [Wordset](https://github.com/wordset/wordset-dictionary) | CC BY-SA 4.0 |
| gazetteer | [factbook.json](https://github.com/factbook/factbook.json) | public domain |
| stars | [Yale Bright Star Catalogue](https://github.com/brettonw/YaleBrightStarCatalog) | public domain |
| constellation lines | [d3-celestial](https://github.com/ofrohn/d3-celestial) | BSD-3 |
| go problems | Lichess puzzle db + Cho Chikun sets | — |

`tools/prepare_dict_fat.py` builds the dictionary (needs
`prepare_dict_compressed.py` beside it), `tools/make_gazetteer.py` the
gazetteer, `tools/make_stars.py` regenerates `stars.h`. The astronomy
verification record is in VERIFICATION.md.

## Power notes

`display.hibernate()` before every deep sleep (without it the e-ink
controller drains the battery in days), 80MHz base clock with 240MHz
bursts for decompression and star math, light sleep between button
polls (`USE_LIGHT_SLEEP` in the sketch — set 0 when debugging over
serial), per-mode idle timeouts, and the clock ticks via RTC alarm
through deep sleep. Deep sleep clears all wake sources before arming
its own — ESP32 wake sources are global and a leftover light-sleep
timer will otherwise wake you 250ms in.
