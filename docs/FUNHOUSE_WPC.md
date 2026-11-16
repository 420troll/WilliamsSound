# FunHouse WPC — signal and firmware reference

**Target:** Williams FunHouse (1990), WPC-89 / A-12738 sound board.

## J506 odd-pin signals
| Signal | J506 pin | ESP32 GPIO (v8) |
|---|---:|---:|
| CD7 | 1 | 34 |
| CD6 | 3 | 35 |
| CD5 | 5 | 32 |
| CD4 | 7 | 33 |
| CD3 | 9 | 27 |
| CD2 | 11 | 14 |
| CD1 | 13 | 13 |
| CD0 | 15 | 12 |
| CA4 | 17 | 15 |
| CA3 | 19 | 4 |
| CA2 | 21 | **25** |
| CA1 | 23 | 21 |
| CA0 | 25 | 16 |
| WDEN | 29 | 17 |
| R/W | 31 | 36 |
| RESET | 33 | 39 |

Historical notes/source describe even pins as ground; confirm actual header against the specific WPC schematic and connector orientation, especially before building an IDC passthrough.

## Additional v8 prototype wiring
- Two TXS0108E modules: WPC bus to 5 V “B” side; ESP32 3.3 V to “A” side; OE 3.3 V; grounds common. This documents the experiment, **not** an endorsement of TXS0108E as a robust production interface for every bus topology.
- microSD: CS GPIO5, SCK GPIO18, MISO GPIO19, MOSI GPIO23.
- PCM5102A: BCK GPIO26, LRCK GPIO1 (shared with UART TX0), DIN GPIO22; SCK unused.
- ESP32-WROOM-32 30-pin DevKit, **not** ESP32-S3. Earlier design discussions considered S3, but uploaded firmware pins and libraries target WROOM-32.

## Firmware behavior
- Sound address low bits `CA=0x1C`, WPC write `R/W=0`.
- ISR captures WDEN rising events into a ring; main loop handles SD file writes.
- Live raw pin state and transition counters remain independent of command decoding.
- SD `/buslog.csv`; manual test WAV `/test.wav`.
- Wi-Fi AP `FUNHOUSE-SNIFFER`; web page `http://192.168.4.1`.
- UART uses GPIO1 until I²S playback takes GPIO1 for LRCK; Serial is suspended during test playback. Fix pin conflicts for production.

## Verification priorities
1. Obtain the exact J506 schematic and determine power/ground/header orientation.
2. Capture 5 V logic waveforms with an appropriate high-impedance analyzer; confirm WDEN event edge, R/W and CA bus setup/hold.
3. Verify reset/boot strapping risks and GPIO36/39 input-only behavior.
4. Confirm full command/acknowledge/reply expectations, including multi-byte commands, concurrency, queue overflow and reset behavior.
5. Build isolated/buffered bus input with measured loading; benchmark capture loss while SD/Wi-Fi/audio operate concurrently.
6. Only then build the adapter/complete replacement.
