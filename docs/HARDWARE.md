# Development and hardware verification

## FunHouse — WPC
Recovered source: `firmware/funhouse-wpc/FunHouse_WPC_WiFi_Sniffer_v8_REBUILT.ino`.

The firmware targets an ESP32-WROOM-32 development board and assumes TXS0108E translators as described in its header. Confirm the actual pin map, voltage levels, bus direction, drive strength and strobe capture before connecting to a machine. The existing firmware is a diagnostic prototype, **not** validated replacement-board firmware.

## Fire! — System 11A
Recovered source: `tools/fire-system11/fire_virtual_esp32_v4_board_aware.py`.

This Python program is a Windows/PinMAME trace-driven audio command monitor, not an ESP32 firmware image. It requires local trace and WAV assets and does not simulate electrical timing.

## Planned milestones
1. Preserve and regression-test the existing command trace mappings.
2. Add a host simulation with synthetic traces and generated test audio.
3. Implement nonblocking ESP32 SD-card WAV playback with I²S DAC output.
4. Design and verify machine-specific buffered, protected command interfaces.
5. Validate command timing on a bench before testing a physical game.

No proprietary Williams ROMs, game sound samples or copyrighted sound assets are part of this repository.
