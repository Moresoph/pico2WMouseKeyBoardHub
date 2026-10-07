# Usable dual-board release checkpoints

## v0.6.11 — usable release (2026-10-07)

The user requested preserving the current version as a release after reporting
that frequent mouse disconnects had stopped. This is a usable release checkpoint,
not proof of the original timeout root cause or certification of every device,
sustained Mac workload, four-PC radio target or exception path.

| Board | Release/deployed firmware | Deployed image SHA256 |
| --- | --- | --- |
| Original dual-mode ESP32 | `0.6.11-wifi-default-off` | `74CCD804E2401589B399D8A26072529E3EC198E37DB18863663447664B055520` |
| Pico 2 W | `0.6.2-hid-dedup` (unchanged) | `2640C2197B08C6F8CEBA6F06A460000964C7F39D33E9F6E7134861D5A2D32F95` |

### Changes since the accepted `ffc7f38` clean-retry checkpoint

- Bluetooth controller modem sleep is disabled on ESP32. CPU power management
  remains off; Pico firmware and output policies are unchanged.
- Mouse fast-interval requests are activity-triggered, not made at HID readiness,
  authentication completion or in response to peer idle-parameter updates.
  Real movement, wheel steps and button changes can request 7.5ms, preserving
  negotiated latency and supervision timeout. Pending requests, ten-second
  spacing and a three-unsatisfied-request budget prevent repeated intervention.
  Input is forwarded before considering negotiation; no-op mouse reports and
  keyboard traffic cannot trigger requests.
- Every boot leaves the Wi-Fi hotspot/HTTP server off, even if previously on.
  ESP32 BOOT double-click toggles it on/off; long-press still opens enrollment
  but does not enable Wi-Fi. USB `web on` is rejected; status, off and local
  password configuration remain supported without implicitly enabling it.
- Existing paired identities/keys, five input-record capacity, seven saved
  PC identities, input cleanup/retry, routing and HID dedup are retained.

### Validation and limits

Release application/source files were compared against the exact source used
for the deployed image. Unflashed debug additions were backed up locally and
excluded: this release has the basic `status/history`, not the candidate `diag`
command. Production tests cover activity/pending/cooldown/failure/rollover,
HID/ACL cleanup ordering and releases, enrollment/deletion, boot-off and physical
enable controls. The existing 33-group output and dual HID-dedup regressions pass.
The release is built from repository source with modem sleep explicitly off;
generated `sdkconfig` values, not only defaults, must reflect this setting.

Prior app-only deployment verification passed and current ESP32 NVS remained
byte-identical before boot; Pico was neither flashed nor rebooted for these
updates. This release operation does not modify the boards or paired records.
Intermittent HCI `0x08` mouse timeouts were previously observed even with Wi-Fi
off and before application cleanup. The later user-reported improvement is not
attributed to any one configuration without controlled evidence.

The local annotated tag is `v0.6.11`; previous release tags are unchanged.
No branch/tag push or hosted GitHub release is created by this local checkpoint.
Firmware images, raw logs, credentials and NVS/flash backups remain excluded
from source control. Build paths/timestamps may change hashes, so the table
identifies the actual deployed images rather than promising reproducible hashes.

## 2026-10-04 accepted clean-retry checkpoint

At 16:08 the user reported no issues noticed so far with the deployed ESP32
`0.6.7-clean-retry` and requested committing it. Pico remains `0.6.2-hid-dedup`,
unchanged. This records initial usable acceptance, not every failure scenario,
sustained Mac movement or four-PC capacity certification.

ESP32 application SHA256:
`D8F1393DCFCD879C232FF88F71AC311DE1A4D0D64C0A63A1ECD4EA08A5D51869`.
Pico UF2 SHA256:
`2640C2197B08C6F8CEBA6F06A460000964C7F39D33E9F6E7134861D5A2D32F95`.

Only failed connection cleanup/retry and descriptor failure classification
changed from the `b401938` baseline. Original paired identities/keys are retained;
Pico, mouse radio parameters, output policies, scanning and NVS locking are
unchanged. See `README.md` and `HANDOFF-SOFTAP-WEB.md` for deployment details.
The broader rejected 0.6.6/0.6.3 candidate is backed up outside the repository,
not current source or board firmware. Existing tags are unchanged; no push.

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
not end-to-end latency or sustained Mac acceptance. Capacity remains five saved
input records and seven PC identities; physical enrollment and existing bonds
are retained.

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
