# Fire! System 11A — virtual emulator and future hardware

**Target:** Williams Fire! (1987), System 11A / D-11581 sound board.

## Recovered program
`tools/fire-system11/fire_virtual_esp32_v4_board_aware.py` is a **Windows desktop emulator/tool**, not ESP32 firmware. It reads a PinMAME-generated `fire_s11_bus_trace.csv`, chooses BOARD0 or BOARD1, identifies 0→1 CA2/CB2 strobes as candidate sound events, uses a locally provided `fire_f4_verified_mapping.csv`, and optionally plays WAVs with Windows `winsound`.

Historical local paths default to `C:\VPinMAME\fire_trace_build\Release\` and `C:\VPinMAME\pinmame\fire_audio\`. These are local development conventions, not files in this repo.

A v4 test UI references a candidate `BOARD1:01` → `F4:0101` correspondence. Do not assume this is an exhaustive validated map.

## Known limitations
- Does **not** emulate electrical timing, analog audio or exact hardware handshake.
- A trace-driven “rising strobe” interpretation does not automatically define D-11581 pin numbers or logic levels.
- The CSV mapping and sound recordings are local data dependencies, not supplied here.

## Next verification
1. Identify full D-11581 connector pinout from primary schematic/manual.
2. Confirm which CPU/PIA drives each command path and how board ID is encoded.
3. Record live logic-level traces (including idle, reset, service mode, and simultaneous effects).
4. Determine whether multiple sound devices and audio mixes must coexist.
5. Implement a host-testable command parser and a separate ESP32 protected hardware input layer.
6. Measure audio output levels, grounding, noise and amplifier interface before board substitution.
