# Device telemetry guide

What each Note Taker device reports to Smaarthi about its own health, how often, and what every field and event means. Applies to firmware **0.1.4-p0** and later.

## The short version

- Every device sends two kinds of record: **heartbeats** (a health snapshot every 5 minutes) and **events** (something happened, sent as it happens).
- They're sent with the audio uploads, every **15 minutes**. Telemetry never turns Wi-Fi on by itself, so it costs almost no battery.
- If Wi-Fi is down, records wait on the memory card and go out once the device is back online. Nothing is lost unless the queue grows past 2 MB, which takes days offline.
- Each record says which device it came from, which power-on session (boot) it belongs to, and when it was captured.

## How to read a record

Every record, heartbeat or event, carries these:

| Field | Example | Meaning |
| --- | --- | --- |
| `deviceId` | `cmutievfo0000s245v67o5mnx` | The device's Smaarthi ID, entered on the setup page ("Karan 1") |
| `eventId` | `c34a93df-01ae-4544-…` | Unique ID for this record. If the device sends it twice, Smaarthi stores it once. |
| `bootId` | `nt-288485b2ae28-26` | Board ID + power-on count. Every record from the same power-on shares it, so a new `bootId` means the device restarted. |
| `recordedAt` | `2026-10-15T15:56:50Z` | When the device captured it, in UTC |

If a device had no correct time when it captured a record (for example after a long time unpowered with no internet), `recordedAt` is the best estimate and the record's `attributes` include `"timeEstimated": true`.

## Heartbeats (every 5 minutes)

The first heartbeat comes 30 seconds after the device starts, then one every 5 minutes, so three heartbeats arrive with each 15-minute upload. All values are inside `attributes`.

### Device

| Field | Example | Meaning |
| --- | --- | --- |
| `firmwareVersion` | `0.1.4-p0` | Software version on the device |
| `boardId` | `nt-288485b2ae28` | ID built into the hardware; never changes |
| `uptimeSec` | `3600` | Seconds since the device last started |
| `bootCount` | `26` | How many times this device has started, ever |
| `resetReason` | `power_on` | Why it last started: `power_on`, `usb`, `software`, `external`, `deep_sleep`, or a problem: **`crash`**, **`watchdog`** (it froze and restarted itself), **`brownout`** (battery voltage dropped too low) |

### Recording (`recording`)

| Field | Example | Meaning |
| --- | --- | --- |
| `state` | `recording` | `recording`, `paused` (staff pressed privacy pause) or `stopped` (no memory card, or card full) |
| `audioLevelDbfs` | `-48` | Average loudness of the last few seconds. Around `-50` to `-35` is normal speech in a room; near `-70` or below is near silence. A device stuck near silence during store hours may have a covered mic or be in a drawer. |
| `chunksRecorded` | `0`–`1` | 10-minute files finished since the previous heartbeat |
| `bufferOverruns` | `0` | Audio dropped because the device couldn't keep up, counted since start. Should stay `0`. |
| `lastSeq` | `241` | Number of the last finished audio file (`0` until the first one finishes after a start). Gaps in the numbers seen in Smaarthi mean missing audio. |

### Storage (`storage`)

| Field | Example | Meaning |
| --- | --- | --- |
| `sdPresent` | `true` | Memory card found and working. `false` means the device can't record. |
| `sdTotalMb` / `sdFreeMb` | `29638` / `10243` | Card size and free space, in MB. The device stops recording below 50 MB free. |
| `backlogFiles` | `1` | Audio files waiting to upload. 0–2 is normal. |
| `backlogMb` | `0.24` | Size of those files, in MB |
| `oldestBacklogAgeMin` | `14` | Age of the oldest waiting file, in minutes. Normal is under 20. Hours means uploads are stuck. Missing if the clock isn't set. |
| `telemetryPending` | `0` | Telemetry records still waiting to send (approximate) |
| `telemetryDropped` | `0` | Records thrown away since the previous heartbeat because the queue was full. Should be `0`. |

### Uploads (`uploads`)

| Field | Example | Meaning |
| --- | --- | --- |
| `chunksUploaded` | `1` | Audio files delivered since the previous heartbeat |
| `bytesUploaded` | `1348402` | Their size in bytes (about 1.8 MB per 10 minutes) |
| `lastError` | `""` | Result of the last upload attempt: empty means fine; `wifi` (couldn't join Wi-Fi), `network` (no internet), `server` (Smaarthi had a problem), `auth` (token rejected), `rejected` (Smaarthi refused a file), `local` (a problem on the device) |

### Wi-Fi (`wifi`)

| Field | Example | Meaning |
| --- | --- | --- |
| `rssiDbm` | `-54` | Signal strength at the last upload. `-30` to `-60` is good, `-60` to `-75` is fair, below `-75` is weak. `0` means not measured yet. |
| `connectMs` | `3200` | How long joining Wi-Fi took at the last upload, in milliseconds |
| `lastFailReason` | `0` | Why the last Wi-Fi attempt failed. `0` means it didn't fail. Common codes: `201` network not found, `202` or `204` wrong password, `15` password handshake timed out. |

### Time (`time`)

| Field | Example | Meaning |
| --- | --- | --- |
| `source` | `ntp` | Where the clock came from: `ntp` (internet time, best), `http` (taken from Smaarthi's server), `rtc` (the board's own clock, kept since the last sync), `none` (unknown) |
| `lastNtpSyncAt` | `2026-10-15T15:56:50Z` | When internet time was last received |

### Health (`health`)

These are for engineers. They should stay roughly flat over days; a steady fall means a memory leak.

| Field | Example | Meaning |
| --- | --- | --- |
| `heapInternalFree` | `88803` | Free fast memory, in bytes |
| `heapInternalMin` | `40008` | Lowest it has been since start. Worry below about 20000. |
| `heapInternalLargest` | `31744` | Largest free block, in bytes. Should stay steady; if it shrinks, memory is fragmenting. |
| `psramFree` | `7285484` | Free large memory, in bytes |
| `stackFreeRecorder` / `stackFreeUplink` | `9668` / `5724` | Spare working memory of the recording and upload tasks, in bytes. Worry below about 1000. |
| `chipTempC` | `41.5` | Processor temperature in °C. Normal is 30–60. |

### Power (`power`)

| Field | Example | Meaning |
| --- | --- | --- |
| `batteryMonitoring` | `false` | Whether this board can measure its battery. On the P0 prototype boards it can't, so there's no battery level yet. |
| `batteryMv` | `3920` | Battery voltage in millivolts; only present when monitoring is available |

## Events (sent as they happen)

Each event has a `name`, a `severity`, and a short `message` with the details. Severities run `Debug` (background detail), `Info` (normal), `Warn` (worth a look), `Error` (needs action).

### Device

| Event | Severity | When | Example message |
| --- | --- | --- | --- |
| `device.boot` | Info | The device started | `fw=0.1.4-p0 reset=1 sd=1` |
| `device.boot` | **Error** | The device restarted after a crash, a freeze or a battery dip | `fw=0.1.4-p0 reset=4 sd=1` |

In the message, `sd=1` means the memory card was found. `reset=` is a number: `1` power on, `3` software restart, `4` crash, `5`–`7` froze and restarted itself, `9` battery voltage too low, `11` USB.

### Recording and storage

| Event | Severity | When | Example message |
| --- | --- | --- | --- |
| `recording.paused` | Info | Staff pressed privacy pause | `auto-resume scheduled` |
| `recording.resumed` | Info | Pause ended: pressed again, or automatically after 10 minutes | `resumed` / `auto-resume` |
| `storage.power_cut_recovered` | Warn | After losing power mid-recording, the device rescued the file it was writing (up to 10 seconds lost) | `seq=239` |
| `storage.sd_fault` | **Error** | Memory card missing or failing; the device can't record | `no card: not recording` / `write failed seq=240` / `3 errors, remounting` |
| `storage.sd_full` | Warn | Card nearly full; recording stopped, nothing deleted | `stopped recording; backlog kept` |
| `audio.buffer_overrun` | Warn | Some audio was dropped because the device couldn't keep up | `frames=12 seq=240` |

### Uploads and connection

| Event | Severity | When | Example message |
| --- | --- | --- | --- |
| `upload.completed` | Info | An audio file reached Smaarthi and became a conversation | `seq=241 conv=cmv9q0uu40006pd0lhk3v11ov url=…` |
| `upload.failed` | Warn | An upload attempt stopped partway; the device retries in 15 minutes | `result=network uploaded=0 of 2` |
| `upload.quarantined` | **Error** | Smaarthi refused the same file 5 times; it's set aside on the card | `seq=241 moved to BAD` |
| `auth.failed` | **Error** | Smaarthi rejected the API token; uploads pause until it's fixed | `uploads paused until the token is fixed` |
| `wifi.connect_failed` | Warn | Couldn't join the store Wi-Fi | `reason=201 backlog=2` (see the Wi-Fi codes above) |
| `time.synced` | Debug | The clock was set from internet time | `ntp` |

### Setup

| Event | Severity | When | Example message |
| --- | --- | --- | --- |
| `setup.opened` | Info | Someone opened the setup hotspot | `hotspot NoteTaker-ae28 for 900 s` |
| `setup.saved` | Info | New settings were saved after a successful Wi-Fi test | `ssid=StoreStaff store=Indiranagar sales=Karan devid=cm… token=kept` |
| `setup.failed` | Warn | The Wi-Fi details entered didn't work; nothing was saved | `ssid=StoreStaff reason=204` |
| `setup.closed` | Info | The setup hotspot closed | `window ended` |

`firmware.confirmed` (Info) will appear after over-the-air updates, which arrive in a later phase.

## What to watch for

| You see | It probably means | Do this |
| --- | --- | --- |
| No records from a device for 45+ minutes in store hours | Switched off, battery flat, or out of Wi-Fi | Check the device and its charge |
| `device.boot` with severity Error, or `resetReason` `crash`/`watchdog`/`brownout` | A crash or a battery problem | Send the time and device to the firmware team |
| `storage.sd_fault`, or `sdPresent: false` | No working memory card: **not recording** | Reseat or replace the card |
| `oldestBacklogAgeMin` over 240, or repeated `upload.failed` | Uploads stuck | Check the store Wi-Fi |
| `auth.failed` | Token rejected | Redo setup with a valid token |
| `wifi.connect_failed` with `reason=202`/`204` | Wi-Fi password changed | Redo setup with the new password |
| `recording.state: paused` for a long time | Privacy pause left on (it ends by itself after 10 minutes) | Nothing, unless it keeps repeating |
| `audioLevelDbfs` near `-70` all day during store hours | Mic covered, or device in a drawer | Check where the device is kept |
| `rssiDbm` below `-75` | Weak Wi-Fi where the device is carried | Expect slower uploads; consider a better access point |
| `heapInternalMin` falling day after day | Memory leak | Tell the firmware team |

## Example: one upload's worth of telemetry

```json
{
  "deviceId": "cmutievfo0000s245v67o5mnx",
  "heartbeats": [{
    "eventId": "7f1c2a0e-5b3d-4c6e-9a81-2d4f6b8c0e13",
    "bootId": "nt-288485b2ae28-26",
    "recordedAt": "2026-10-15T15:56:20Z",
    "attributes": {
      "firmwareVersion": "0.1.4-p0", "boardId": "nt-288485b2ae28",
      "uptimeSec": 30, "bootCount": 26, "resetReason": "power_on",
      "recording": { "state": "recording", "audioLevelDbfs": -50, "chunksRecorded": 0, "bufferOverruns": 0, "lastSeq": 0 },
      "storage": { "sdPresent": true, "sdTotalMb": 29638, "sdFreeMb": 10244, "backlogFiles": 1, "backlogMb": 0.24,
                   "oldestBacklogAgeMin": 1, "telemetryPending": 0, "telemetryDropped": 0 },
      "uploads": { "chunksUploaded": 0, "bytesUploaded": 0, "lastError": "" },
      "wifi": { "rssiDbm": -54, "connectMs": 3200, "lastFailReason": 0 },
      "time": { "source": "ntp", "lastNtpSyncAt": "2026-10-15T15:56:50Z" },
      "health": { "heapInternalFree": 88803, "heapInternalMin": 49088, "heapInternalLargest": 31744,
                  "psramFree": 7285484, "stackFreeRecorder": 9668, "stackFreeUplink": 5724, "chipTempC": 41.5 },
      "power": { "batteryMonitoring": false }
    }
  }],
  "events": [{
    "eventId": "c34a93df-01ae-4544-81ff-89e07fe0eaef",
    "bootId": "nt-288485b2ae28-26",
    "name": "device.boot", "severity": "Info",
    "message": "fw=0.1.4-p0 reset=1 sd=1",
    "recordedAt": "2026-10-15T15:55:50Z",
    "attributes": {}
  }]
}
```
