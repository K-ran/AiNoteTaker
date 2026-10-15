# AI Note Taker

A battery-powered recorder a salesperson carries around the store. It records conversations all day, stores them on an SD card, and uploads them to Smaarthi for transcription and notes.

| Folder | What's inside |
| --- | --- |
| [`firmware/`](firmware/README.md) | ESP-IDF firmware for the ESP32-S3 audio board (P0 pilot) |
| [`docs/`](docs/) | [Field guide](docs/USER_GUIDE.md) (flash, set up, use), [flashing details](docs/FLASHING.md), and the [telemetry guide](docs/TELEMETRY.md) (what each device reports) |
| `server/` | Early local upload test server (pre-Smaarthi) |

Release binaries are attached to the GitHub releases, not committed.

**Security notes:** never commit tokens or Wi-Fi passwords. `references/` (the vendor material and API notebook) is deliberately not in this repo.
