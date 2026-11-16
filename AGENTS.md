# Codex handoff — WilliamsSound

Read **README.md**, **docs/PROJECT_HISTORY.md**, **docs/FUNHOUSE_WPC.md**, and **docs/FIRE_SYSTEM11.md** before changing code.

## Mission
Build an open, robust replacement for Williams pinball sound electronics with **separate** FunHouse WPC and Fire! System 11A adapters and one shared testable audio engine. Start with non-destructive bench and simulation work. Do not erase historical prototypes or silently rewrite findings.

## Current state
- `firmware/funhouse-wpc/FunHouse_WPC_WiFi_Sniffer_v8_REBUILT.ino`: recovered ESP32 **WROOM-32** diagnostic sniffer with web logging, SD and optional DAC test.
- `tools/fire-system11/fire_virtual_esp32_v4_board_aware.py`: recovered Windows Python/PinMAME virtual emulator.
- Neither is a completed replacement product. No physical validation logs or original CSV/WAV datasets are bundled.
- README/docs summarize past work, including outstanding WDEN edge-polarity conflict.

## Priorities (in order)
1. **Repository audit and tests.** Inspect both source files without assuming they build on this host. Document dependencies and current behavior. Add CI-friendly, hardware-free tests of parsing, sequence grouping, event buffering, board-specific routing and WAV header parsing using only synthetic data.
2. **Shared portable audio core.** Implement a tested nonblocking WAV playback state machine and bounded queues on host first. Support interrupts/overlap policies, looping music, effects/voice priority, stop/replace commands, reset and underrun reporting. Define APIs and explicit sample format support.
3. **ESP32 audio bench target.** Add one minimal PlatformIO/Arduino project for a known supported ESP32/WROOM board with SD → I²S DAC test sine/test WAV playback. Avoid GPIO1 UART0/I²S conflict in any new design. Establish exact pin profiles per board.
4. **FunHouse protocol.** Extract bus-decoder logic from v8 to a deterministic, replayable module, preserve raw events, and use real captures to resolve WDEN polarity, timing and sequence gaps.
5. **Fire! protocol.** Extract PinMAME CSV processing into host-testable functions without Windows/Tk dependencies; preserve BOARD0/BOARD1 and avoid conflating PIA strobes with WPC.
6. **Hardware interfaces.** Draft schematics and acceptance tests *after* validating original wiring/pinouts and logic levels. Separate adapters per machine.
7. **Mappings.** Create auditable CSV schemas with status labels `confirmed`, `candidate`, `unknown`; never promote fuzzy name matches to confirmed. Use generated test sounds, no proprietary asset uploads.

## Guardrails
- Do not deploy firmware to real games or assume any GPIO is 5 V tolerant.
- Preserve historical source untouched in its original directory; make new modules under `src/`, new board targets under `firmware/`, and host tests under `tests/`.
- Do not copy Williams ROMs or original audio files into this public repo.
- Clearly distinguish bench-confirmed results, source-code claims, inferred behavior and unverified guesses.
- Prefer small feature branches, tests, PR summaries and reproducible build commands.
- Do not claim physical performance without a hardware result log.

## First Codex assignment
Create a `feature/host-audio-core` branch. Produce: (a) a source audit, (b) a WAV parser and bounded event-queue reference implementation, (c) synthetic regression tests, (d) documentation for running those tests on WSL, (e) an explicit list of open electrical/firmware questions. No hardware deployment, firmware flashes, or changes to historic prototype files. Open a draft PR for review.
