# Usable dual-board release checkpoints

## 2026-10-04 usable baseline before further reliability changes

Source baseline: `000b7f8` on `classic-hid-output`; the existing `v0.6.3`
tag remains unchanged. This checkpoint is usable, not a claim that every
failure/recovery path or four-PC radio-capacity target has passed acceptance.

| Board | Deployed firmware | Image SHA256 |
| --- | --- | --- |
| ESP32 | `0.6.5-reconnect-phases` | `2D0707A5747ABB8AC18EECDDF52243C0B48527F777C14735C893E4E42B475136` |
| Pico 2 W | `0.6.2-hid-dedup` | `2640C2197B08C6F8CEBA6F06A460000964C7F39D33E9F6E7134861D5A2D32F95` |

The 12:53 observation confirmed encrypted/HID-ready FILCO and M585,
keyboard reports increasing from 300 to 339, selected PC1 ready, output queue
empty, overflow zero and UART framing errors zero. These are runtime counters,
not end-to-end latency or sustained Mac acceptance. Five saved input records,
seven saved PC identities, physical enrollment and existing bonds are retained.

Known follow-up work: user-code pairing must not inherit the short HID-setup
deadline; missing SDK callbacks can hold a worker and rollback indefinitely;
scan cancellation needs a terminal result; persistent old-PC release errors
can block switching. Diagnostic printing/storage still hold the input mutex,
ESP32 mouse peripheral-latency control is not wired to Pico's mouse-active
setting, and old generic errors can outlive successful recovery. See
`HANDOFF-SOFTAP-WEB.md` for deployment and acceptance details.

Firmware images, NVS/flash backups, credentials and raw device logs are excluded.
This checkpoint does not push a branch, change a tag or flash another image.

## v0.6.3 — usable dual-board release checkpoint

Date: 2026-10-03.

This release captures the ESP32 + Pico 2 W implementation and its known
verification limits. “Usable” identifies a development release checkpoint,
not certification of every input device, radio-capacity target or pairing flow.

## Firmware and deployment

| Board | Source/build identity | Last deployed identity |
| --- | --- | --- |
| Original dual-mode ESP32 | `0.6.3-auto-input` | `0.6.3-auto-input` |
| Pico 2 W | `0.6.1-fault-lamp` | `0.6.0-slots-buttons` |

The Pico source includes the requested new fault-lamp pattern, but that Pico
image has not been flashed. The last deployed Pico already supports the same
seven-slot UART status/management protocol. Do not assume identical version
numbers or that a source tag automatically updates attached boards.

Last deployed ESP32 application SHA256:

```text
A275AE4557A7CC464A4BC548C7DF20F700F4F9251783A1ACD2D873D6A8BD371C
```

Last deployed Pico UF2 SHA256:

```text
D0259472D9C19493E7ED9C59127166D13AC281DE0860A81F9BD55CB8DE0F100F
```

Pico `0.6.1-fault-lamp` built from this release, not yet deployed, UF2 SHA256:

```text
5855ED27B13BCA68A568D0567DD7EB8C61E0EE78E56030DA35222F891A208755
```

Firmware binaries, NVS/Flash backups, pairing keys and the device-local hotspot
password are not included in Git. Build timestamps may change binary hashes.

## Changes since the Bluetooth-only checkpoint

- ESP32 SoftAP and local HTTP management at `http://192.168.4.1/`, including
  active-input status, seven PC slots, explicit stale/error states and
  physically authorized input/PC deletion.
- Five saved input records, with one active keyboard and one active mouse;
  seven saved PC identities, retaining four peripheral radio contexts rather
  than promising seven simultaneous connections.
- BOOT long-press/release starts bounded BLE and Classic discovery and
  automatically admits a unique, previously unknown, explicitly classified
  keyboard/mouse. Known identities and hub broadcasts are excluded; ambiguous
  or unclassified candidates require manual selection.
- Target-specific encryption/HID validation before committing new input records,
  old-record preservation on failed admission, and FIFO eviction only after
  successful admission when full.
- Persistent device names with CRC-checked registry migration, authorized
  naming, readable deletion controls and explicit disabled-button reasons.
- Isolated BLE open worker, automatic recovery/backoff for transient failures,
  callback-owned HID pointers and request/ACL-generation guards.
- Runtime board buttons, normal lamps OFF, and shared fault indication of
  two seconds solid ON followed by two seconds of 100 ms ON/OFF flashing.
- USB-only persistent hotspot-password configuration; no hard-coded local
  hotspot credential in source or HTTP responses.

## Verified observations and limitations

Encrypted FILCO and M720 forwarding and encrypted PC1 BLE HID output were
physically observed in earlier sessions. The current protocol's seven-slot
Pico runtime, retained PC identities, PC1 at 7.5 ms, mouse-buffer OFF and UART
freshness were confirmed after deployment. ESP32 application updates were
verified while preserving NVS; Pico program/non-program readback was verified.

Native regression coverage includes production input-admission functions,
unique/ambiguous auto-selection, encryption/role gates, persistent-name and
legacy-record migration, rollback/FIFO/identity deletion, timer boundaries,
framing/freshness and existing output behavior. Eleven real-browser fixture
scenarios cover the management page, names and nonempty deletion controls.
Both firmware builds pass.

Actual automatic BLE/Classic scan cycling and Pico-broadcast exclusion were
observed on ESP32. M585/M590 was not advertising in that observation, so
successful physical auto-pairing of that new mouse is **not yet confirmed**.
Physical-button enrollment, latest phone-page appearance and real destructive
management operations still need acceptance. USB CH340 intermittently
disappears or reports access errors. SoftAP with active clients plus Classic/BLE
is an ESP-IDF C1 coexistence scenario; unchanged input smoothness and long-term
stability with a web client are not guaranteed. Four-PC concurrent operation
and complete physical switching acceptance also remain unverified.

## Build and checks

```powershell
.\tools\build-esp32.ps1 -BuildDirectory build\esp32-slots-buttons
.\tools\build.ps1 -DualBluetooth -BuildDirectory build\dual-slots-buttons -Jobs 4
.\tests\test-dual-bt.ps1
.\tests\test-output.ps1
python .\tests\test-web-ui.py
git --no-pager diff --check
```

These commands do not flash hardware. Keep current pairing backups before
any update. The version-2 name-bearing input registry can read older records,
but older v1-only firmware cannot read it: do not downgrade firmware alone or
erase bonds to bypass a schema error.
