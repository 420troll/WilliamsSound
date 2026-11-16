# Project history and recovered findings

This document records prior experimental work and its confidence level. Hardware details should be verified against schematics and bench measurements before implementing a replacement.

## Goal and architecture
Replace aging Williams pinball sound electronics while preserving game CPU interaction. Two **distinct** targets:
- **FunHouse (1990):** WPC-89, original WPCS/A-12738 sound board, ESP32-based capture/playback candidate.
- **Fire! (1987):** System 11A, D-11581 sound board, separate ESP32-compatible command decoder and sound engine.

Planned replacement path: protected/buffered command interface → input event queue → per-game command mapping/sequencing → SD-card WAV/audio engine → I²S DAC (e.g. PCM5102A) → amplifier. A PCM1808 is an **ADC**, not a playback DAC.

## FunHouse chronology
1. Investigated the WPC J506 address/data/control lines and `$3FDC` sound command writes; proposed a passive pass-through IDC analyzer so the original sound board remains operational while logging.
2. Built ESP32/WROOM-32 prototypes using two TXS0108E logic-level modules and microSD logging. Early source revisions differed in control wiring and decoding.
3. Produced progressively more elaborate sniffer firmware: raw bus monitoring, 1–6 byte sequence grouping (historical grouping window approximately 3 ms), Wi-Fi monitoring and logging, and later on-demand PCM5102A DAC test playback.
4. A hardware-revision correction changed **CA2 from GPIO2 to GPIO25**; the v8 rebuilt source explicitly documents GPIO25 as the working connection. Do not copy the earlier mapping uncritically.
5. Prior PinMAME/PinSound analysis reported **109 extracted command entries** and **223 sound-pack events**, but command-to-asset coverage is not complete or necessarily exact. Keep raw captures authoritative.

### Sound pack investigation
Historical `Funhouse_Original.pspack` inventory: 223 items = 102 voice + 86 SFX + 27 music + 8 jingles. Earlier spreadsheet analysis classified only **5 exact**, **24 possible** and **194 unresolved** mappings; this is not a completed 223-entry command map. Reported examples include `07` → “midnight” and `84` → “outlane”. A multi-byte example `7A 2B` was used for “I thought we were pals!” in an earlier sniffer prototype. These associations must be re-verified against live captures and playback before release. Keep proprietary sound assets, ROM images and extracted music out of this public repository.

### Register/address notes (research leads, not all bench-confirmed)
- WPC main CPU sound command write: `$3FDC`, address lines CA4..CA0 `11100` / `0x1C`.
- Historical notes mention `$3FDD` control/status, sound CPU `$3000` receive, `$3C00` reply, and `$2000` bank select.
- Historical notes mention ROM filenames `funh_l9.rom`, `fh_u18.sl3`, `fh_u15.sl2`, `fh_u14.sl2`; filenames are references only, no ROM contents included.
- Some earlier notes describe a **falling WDEN** edge, while v8 firmware explicitly captures the **rising WDEN** edge with R/W low. **This is an unresolved edge-polarity/timing discrepancy.** Confirm against schematics and logic-analyzer captures. Do not declare either edge universally correct from the notes alone.
- Commands may be multi-byte; single-byte-to-single-WAV assumptions are insufficient.

## Fire! chronology
1. Explored virtual sound-board command paths using PinMAME `fire_s11_bus_trace.csv`.
2. Built Windows standard-library Python virtual ESP32 emulator, later board-aware v4.
3. v4 watches board-specific CA2/CB2 **0→1 strobe transitions** as *candidate* events and uses `fire_f4_verified_mapping.csv` with local WAV recordings. It has BOARD0/BOARD1 selection and traces, and a documented BOARD1:01 → F4:0101 test path.
4. The program **does not emulate electrical timing**. Its virtual events/mappings do not establish physical D-11581 pinout, voltage levels or replacement compatibility.

## Cross-project decisions
- Do not wire an ESP32 GPIO directly to a Williams 5 V bus.
- Preserve raw traces, timestamped command bytes and board identifiers.
- Separate virtual PinMAME decoding from live hardware timing and electrical design.
- Avoid conflating the Fire! System 11A bus with FunHouse WPC.
- Develop and regression-test the audio engine on the bench before any physical board substitution.

## Evidence and outstanding questions
The code in this repository is preserved as *historical prototypes*. Statements labeled “verified” inside prior source reflect tests done at that time, but those tests are not independently reproduced by this repository's CI. Missing: fully versioned capture logs, the original 109-command table, audio licensing clearance, Fire! schematics and measured signals, validated output amplifier wiring, and reproducible end-to-end hardware test reports.
