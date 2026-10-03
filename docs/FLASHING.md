# Flashing the P0 firmware

Firmware **0.1.2-p0** for the Waveshare ESP32-S3-AUDIO-Board (ESP32-S3R8, 16 MB flash).

| File | Use |
| --- | --- |
| `notetaker-p0-0.1.2-full.bin` | **New or wiped boards.** One file, written at address `0x0`. |
| `notetaker-p0-0.1.2-app.bin` | **Updating** a board that already runs P0. Keeps its Wi-Fi, names and token. Written at `0x20000`. |
| `SHA256SUMS` | Checksums for both files |

Download them from the GitHub release **v0.1.2-p0**.

> The full image overwrites the settings area. After flashing it, the board has to be set up again (Wi-Fi, store, salesperson, token). See the [user guide](USER_GUIDE.md).

## Option A: in the browser (no installs)

Works in **Chrome or Edge** on Mac, Windows or Linux.

1. Plug the board into your computer with a USB-C **data** cable.
2. Open **https://espressif.github.io/esptool-js/** and press **Connect**. Pick the port that appears (on a Mac it looks like `cu.usbmodem…`).
3. Optional but recommended for a clean start: press **Erase Flash** and wait for it to finish.
4. In the flash table, set **Flash Address** to `0x0` and choose `notetaker-p0-0.1.2-full.bin`.
5. Press **Program**. It takes about 30 seconds.
6. Press the board's **RESET** button (or unplug and replug it). The status light starts its dim white blink: the board is recording.

## Option B: command line (esptool)

```sh
pip install esptool            # or use the one inside ESP-IDF
PORT=/dev/cu.usbmodem1101      # Windows: COM5 etc.

# new / wiped board
esptool.py --chip esp32s3 -p $PORT erase_flash
esptool.py --chip esp32s3 -p $PORT -b 460800 write_flash 0x0 notetaker-p0-0.1.2-full.bin

# update an existing P0 board (keeps its settings)
esptool.py --chip esp32s3 -p $PORT -b 460800 write_flash 0x20000 notetaker-p0-0.1.2-app.bin
```

## Option C: build from source

```sh
. ~/esp/esp-idf/export.sh                 # ESP-IDF v5.5
cd firmware
idf.py build
idf.py -p /dev/cu.usbmodem1101 flash      # flashes bootloader, partitions and app
```

## If flashing fails with "No serial data received"

1. Unplug and replug the USB cable, then try again. This fixes it most of the time.
2. Still failing: hold **BOOT**, tap **RESET**, release **BOOT**. The board is now in download mode. Flash again, then press **RESET**.
3. Use a different cable: many USB-C cables only carry power.

## Check it worked

Optional, for developers. Open a serial terminal at 115200 baud (for example `python firmware/tools/devserial.py --reset --seconds 20 --cmd 15:status`). You should see:

```
device nt-xxxxxxxxxxxx fw 0.1.2-p0 boot 1
... rec recording ...
```
