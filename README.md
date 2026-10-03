# AI Note Taker

A battery-powered recorder a salesperson carries around the store. It records conversations all day, stores them on an SD card, and uploads them to Smaarthi for transcription and notes.

| Folder | What's inside |
| --- | --- |
| [`firmware/`](firmware/README.md) | ESP-IDF firmware for the ESP32-S3 audio board (P0 pilot) |
| [`docs/`](docs/) | [Flashing guide](docs/FLASHING.md) and [user guide](docs/USER_GUIDE.md) for the team |
| [`tools/recordings_viewer.py`](tools/recordings_viewer.py) | Local web page to browse and play uploaded recordings |
| `server/` | Early local upload test server (pre-Smaarthi) |

Release binaries are attached to the GitHub releases, not committed.

**Security notes:** never commit tokens or Wi-Fi passwords. `references/` (the vendor material and API notebook) is deliberately not in this repo.
