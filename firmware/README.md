# Note Taker firmware (P0)

ESP-IDF v5.5 firmware for the Waveshare ESP32-S3-AUDIO-Board. It records all day (16 kHz mono, Opus 24 kbps in Ogg), stores audio on the SD card in 10-minute chunks, and uploads them every 15 minutes through the Smaarthi API (presigned URL → R2 PUT → createAsset → createConversation with the device's Smaarthi `deviceId`). A chunk is deleted only after its conversation exists.

Design background: the AI Note Taker design doc and the "Counter Recorder: how it works" deck. Team docs: [flashing](../docs/FLASHING.md), [user guide](../docs/USER_GUIDE.md).

## Build and flash

```sh
. ~/esp/esp-idf/export.sh
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash
./test/run_host_tests.sh          # host test for the Ogg Opus muxer (needs: brew install opus opus-tools)
```

Release images: `cd build && python -m esptool --chip esp32s3 merge_bin -o ../release/notetaker-<ver>-full.bin --flash_mode dio --flash_size 16MB --flash_freq 80m @flash_args`.

## Layout

```
main/
  main.c              boot order: config -> board -> storage -> recording -> network/UI
  app_config.h        every P0 tunable (future remote settings live here)
  core/               device_config (NVS), event_log (SD JSON lines), timekeep (RTC+NTP+HTTPS Date), console
  board/              PCF85063 RTC, battery ADC (off until the BAT_ADC resistor is fitted)
  audio/              capture (I2S -> PSRAM ring), recorder (Opus -> Ogg -> SD), ogg_opus (pure C, host-tested)
  storage/            chunk_store: .PRT/.OGG/.JSN files, crash recovery, sequence seeding, remount
  net/                wifi_link (session ownership), smaarthi_api, uplink (upload windows), setup_portal (hotspot page)
  ui/                 status_led (flags per cause), buttons
  hardeware_driver/   vendor BSP (unchanged)    tca9555_driver/  vendor IO expander (unchanged)
tools/devserial.py    non-interactive serial log/commands (redacts NT_* secrets)
test/                 host tests
old_demos/            earlier experiments, not built
```

### Tasks

| Task | Core / prio | Stack | Job |
| --- | --- | --- | --- |
| capture | 1 / 7 | 4 KB | codec → 30 s PSRAM ring; never blocks on SD or network |
| recorder | 1 / 6 | 32 KB internal | Opus encode (libopus needs ~22 KB stack), Ogg pages, fsync every 10 s, 10-min rotation, TWDT-subscribed |
| uplink | 0 / 4 | 10 KB | upload windows, status snapshot |
| portal | – / 4 | 6 KB | setup hotspot start/stop/test (requests come from buttons, console, timer) |
| buttons, led, console | – / 2–3 | 3–4 KB | staff input, status light, developer commands |

### Data safety rules

- Audio is deleted in one place only: `uplink.c` after `createConversation` succeeds.
- Every upload step is recorded in the chunk's `.JSN` sidecar before moving on, so a reboot resumes. The sidecar is written as `.JST` (fsynced), then promoted.
- A `.PRT` left by a power cut is closed as a finished chunk on the next boot (at most 10 s lost).
- Sequence numbers are seeded above anything already on the card, so they're never reused after an NVS erase.
- R2's ETag is checked against the MD5 of what was sent.
- Only explicit validation errors (`BAD_USER_INPUT`) count towards quarantine. Network, server and unknown errors retry indefinitely.

## Developer console (USB serial, 115200)

`status | upload | rotate | pause | resume | setup | setup-off | reboot | wifi <ssid> <pass> | names <store> <sales> | devid <smaarthi-device-id> | token <jwt> | rec-clear yes`

Pass secrets through the environment so they stay out of shell history and logs:

```sh
NT_TOKEN=... python tools/devserial.py --seconds 5 --cmd '0:token $NT_TOKEN'
```

Disabled with `DEV_CONSOLE_ENABLED 0` in `app_config.h` (production).

## Verified on hardware (2026-10-03)

- End-to-end uploads (10+ chunks): downloaded file's MD5 = device MD5 = R2 ETag; valid Ogg Opus with metadata tags; speech audible.
- Power cut mid-chunk: recovered on boot and uploaded.
- Wrong Wi-Fi password: recording continues, backlog kept, uploads resume after the fix.
- Invalid token: `auth_failed`, uploads paused, nothing quarantined, resumes with a valid token.
- Full erase + release image: boots unconfigured, records, seeds the sequence above the card's chunks.
- 8 back-to-back upload cycles: internal heap flat (≈94 KB free, min 41 KB); PSRAM settled after ~2.6 KB.
- Setup hotspot comes up (AP+STA) and closes on schedule. **Not yet tested from a phone**: the form, the save-after-test flow, and the LAN-request rejection.

## Known gaps (planned)

- **P1:** SD encryption, telemetry endpoint (status/events are already queued in `LOG/`), OTA (A/B partitions and rollback are in place), Opus/CPU power tuning, retrying BAD chunks daily, TLS connection reuse.
- **P2:** per-device tokens and setup codes, Secure Boot and flash encryption, installer app, server-side stitching.
- Battery monitoring needs the BAT_ADC resistor (`BATTERY_ADC_FITTED`).
