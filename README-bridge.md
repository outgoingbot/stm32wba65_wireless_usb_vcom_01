# Native USB CDC ACM / IEEE 802.15.4 bridge

## Python latency measurement (Windows)

From this project directory in PowerShell, use the prepared local environment:

```powershell
.\.venv-latency\Scripts\python.exe tests\serial_latency.py COM13 COM16 --baud 115200 --poll-ms 100
```

On another PC, install Python and run `py -m pip install pyserial`, then use
`py tests\serial_latency.py COM13 COM16 --baud 115200 --poll-ms 100`.
If the `py` launcher does not find Python, use your Python executable's full path.
Close other programs holding these ports before starting.

The first port sends; the second receives. Swap them to test the other direction.
`--poll-ms` is the interval between timestamp probes, in milliseconds; reception
runs continuously in a separate thread. Ctrl+C stops and prints statistics.
Add `--count 100` for a bounded run. `--timeout-ms 3000` controls the timeout.
Every probe contains a monotonic timestamp from this PC, a run ID and a sequence
number. Results measure host write to receipt of the complete probe, including
USB, Windows scheduling/buffering, serialization and wireless transport; they are
not pure RF latency. Native USB CDC baud settings do not set the RF bitrate.

On 2026-10-07, COM13 to COM16 at 115200 baud and 100 ms intervals received all
30 probes: median 10.938 ms, mean 11.809 ms, min 6.266 ms, max 24.111 ms.
This small sample is a rough observation, not a reliability guarantee. No firmware
was changed or flashed for this test.

Flash the SAME `artifacts/bridge.hex` onto both STM32WBA65 boards. Build with
`./build-bridge.ps1` (add `-Full` for a complete rebuild). The build does not flash.
BIN load address is 0x08000000. SHA256.txt records the current artifact hashes.

## Hardware bring-up, 2026-10-06

Native USB enumeration was fixed by correcting the USB PHY reference clock:
HSE is 32 MHz, but HAL_PCD_MspInit used SYSCFG_OTG_HS_PHY_CLK_SELECT_1 (16 MHz).
It now uses SYSCFG_OTG_HS_PHY_CLK_SELECT_6 (32 MHz), also saved in the .ioc.
High-speed mode remains enabled. Both probes were programmed and verified with
the same image. Windows now reports both native CDC devices as healthy:

| STLINK probe | MCU UID / USB serial | Native USB |
| --- | --- | --- |
| 066FFF555278665067144745 | 002B00444245500120373942 | COM13 |
| 0671FF555278665067205048 | 003D001A4245500220373942 | COM16 |

COM9 and COM12 are separate USART2 diagnostic adapters inside the STLINK modules.
They are not the native USB CDC interfaces. COM10 is unrelated and is not used.
Port numbers are Windows assignments and can change. USART2 is 115200 8N1.

Other bring-up fixes include a 32 KiB USBX memory pool (the generated 1 KiB pool
failed initialization), PCD start/FIFO setup, USB interrupt handling and standalone
USBX polling. USART2 uses interrupt-driven logging when no DMA handle is present.
Native CDC carries binary payload only; diagnostics go to USART2.

## Validation and remaining limitation

Both radios initialize and mutually discover/pair. Sequential, exact binary
comparisons passed in BOTH directions at lengths 1, 63, 64, 65, 95, 96, 97,
511, 512 and 513 bytes. The 1024-byte case timed out. An earlier simultaneous
exchange passed 1 and 63 bytes but timed out at 64 bytes; diagnostic counters
showed a MAC no-ack error (0xE9), with USB input received correctly. Therefore
native USB enumeration is fixed, but sustained/simultaneous wireless delivery
is not yet reliable. Do not treat this firmware as a lossless serial replacement.

Run `./tests/hardware_bridge_check.ps1 -PortA COM13 -PortB COM16` for sequential
binary checks with .NET SerialPort. The Python test exercises simultaneous
transfers and requires pyserial. Close applications holding native CDC ports first.

## Shared-image pairing and data path

Each MCU obtains its radio EUI through the bundled vendor otPlatRadioGetIeeeEui64
API. The current library constructs this from company ID and part of the UDN;
the complete 96-bit UID is also exchanged to detect duplicate radio identities.
USB serial strings use all 96 UID bits. There are no separate A/B firmware roles.

Discovery uses PAN 0xCAFE, channel 15, periodic broadcast HELLO packets and mutual
peer selection before forwarding. An additional distinct peer or identity collision
stops forwarding until reset. An offline peer remains pinned; it is not silently
replaced. DATA packets use extended addressing, MAC acknowledgments, and up to
64 user bytes. Each direction has a 2048-byte queue. Failed MAC transmissions
currently count/drop data; there is no application-level retransmission or
sequence-based duplicate suppression. Power saving is disabled for bring-up.

## Source and regeneration

Application bridge logic is in Core/Src/wireless_bridge.c, with integration through
USER CODE blocks. Preserve those blocks when regenerating. USB IRQ and PHY clock
selection, USART2 IRQ and logging enable are recorded in the .ioc. makefile.defs
adds the bridge object when generated build rules have not discovered it yet.
The missing app_usbx.h include in peripheral_init.c fixed a locally reproduced
baseline compile failure; disabled generated IRQ settings are configuration issues,
not evidence of a general CubeMX defect.

Original full application-flash backups are retained as artifacts/before-*.bin.
No option bytes, OTP, or mass erase operations were used.
