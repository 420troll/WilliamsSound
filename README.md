# WilliamsSound — TROLLfx Pinball Sound Replacement

Development workspace for replacement sound electronics for Williams pinball machines.

## Existing recovered prototypes
- `firmware/funhouse-wpc/`: FunHouse WPC experimental ESP32-WROOM-32 Wi-Fi sound-command sniffer. **Not a drop-in replacement sound board.**
- `tools/fire-system11/`: Fire! System 11A board-aware virtual command/audio emulator for Windows / PinMAME.

## Development targets
- FunHouse (WPC, A-12738 sound board): verify protected command-bus capture, then SD/WAV/I²S playback.
- Fire! (System 11A, D-11581): verify separate command paths and real hardware timings, then implement audio engine.
- Keep each machine's electrical interface and test suite separate.

## Safety and status
**Experimental software only.** Do not connect ESP32 GPIO directly to 5 V game signals. Confirm connector pinout, logic polarity, electrical buffering, command timing, supply/ground and amplifier interfacing before testing on a physical game. No game ROMs or copyrighted audio assets are included.

Initial GitHub import of recovered project sources. Next milestone: testable ESP32 audio playback and command mapping.
