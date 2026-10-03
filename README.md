# Pico 2 W Bluetooth keyboard/mouse hub

**Post-release deletion fix (`0.6.4-delete-controls`):** physically authorized
deletion is no longer disabled merely because automatic discovery is running.
Confirming an input deletion stops discovery, waits up to six seconds for
completion, rechecks the window token and exact saved identity, then runs the
existing deletion path. A failed stop, expired authorization, cancellation or
changed identity preserves the record and reports that deletion was not executed.
An active pairing trial and storage/telemetry faults remain guarded.
The fix was built, tested and deployed on 2026-10-03 after USB was moved to
another port (COM16). Application SHA256
`07EEB8104E9A0A50D95BF36EED9395770CF6C0775686B287D2A627CFAA908312`;
flash verification and byte-identical NVS preservation before reboot passed.
Runtime confirmed `0.6.4-delete-controls`, no storage fault, fresh UART/PC
telemetry, encrypted FILCO input and the newly saved/active `M585/M590` at
7.5 ms with real reports. M720 remains saved and inactive. The initial serial
re-enumeration after flashing was transiently unavailable; a later COM16 status
read succeeded. This fix is recorded as a separate commit after the unchanged
v0.6.3 release tag.
Existing user records were not destructively deleted as a validation step.

**Release checkpoint: `v0.6.3` (2026-10-03).** See
[RELEASE-NOTES.md](RELEASE-NOTES.md) for firmware identities, deployed image
hashes and acceptance limits. ESP32 is deployed as `0.6.3-auto-input`; Pico
source builds as `0.6.1-fault-lamp`, while the attached Pico remains on
`0.6.0-slots-buttons`. Automatic new-mouse physical acceptance is still pending.

## Button and saved-slot controls (0.6.0)

### Automatic input enrollment and readable controls (ESP32 0.6.3)

Long-press ESP32 BOOT for at least three seconds and release: the physically
authorized window now automatically scans BLE followed by Classic. A single
new HID mouse or keyboard with an unambiguous advertised role is automatically
connected, encrypted, checked against its actual HID descriptor, saved and made
active. Previously saved identities and hub names (including the Pico itself)
are excluded. Multiple fresh candidates of the same role stop automatic selection
and request an explicit page choice; generic HID with no keyboard/mouse role
also requires manual choice. No RSSI/model-name guessing or automatic
confirmation of a security code is used.

The existing old-target cleanup, provisional pairing and failure rollback
remain in force. A uniquely identified keyboard and mouse can be enrolled
sequentially within the same authorization. A completed admission closes the
window when no other new candidate remains. No discovered device is paired
when the window is closed. The USB console's explicit `enroll [auto|manual|cancel]`
uses the same paths for operator-driven diagnostics; HTTP cannot open its own
authorization. To delete records without auto enrollment, long-press to authorize
and then use **Stop automatic enrollment / Manual management** on the page.

Device names are learned from advertisements or the connected HID device and
persisted in a CRC-checked version-2 input registry, alongside the five identities,
active roles and FIFO order. Version-1 (68-byte) records migrate without dropping
addresses, age or keys. Names remain visible when the device is offline or after
reboot. Devices that have not supplied a name are explicitly labeled
“name not yet obtained”; operators can set a name in authorized manual management.
Names are length-bounded, serialized/escaped as text and never interpreted as HTML.
Older v1-only firmware cannot load the upgraded name-bearing registry; do not
downgrade firmware alone after migration or erase bonds to bypass a schema error.

The old grey disabled buttons could appear blank/unclear. Delete buttons now
retain high-contrast text for current inputs, saved records and all PC cards.
Unauthorized, busy, unconfigured and stale states show their reason alongside
the disabled button. Deletion still requires explicit confirmation, physical
authorization, matching identity and (for PCs) a Pico acknowledgement.
Native checks execute the production management/candidate/trial functions,
including both unique mouse/keyboard auto admission, ambiguity/known-hub guards,
old-record retention and name persistence. Browser checks cover eleven states,
including readable disabled deletion controls, saved/missing names and auto mode.
Live operator `enroll auto` (the same admission path as a BOOT long press) confirmed
automatic BLE/Classic scan cycling, waiting for the old BLE worker, and exclusion
of the Pico broadcast. M585/M590 was no longer advertising in that bounded run,
so actual new physical-mouse pairing is not claimed; the test window was cancelled
and both old input records retained. Pico firmware was not changed for this task.
Subsequently, at 19:45, COM16 runtime confirmed M585/M590 saved as input record 3
and active, encrypted/HID-ready with 441 real reports; M720 record 2 remained
saved/inactive. After the 0.6.4 update it reconnected encrypted with 401 reports,
confirming physical new-mouse admission and persistent identity/name.
The exact button-versus-page trigger was not captured in that observation.
The final ESP32 application was identity-checked, flash-verified and deployed,
SHA256 `A275AE4557A7CC464A4BC548C7DF20F700F4F9251783A1ACD2D873D6A8BD371C`.
NVS was byte-identical before/after flashing, before reboot; the previous scan
run already migrated the input registry without losing active FILCO/M720 records.
The final startup confirmed SoftAP/HTTP on `PicoHub-9235`; a subsequent console
read again encountered a transient CH340 USB access error. The actual physical
button/new-mouse admission and final phone-page appearance still need user
acceptance. No saved input/PC was destructively deleted for validation.

The dual-board variant saves **five input records** and **seven PC records**.
These are storage limits, not a guarantee of 12 simultaneous radio links.
ESP32 keeps one active keyboard and one active mouse chosen from its five
records. Pico has seven logical PC identities and retains a four-peripheral
connection limit; simultaneous four-PC capacity remains a hardware acceptance
item. Selecting another ready PC works through BOOTSEL or Ctrl+Alt+Left/Right.

| Runtime gesture | ESP32 BOOT (GPIO0) | Pico BOOTSEL |
| --- | --- | --- |
| Short press/release | Normal-status acknowledgement; during management, cancel | Next ready PC; during PC pairing, cancel |
| Double click | Toggle Wi-Fi/HTTP | Same next-ready action, no destructive function |
| Hold at least 3 s, then release | Open 120 s authorization and auto-scan/enroll unique new inputs; start hotspot if off | Open 120 s keyboard-code PC pairing |

Long actions happen on release. Buttons held during startup must first be
released and do not invoke runtime actions. EN/RESET still resets. Holding
BOOT/BOOTSEL while powering up can enter the normal ROM download mode.
No runtime button gesture clears all records.

Normal status LEDs are OFF. Startup gives a short acknowledgement; reconnect
is one 100 ms flash every 2 s; pairing/management is 500 ms on/off; code entry
is 100 ms on/off; faults repeat 2 seconds steadily ON followed by 2 seconds of
100 ms ON/OFF flashing (a 4-second cycle starting steadily ON when fault
indication becomes active); successful pairing
acknowledges briefly. ESP32 reports active inputs and UART, Pico reports UART
and the currently selected PC. Unselected offline PCs are not faults.
The ESP32 red power LED is not software-controlled.

Connect to the existing protected hotspot and open `http://192.168.4.1/`.
Long-press ESP32 BOOT to start automatic enrollment. For manual administration,
choose **Stop automatic enrollment / Manual management**, then scan Classic
or BLE candidates, choose a specific named/addressed device as keyboard or mouse,
activate a stored input, or explicitly confirm deletion of an input/PC slot.
Unknown advertisements are never auto-selected by RSSI or scan order; only a
unique, new, explicitly classified keyboard/mouse is auto-admitted in the window.
Legacy FILCO PIN remains `123456` + Enter on the new keyboard when prompted;
SSP confirmation/passkeys have explicit page controls. PC pairing requires an
already-connected keyboard for the computer's code and never silently falls
back to Just Works. Incoming unknown pairings stay rejected outside explicit
target authorization.

When full, the oldest **first-admitted** record is evicted, not the
least-recently-used device. Reconnection/activation does not change age.
Input candidates are provisional until encrypted HID with the requested role
is confirmed; cancel, timeout, wrong HID role or pairing failure restores the
previous input record. Old records/bonds are not discarded before a successful
replacement is persisted. PC replacement follows authenticated encrypted
identity binding; a failed new pairing does not claim or evict a PC slot.
If the oldest evicted input was the other currently active role, that role
becomes unconfigured until another saved input is activated.

Deletion requires physical ESP32 authorization, a window token, explicit
address identity and same-origin JSON POST. PC deletion crosses a session/nonce
checked UART request and is only reported complete after Pico acknowledgement;
missing acknowledgement is reported as uncertain, never success.
An explicitly deleted active device is released/disconnected and its own
bond removed; no other device keys are erased.

Existing two-input NVS records and four-PC PHO1 storage are loaded/migrated
without losing identities, selection or bonds. Historical records did not
store admission timestamps: their initial FIFO order is deterministically
slot order. New admissions persist FIFO order. UART input payload stays 46 bytes;
seven-PC output status becomes 76 bytes, with separate authorization and
acknowledged management frames. Update both boards together.
The old single-board configuration retains its four slots and old button
behavior; the new gestures/capacity are dual-board-only.

### New mouse enrollment correction (ESP32 0.6.2)

An enrolled M720 is not a permanent or exclusive target. Any selected Classic
or BLE HID mouse can occupy the active mouse role after encrypted HID validation,
while up to five input records remain saved. ESP32 `0.6.2-input-enrollment`
corrects discovery requests rejected solely because the old device was still
in a reconnect/open call: the requested scan is queued, background offline
reconnects are held while management is open, and discovery starts once the
in-flight operation ends. The wait is bounded and explicitly reported; it does
not cancel an SDK-owned BLE open unsafely or erase the old bond.

Discovery retains named/HID candidates rather than filling the list with
anonymous unrelated broadcasts, and prioritizes HID candidates at capacity.
The page shows queued scanning, the exact new trial address, waiting/validation/
rollback phase and Chinese operation results. Selecting a new device after
waiting gets a fresh 120-second target-specific pairing budget. Passkey entry
is kept intact across status refreshes. Pressing BOOT only authorizes management;
it does not guess which nearby mouse to pair.

Native regression coverage uses the actual production management/discovery/
trial functions, with only SDK/NVS/queue boundaries mocked. It verifies adding
an unfamiliar mouse despite an existing saved mouse, not saving before encryption,
preserving keyboard/old mouse, switching back, failure/cancel/expiry/wrong-role/
write-error rollback, queued scanning and stale candidate/token rejection.
This is software-path validation, not a claim that every physical mouse works.
Rollback closes the management window so the restored old target can reconnect
immediately rather than staying held until authorization expires.

On 2026-10-03 the identified ESP32 received the final `0.6.2-input-enrollment`
application, SHA256
`E9A7DBD37C3863A0B807147BCC0EA9DD627DCD54B3B113227D05F5B1C3596AF5`.
Application verification passed and NVS was byte-identical before/after flashing,
before reboot. Pico was not reflashed and remains `0.6.0-slots-buttons`.
Live reads before and after the rollback correction confirmed both original
input records/active selections, no storage fault, UART and PC telemetry.
An intermediate console read encountered a CH340 access-denied error, but the
last read succeeded on COM15 and confirmed the deployed 0.6.2 identity.
A new physical mouse was not
available for the actual pairing test; do not treat native trial tests as proof
of that particular mouse's radio/HID compatibility. Reopen physical management
and use the phone page to scan/select the new mouse without deleting M720 first.

Fault-lamp revision `0.6.1-fault-lamp` implements the 2-second solid / 2-second
fast-flash pattern on both boards, including fresh entry and timer wraparound.
Both images build and the regression checks pass. The standalone 0.6.1 images
were **not deployed**:
only Pico COM5 was detected at that check. The later ESP32 0.6.2 enrollment
deployment includes its new fault lamp; Pico still has its old 0.6.0 lamp.
The ESP32 build path now contains 0.6.2 and the Pico build path contains pending
0.6.1; do not confuse current build files with the original 0.6.0 deployments.
The previously deployed 0.6.0 binaries are retained locally as
`backups\esp32-0.6.0-slots-buttons-deployed-20261003.bin` and
`backups\pico-0.6.0-slots-buttons-deployed-20261003.uf2`.

```powershell
.\tools\build-esp32.ps1 -BuildDirectory build\esp32-slots-buttons
.\tools\build.ps1 -DualBluetooth -BuildDirectory build\dual-slots-buttons -Jobs 4
.\tests\test-dual-bt.ps1
.\tests\test-output.ps1
python .\tests\test-web-ui.py
```

Deployment on 2026-10-03: both boards received `0.6.0-slots-buttons`.
ESP32 application SHA256
`6A4E4D8D64C1E20A1EF9AF1ECC2C9A480E3F68A6AE16C99D897268668201E7B9`;
Pico UF2 SHA256
`D0259472D9C19493E7ED9C59127166D13AC281DE0860A81F9BD55CB8DE0F100F`.
ESP32 MAC was confirmed, the application was flash-verified and its 24,576-byte
NVS partition was identical before/after writing, before boot. Pico readback
matched 2,088 program blocks and all 893 non-program sectors were unchanged.
Pico runtime confirmed seven slots, retained PC1/PC2 identities, encrypted
HID-ready PC1 at 7.5 ms, mouse buffer OFF and fresh UART telemetry.

Native tests cover actual IDF Classic=0/BLE=1 migration, FIFO admission,
failed-trial copy preservation, CRC, active-input deletion, four-to-seven-PC
storage migration, failed writes, exact identity deletion, release gestures,
quiet LEDs and separate control frames. Browser tests cover seven-PC/five-input
rendering, expired/error states and physical-authorization controls. A first
deployment exposed an enum mismatch in input migration; it was corrected,
regression-tested and reflashed without deleting any saved target/bond.
The final ESP32 console read is blocked by recurring CH340 USB disappearance/
access errors. The host has only its active Internet Wi-Fi connection and no
permission to interrupt it, so final real-device web/button/new-pairing/deletion
acceptance remains pending; destructive deletion of existing user records was
not used as a test. Use a phone on the protected hotspot to finish acceptance.
The following 0.5.x observations describe historical baselines, not this new
revision's LED, slot-count or HTTP management behavior.

**Historical accepted setup:** ESP32 `0.5.1-bonded-reconnect` receives the FILCO
Classic keyboard and M720 BLE mouse, then forwards input over UART to Pico 2 W
`0.5.0-dual-bluetooth`, which provides BLE HID output to the computer. Keyboard
and mouse forwarding to PC1 is hardware-confirmed; four-PC switching acceptance
is still pending. See the **Dual Bluetooth** section for wiring and build commands.

The single-radio prototype below remains available as a separate configuration.

Experimental firmware for one Raspberry Pi Pico 2 W:

```text
Classic HID / BLE HOGP keyboard --+
                                +-- Pico 2 W -- BLE HID -- computer 1
Classic HID / BLE HOGP mouse ----+           +-- BLE HID -- computer 2
                                            +-- BLE HID -- computer 3
                                            +-- BLE HID -- computer 4
```

All four computer links are intended to stay connected. Selecting a computer
changes the destination of input reports; it does not disconnect the other
computers. Computer-side Bluetooth must support BLE HID.

**This is a feasibility prototype, not confirmation that CYW43439 can sustain
all six links.** Controller connection failures and disconnect reasons are
reported over USB serial. A successful build alone does not prove radio
capacity, computer compatibility, or switching latency.

## Single-radio reference baseline: 0.4.11 (2026-09-27)

Version **0.4.11** is the user-approved reference for further development and
rollback of the single-radio variant, replacing its 0.4.10 baseline. It includes the
Classic HID/SDP disconnect recovery described below. "Standard" means an
accepted development baseline, not a production or security-hardened release.

| Item | Recorded baseline |
| --- | --- |
| Firmware identity | `0.4.11-open-test-mouse-buffer` |
| Output backend | BLE HOGP; Classic output remains an opt-in experiment |
| Input links | Classic keyboard with explicit Level 0 forwarding consent; bonded BLE mouse |
| Performance configuration | Saved fast mode; mouse-active ON; mouse-buffer ON; pc-standby OFF |
| Selected computer | Saved PC1; PC2 remains enrolled but was offline |
| Observed operation | After reboot and device wake-up, both inputs and PC1 were HID-ready; keyboard/mouse reports and PC1 output counters advanced |

Build this source revision with:

```powershell
.\tools\build.ps1 -Jobs 8 -OpenClassicInputTest
```

The verified, deployed image is retained locally at
`backups\pico_bt_hub-0.4.11-classic-recovery.uf2`, SHA256
`E6D43C3E6CEF69C9E34A752D50E0664BC9DE97A02171D430ED15EB5C5A68CDAD`.
Use that exact backup for binary-identical rollback; rebuild timestamps can
change the hash. The previous 0.4.10 backup is also retained. Binary backups,
device logs and pairing keys are not committed to Git.

Existing identities, bonds, forwarding consent and latency policy were
preserved. The Classic Level 0 keyboard link is still **unencrypted and
unauthenticated**. Four-PC/six-link capacity, long-term freedom from intermittent
lag, and the 15 ms standby-PC switching behavior are not established by this
baseline's three-link recovery observation.

## Dual Bluetooth: ESP32 input + Pico 2 W output (usable checkpoint)

This is an opt-in, two-firmware alternative to the single-radio and USB Host
experiments. Keyboard/mouse forwarding to PC1 is hardware-accepted; simultaneous
four-PC connections and physical switching acceptance are not yet established.

### Reboot reconnection recovery (ESP32 0.5.3)

The BLE HID open call can block for tens of seconds while a mouse is asleep.
It now runs in a dedicated worker, separate from Classic HID submissions and
the 100 ms connection/LED/deadline monitor. Setup is serialized within each
transport, not across both radios' input protocols; a missing mouse cannot hold
the saved Classic keyboard's reconnect scheduling or incoming HID completion.
Successful OPEN callbacks alone own live HID pointers. The value returned by
the SDK open call is never retained, because its callback may already have
closed/freed the device before the call returns.

Normal connection/HID/encryption deadlines now trigger release, link cleanup
and automatic retry with capped 1/2/4/8/15-second backoff, rather than permanently
pausing the input. A still-running BLE worker is never duplicated. Existing
Classic ACLs without HID get a five-second profile grace; stalled ACL cleanup
is posted to the Bluetooth thread, checks the exact live connection handle and
does not remove bonds. Unknown targets, missing pairing consent, actual
authentication/key failures, unsupported descriptors and malformed input retain
their safety gates. No ordinary retry opens a pairing window or accepts a PIN.

Encryption completions carry a unique request token, preventing a late result
from an earlier attempt from authorizing a replacement attempt. An encryption
OFF notification during initial setup does not destroy the HID descriptor;
loss of established encryption still immediately releases input and cleans up.
`status` reports `worker`, `recovering`, `profile-request`, retry count and delay
in addition to actual ACL/HID/encryption status. Sleeping keyboards still need
normal key wake-up on their original paired channel; this is not re-pairing.
The ESP32 application component is size-optimized (`-Os`) to fit the unchanged
partition layout. Pico firmware and mouse/output timing parameters are unchanged.

The first live deployment observation confirmed that Classic retries continue
during a blocked BLE open, retry delay caps at 15 seconds, saved FILCO bond is
present and neither input is permanently paused. Both devices still timed out
without responding in that bounded run, before keyboard authentication; this
does not prove lost keys or successful physical reconnection. Physical wake-up
on the original paired channels is required for repeated reboot acceptance.
Final recovery image deployed on 2026-10-01: application SHA256
`34D305F6C0909AE1E7CFECE2D162AFE20359A21BCA3131CCCA2B62CF12BDFA43`,
size `0x175950` with 5,808 bytes remaining in the unchanged app partition.
The identified ESP32 was application-flashed and verified after a current NVS
backup; the Pico was not reflashed. The final bounded reboot observation again
showed independent retries, `bonded=1`, no timeout pause and no PIN/new pairing
window, but no response from either physical input. The user was unavailable
for device wake-up, so repeated encrypted keyboard reconnection is not yet
accepted. Pico PC1 remained encrypted/HID-ready at 7.5 ms, mouse buffer OFF,
with an online UART link and zero framing errors.

### Local SoftAP status website

The optional-management firmware revision `0.5.2-softap-status` starts an
ESP32-only Wi-Fi hotspot `PicoHub-XXXX` with a fixed management address
`http://192.168.4.1`. It does not join a router, require the router's password,
or provide Internet access/NAT. Connect the phone or management computer to
this hotspot and explicitly keep the connection when the OS reports no Internet.
The computers controlled over Pico Bluetooth do not need to join the hotspot.

The WPA2 hotspot password is generated independently on first boot and saved
in the ESP32 `hub_web` NVS namespace. It is not the keyboard pairing PIN, is
not hard-coded in source, and is never returned by HTTP. Run `web` on the ESP32
USB console to read the SSID/password. `web off` stops HTTP and the Wi-Fi radio
without disabling Bluetooth; `web on` restores them. These switches are RAM-only;
the hotspot starts on reboot. Maximum management clients: two.
`web password <new-password>` on the USB console saves and verifies an
8–63-character printable ASCII password in the local `hub_web` namespace,
then reapplies the protected hotspot configuration. An active hotspot briefly
disconnects clients and restarts; Bluetooth target/bond settings are not changed.
The selected password is device-local, never embedded in public source or HTTP.
Use a nontrivial password when nearby unauthorized access matters.
The USB password-command image was application-flashed and verified on
2026-10-03, SHA256
`54735CFEBC3A1E23D0D7FE47E52A4E6F76B8E8674E75BD3612341A759EDC790D`.
Existing NVS was byte-identical through that update. The first attempt to change
the local password was blocked by COM15 disappearing. After the user replugged
USB at 18:56, the password command and subsequent `web` status confirmed the
requested local password and hotspot ON. The actual device password is not
included in source or documentation.

The phone-friendly page uses only embedded HTML/CSS/JavaScript, fetching
read-only `/api/status` every two seconds without overlapping requests.
It reports the two selected inputs, saved bonds, encrypted/HID readiness, and
the Pico's four logical computer slots, active selection and mouse-buffer state.
It never displays actual keystrokes, mouse movement, or pairing keys. Unknown
input names fall back to the slot/address; stored PCs are labeled PC1 through PC4.
No pairing, selection, forgetting, or other state-changing HTTP endpoint exists.

Pico `0.5.2-dual-web-telemetry` sends a separate 46-byte output-status payload
once per second over the existing 460800-baud UART. Input reports and their
100 ms heartbeat/500 ms fail-safe are unchanged. Computer telemetry has separate
sequence numbers and uses bounded, non-blocking UART TX, with heartbeats scheduled
before new telemetry frames. Computer telemetry also has
sequence/session checks; duplicates cannot refresh its three-second freshness
deadline, and telemetry cannot keep the input watchdog alive. Missing/old Pico
telemetry, UART loss, HTTP errors and pairing-storage faults are explicitly
shown as unknown, never as cached online success. Update both boards: an older
Pico can still forward input but cannot provide computer status.

Wi-Fi/Classic/BLE share the ESP32 radio. Low-frequency status access, Wi-Fi/HTTP
on core 1, bounded buffers and snapshot-only HTTP handlers reduce contention,
but do not guarantee unchanged physical input latency. Use `web off` to compare
against the working Bluetooth-only setup. No DNS/captive-portal auto-launch is
implemented; type the fixed IP address into the browser.
Wi-Fi IRAM/RX-IRAM throughput optimizations are disabled to fit Classic/BLE plus
Wi-Fi into this original ESP32's IRAM; Bluetooth controller configuration is
retained. This is a small status server, not a high-throughput network service.
The hotspot uses 20 MHz channel width. ESP-IDF marks SoftAP-with-connected-clients
plus Classic/BLE as supported but performance-unstable (C1), so actual coexistence
and reconnection still need physical checks. After a five-second grace, a bonded
incoming Classic ACL without HID is completed by host HID setup on the existing
ACL when the serialized connection worker is available; waiting behind another input must not pause it as an
authentication timeout. Encryption is taken from GAP events, never inferred
from the existence of a saved bond.
An incoming HID can open without producing the expected GAP encryption event.
For a mapped Classic target without confirmed encryption, a version-locked
IDF bridge requests BR/EDR encryption on the Bluetooth stack thread. Completion
reads the live ACL and actual stack encryption flag; a success status or saved
bond alone is insufficient. Results are queued without blocking the stack and
matched to the ACL generation so a delayed callback cannot authorize a new
connection. Missing keys still encounter the existing explicit pairing gate;
this path never opens a pairing window or permits plaintext forwarding.

Build and run the related checks without replacing the previously saved images:

```powershell
.\tools\build-esp32.ps1 -BuildDirectory build\esp32-dual-web
.\tools\build.ps1 -DualBluetooth -BuildDirectory build\dual-web -Jobs 4
.\tests\test-dual-bt.ps1
.\tests\test-output.ps1
python .\tests\test-web-ui.py
```

Deployment observation on 2026-10-01: the final ESP32 application SHA256 is
`B43FB16E5B6F2A32E00EDADF5233852150E1DCA8A467363AF266919690662B3F`;
Pico UF2 SHA256 is
`5F75CEBD3477C41C11AE763673732661F6B8061468B08A5825F266F86FC7680D`.
ESP32 updates were application-only after NVS backups. Pico full-flash readback
matched all 2,060 program blocks and all 895 non-program sectors were unchanged,
including computer pairing storage. Real HTTP/browser checks verified four
computer slots: PC1 encrypted/HID-ready at 7.5 ms, PC2 saved/offline, PC3/4 empty,
fresh UART telemetry and mouse buffer OFF. HTTP POST was rejected; the API did
not contain hotspot passwords, link keys, or actual input data.

The last live Bluetooth check after the encryption update confirmed FILCO
`HID=1 encrypted=1 bonded=1` with 72 decoded reports, and M720
`HID=1 encrypted=1` with 720 reports at actual
`interval=7.500ms latency=44 timeout=2160ms`. Both slots were unpaused with no
pending connection or plaintext exception. At 11:42 the user confirmed keyboard
connection had recovered. The normal GAP encryption event was observed in this
successful session; the extra encryption-request branch was covered by native
tests, not independently forced in that session. Earlier hotspot/browser and
off/on checks passed, but the management PC was disconnected from the hotspot at
the final read-only check. Latest combined keyboard-plus-active-web-client
smoothness and long-term C1 coexistence are not certified; no additional flashing
or Bluetooth parameter changes were performed after this user confirmation.
The current image fits the existing app partition with only 4,640 bytes spare;
future additions must be size-checked rather than silently changing partitions.

```text
Bluetooth keyboard + mouse -> ESP32 (Classic HID / BLE HOGP host)
                              -> wired UART -> Pico 2 W -> BLE HID computers 1..4
```

Use an **original dual-mode ESP32**, not an ESP32-S3/C3. The locally identified
board is ESP32-D0WDQ6 revision 1.1, 40 MHz crystal, 4 MB flash. ESP32 Wi-Fi is not
started. Pico no longer scans/connects Bluetooth inputs in this build; it keeps
the existing BLE computer identities, bonds, routing, passkey capture and
Ctrl+Alt+Right/Left behavior. Four simultaneous computer links and latency under
two-radio load still require hardware observation. Two antennas need clearance;
two radios do not eliminate 2.4 GHz interference.

### Wiring and power

| Pico 2 W | ESP32 | Purpose |
| --- | --- | --- |
| GP4, physical pin 6 (UART1 TX) | D14 / GPIO14 (UART1 RX) | Pico heartbeat/challenge |
| GP5, physical pin 7 (UART1 RX) | D13 / GPIO13 (UART1 TX) | Keyboard/mouse states |
| GND, e.g. physical pin 8 | GND | Common signal ground |

The firmware uses the board's active-high blue LED on GPIO2 as its status LED.
The red power LED remains a power indicator and cannot be controlled by the
firmware. No external LED is required.

UART is **460800 baud, 8N1, 3.3V logic, no flow control**. The current wiring uses
the ESP32 board's D13/D14 labels as GPIO13/GPIO14, not physical pin numbers.
UART1 is routed to these pins by firmware; the board's UART0 TX/RX labels
remain for USB programming/console. Both boards may be powered by their own USB
connections to the same computer; **do not join their 5V/3.3V supply pins** and
do not power ESP32 from Pico's 3V3 output. Disconnect the old external VBUS feed
and USB Host/Hub wiring before powering Pico from the PC.

GPIO13 and GPIO14 are not ESP32 strapping pins, so the connected Pico UART no
longer interferes with entering the ROM serial downloader. GPIO2 remains a
strapping pin but is only connected to the board's built-in LED circuit; the
firmware configures it as an output after boot.

Pico's micro-USB becomes a USB **device/CDC console**, not USB Host. ESP32's
onboard USB-to-UART remains its 115200-baud programming/console port. Merely
plugging both boards into the PC does **not** connect their UARTs. The above
three signal wires are still required. Old COM numbers are not reliable:
COM12, previously a Pico adapter, was identified as the ESP32 in this setup.

### Builds and rollback

```powershell
.\tools\build.ps1 -DualBluetooth -Jobs 4
.\tools\build-esp32.ps1
.\tests\test-dual-bt.ps1
```

Outputs are isolated under `build\dual-bluetooth` (Pico UF2) and
`build\esp32-dual-bluetooth` (ESP32 bootloader, partition table and application).
Neither build script flashes hardware. The ESP32 build expects ESP-IDF
**v5.5.1 / fcae32885b0296b32044cb99ecbdc50d98dddb83** in `.deps\esp-idf` and tools
in `.tools\esp-idf`. After cloning that tag and initializing its submodules:

```powershell
$env:IDF_TOOLS_PATH = "$PWD\.tools\esp-idf"
python .\.deps\esp-idf\tools\idf_tools.py install --targets esp32
python .\.deps\esp-idf\tools\idf_tools.py install-python-env
```

ESP32 uses the existing pinned SDK's BTstack **HID parser only**, not its radio
stack; Bluedroid owns the ESP32 radio. The parser's BlueKitchen copyright,
non-commercial license and redistribution conditions remain in
`.deps\pico-sdk\lib\btstack\src\btstack_hid_parser.c` and `btstack_util.c`;
commercial distribution needs the appropriate license. No vendor source is
edited. A hash-checked generated IDF Classic-HID overlay requests encryption
without requiring MITM protection for normal Classic input. In the pinned IDF
security manager, requesting encryption also sets authentication, but specifying
authentication at registration time would additionally require MITM under SSP.
An explicitly authorized `open-connect` target alone uses unencrypted Level 0,
preserving the earlier keyboard experiment. This Classic input policy does not
change BLE inputs or Pico's encrypted BLE computer outputs.

The original ESP32 flash was backed up before writing any firmware to
`backups\esp32-10521c1d9234-before-dual-bt-20260929.bin` (4 MB), SHA256
`CD3517473707D59C3D915B52A3E16213CADCE80D9FFB2B4371958FB7ACB51A08`.
It previously reported ROM `flash read err, 1000`; that alone does not establish
hardware damage. This backup may contain private data and stays ignored.
The 0.4.11 and 0.4.21 Pico backups are retained. Enter Pico BOOTSEL physically
with its old external 5V/Host wiring disconnected; identify the exact board
before writing. Never load/execute old USB Host firmware while still connected
to a PC USB data port.

### Enrollment and use

On the **ESP32** console, use `help`, `status`, and `scan classic` / `scan ble`.
Scanning only lists candidates and never pairs automatically. Assign each
physical input device to one of two slots:

```text
connect 1 classic AA:BB:CC:DD:EE:FF
connect 2 ble public AA:BB:CC:DD:EE:FF
```

Use `random` instead of `public` when indicated by the BLE scan. Each explicit
connect opens a 120-second pairing window for that target only. A compatible
Classic legacy keyboard PIN request is answered automatically with the fixed
PIN **`123456`**. While the LED is flashing rapidly, type `123456` on that
Bluetooth keyboard and press Enter. The fixed PIN is never accepted outside the
explicit window or for an unselected address. A device that requires a
16-character legacy PIN is rejected rather than silently weakening or changing
the policy. SSP/BLE confirmation and passkey requests remain explicit: use
`confirm <slot> yes|no` or `passkey <slot> <six digits>` when requested. Normal
forwarding requires encryption.

Classic input is connectable but non-discoverable so an enrolled keyboard can
initiate its own reconnection. Outside an explicit pairing window, a normal
Classic HID open must match a selected, unpaused target with a saved bond;
unselected/paused HID opens are closed and unknown PIN/SSP requests are rejected.
An explicitly saved `open-connect` exception is unchanged and is not enabled
by this reconnect policy. Incoming ACL/HID setup is tracked as a pending
connection so background reconnect does not open the same target concurrently.
`status` reports actual GAP connectability/discoverability, Classic bond
presence, and incoming-connection counters without exposing link keys.
If `bonded=0`, selecting an address alone is not proof of successful pairing;
use an explicit pairing window rather than accepting a new PIN automatically
during ordinary reconnection.

The built-in blue GPIO2 status LED uses these patterns:

| Pattern | Meaning |
| --- | --- |
| Off | No input target is configured |
| One 0.1 s flash every 2 s | A saved input is reconnecting |
| 0.5 s on / 0.5 s off | Explicit pairing window is active |
| 0.1 s on / 0.1 s off | Type fixed PIN `123456` on the keyboard, then press Enter |
| Steady on | Every configured input is connected and ready to forward |
| Two short flashes every 2 s | At least one configured input is paused after a fault |

Pairing and PIN-entry indications take priority over the fault/ready patterns
when two input slots are in different states.

For the previously used Classic keyboard that cannot negotiate an encrypted
HID link, the explicit alternative is:

```text
open-connect 1 AA:BB:CC:DD:EE:FF
```

This **persists consent to unencrypted, unauthenticated Classic input** for that
one address. The address can be spoofed; it is not a cryptographic identity.
Consent is not imported from Pico and is never enabled for BLE or PC output.
Use `disconnect 1`, wait for CLOSE, then normal `connect` to remove that
exception, or `forget 1` to remove its target/bond. Targets reconnect after
restart and ordinary disconnects; authentication failures/unsupported HID
pause the affected slot until explicit action. Reconnection to BLE devices
that rotate private addresses is not established; re-scan/reselect may be
needed. Keyboard HID LED output (Caps Lock, Num Lock, and similar), media/vendor keys
and arbitrary gamepads are not implemented.

On the **Pico USB CDC** console, use the existing `status`, `pair keyboard`
(once a keyboard is ready), `pair justworks` or `pair on`, `select 1..4`, and
`release`. Existing Pico input enroll/reset/scan commands are disabled in this
variant; manage input peers on ESP32 instead. Do not re-pair or erase existing
computer bonds merely to change the input backend. The dual-Bluetooth build
defaults the Pico mouse motion-credit buffer to OFF: moving input traffic to the
ESP32 removes its original shared-controller motivation, and the buffer can add
up to 7.5 ms of intentional motion delay. `mouse-buffer on` remains available
for output-credit experiments. The single-radio baseline keeps its existing ON
default.

For encrypted, mapped BLE mice, ESP32 requests a 7.5 ms connection interval
while preserving the currently negotiated peripheral latency and supervision
timeout. If the peer subsequently changes the interval, the request may be
repeated, bounded to three submissions per connection to avoid an update loop.
Rejected updates and exhausted retries are logged; `status` shows the actual
interval, not just the requested target. Classic inputs and BLE keyboards
without mouse reports are unaffected. A 7.5 ms input interval and 7.5 ms PC
output interval do not imply a fixed 15 ms end-to-end motion latency.

Before this input-interval change, the M720's live parameters were 11.25 ms,
peripheral latency 44, and supervision timeout 2160 ms. On 2026-10-01, the
7.5 ms request firmware was flashed and verified on the identified ESP32,
application SHA256
`2AA9FC3BD754B99983A462B279BF1AA76AF49FB675219BCEFBB5F14167B66FD0`.
The application-only update left the NVS partition untouched; a current NVS
backup is retained locally. Runtime confirmed the new 7.5 ms target and the
Pico's default-OFF buffer, but M720 reconnects timed out during the subsequent
bounded observation. The actual post-update mouse interval and motion latency
were not verified at that point. A subsequent live check confirmed encrypted
HID mouse input with `interval=7.500ms latency=44 timeout=2160ms`,
`update-pending=0 attempts=1`, and 2,066 real input reports. This confirms the
negotiated interval, not absolute physical-motion-to-display latency.

On 2026-10-01, ESP32 `0.5.1-bonded-reconnect` was deployed with application
SHA256 `E1C0D6E5D0C4658B55FB10F499A7E21C4DC802B6267F49F64F898235C39B1B75`.
The identified board received an application-only, verified update after a
local NVS backup; input target addresses and the FILCO bond survived reboot.
Live status confirmed `connectable=1 discoverable=0`, FILCO `bonded=1`,
`allow-unencrypted=0`, an online UART link, and the unchanged Pico default-OFF
mouse buffer and encrypted PC1 output at 7.5 ms. During the initial bounded observation,
FILCO still failed paging (`0x04`) without reaching authentication or incoming
HID, and M720 reconnect attempts timed out (`0x85`).

At 09:14 the user confirmed that both inputs were connected and usable, with
responsive typing and no keyboard or mouse stutter. A subsequent live check
confirmed FILCO `HID=1 encrypted=1 bonded=1 incoming=1 incoming-connections=1`
with 585 keyboard reports, and M720 `HID=1 encrypted=1` with 2,932 mouse reports
and actual `interval=7.500ms latency=44 timeout=2160ms`. Both slots were unpaused,
with no pending connection, no invalid reports, and no unencrypted exception.
Pico confirmed keyboard/mouse routing, an online UART link without framing
errors, mouse buffer OFF, and encrypted HID-ready PC1 at 7.5 ms.
This is the usable ESP32 + Pico 2 W dual-Bluetooth checkpoint; it does not claim
an absolute physical-motion latency measurement or completed four-PC switching
acceptance. Normal bonded reconnect must not be replaced with automatic PIN
acceptance or re-pairing.

On 2026-10-01, this default-OFF Pico image was deployed to the identified board
`F1812912A883CCE2`, UF2 SHA256
`58F523A32916205B6911A17C82A15F33EE8C7EA5F2B689F2E27E05B1EBB11431`.
Full-flash readback matched all 2,056 application payload blocks and confirmed
all 895 non-program sectors, including computer pairing storage, were unchanged.
After reboot, `status` reported `mouse-buffer enabled=0`, `boot default OFF`,
zero motion holds/deadline sends, an online UART link, and an encrypted/HID-ready
PC1 connection using its existing bond. Both ESP32 inputs were selected but
offline/reconnecting at this check; this is not a new keyboard/mouse latency
measurement. Disabling the buffer removes the deliberate credit-reservation
hold, not normal BLE scheduling or coalescing of already queued motion reports.

### UART failure behavior and acceptance

Protocol v1 uses `0x7e` delimiters, `0x7d` escaping, CRC16-CCITT-FALSE, a fresh
Pico challenge, ESP32 session ID, 16-bit sequence and full state of both input
slots. Mouse deltas accompany individual reports and are **not** repeated by
heartbeats. Corrupt frames, sequence gaps, UART overflow, changed ESP32 session
or 500 ms without a fresh frame release inputs and require a new handshake.
Duplicate frames cannot replay movement or keep a dead link alive. Reconnection
requires neutral physical keys/buttons before forwarding. A bounded 64-event
ESP32 queue preserves normal key/button order; overflow is an explicit fault
with a new session, not silently overwritten key-up events. This is fail-safe
recovery, **not a lossless retransmission protocol**.

The descriptor parser supports standard keyboard arrays/bitmaps and signed
relative mouse X/Y/wheel with report IDs, up to 3 maps / 16 reports per device.
Unsupported/oversized/malformed layouts pause the device rather than guessing
byte offsets. Slot state is not declared ready from a synthetic neutral report:
real input must be observed, and reports are not printed as typed-key payloads.

End-to-end acceptance still requires two actual input devices, nonzero mouse X/Y,
real keyboard reports reaching the selected PC, Ctrl+Alt+Right switching, and
release after pulling a UART wire or resetting ESP32. The deployment/heartbeat
observations below do not establish those input/output behaviors.

Bring-up observation before the D2/D15 remap: the identified ESP32 on COM12 was
flashed with the 0.5.0 candidate using TX=25/RX=26, with all written regions
verified and the original full-flash backup retained. Its `status`/`help`
console responded and BLE scanning received
advertisements. Classic inquiry was accepted but no discoverable Classic device
was observed; this is not Classic keyboard compatibility evidence. At that time
both input slots were unselected and `Pico-link=0 packets=0`: Pico deployment was
still blocked on BOOTSEL access.
The subsequent TX=15/RX=2 firmware was flashed on COM12 after rechecking the
ESP32 MAC, with all three written regions verified and NVS left untouched.
Application SHA256:
`F988F176CB0D9E0ACA9281636474C067E3E6D534ABE4EA93164CBC7041D85B82`.
After reset, two `status` requests confirmed `UART1 TX=15 RX=2 baud=460800`.
Before Pico deployment, both input slots were unselected and `Pico-link=0 packets=0`.
Local evidence:
`build\dual-bt-esp32-d2-d15-flash.log` and
`build\dual-bt-esp32-d2-d15-console.log`.

The subsequent Pico deployment succeeded on 2026-09-29 after the known board
`F1812912A883CCE2` reappeared in BOOTSEL. A fresh, verified 4 MB backup is retained
at `backups\pico-f1812912a883cce2-before-dual-bt-20260929-2356.bin`, SHA256
`74AD70C33CAD6EB9A89C2CFC296BB28DA833333CBCF765E2830C96D811E1CBFE`.
The flashed `0.5.0-dual-bluetooth` UF2 has SHA256
`33A7FBFB2DBDD75F8A531EAACE50CF4B94F55A992FF731BAFBABA6EE251B2F93`.
Before execution, full-flash readback confirmed all 2,056 application payload
blocks and that every sector outside the firmware, including Bluetooth TLV
storage, was unchanged.

After boot, Pico USB CDC appeared as COM5. Both ends reported the UART link
online: Pico accepted packets increased from 181 to 232, while ESP32 sent packets
increased from 181 to 231. Pico's reset count stayed at its startup value of 1,
with zero framing errors; ESP32 reported zero queue overflows. PC1 reconnected
using its stored bond and became encrypted/HID-ready with both notifications
enabled; PC2 remained reserved but offline. No pairing records were cleared.
At that point both ESP32 input slots were unselected, so real keyboard/mouse
forwarding and switching were still unverified. Evidence stays ignored under `build`:
`dual-bt-pico-flash-20260929-2356.json`,
`dual-bt-pico-console-20260929-2356.log`, and
`dual-bt-esp32-link-20260929-2356.log`.

The first Classic keyboard enrollment on 2026-09-30 exposed a command-validation
bug: `ESP_HID_TRANSPORT_BT` is zero, not an invalid transport. The parser now uses
`ESP_HID_TRANSPORT_MAX` as its invalid sentinel. The corrected ESP32 application
was built and flashed with verification, SHA256
`9190CC773D8D6A15D7A5FF7B8E8428C395BAE2639EA67FD06DE72AC192B13BF3`.
The known FILCO was discovered, normal encrypted `connect 1 classic` was accepted,
and ACL/SDP progressed to a legacy PIN request. Pairing did not complete: the
latest slot status was selected but paused, with no encryption, HID readiness or
input reports. No unencrypted-input exception was enabled. UART communication
recovered after the firmware update; mouse enrollment and input forwarding
remain outstanding.
The explicit 120-second pairing window now also bounds the connection timeout
while pairing is still allowed; ordinary reconnects retain their 40-second
timeout. This fix was built and flashed to the identified ESP32 with all three
written regions verified (application SHA256
`1F0E5B5D44FBAA0C1BDC522BCFDF839FC835BD73914597C42153FA0F57CA00AB`).
Pico was temporarily put in BOOTSEL during the ESP32 reset to avoid driving its
GPIO2 strap pin, then rebooted; both radios recovered and the UART link came
back online. A secure retry reached the keyboard's PIN request, but no physical
PIN entry was confirmed and the input slot later paused without encryption or
HID reports. The FILCO remains discoverable. Its encrypted pairing and physical
key delivery are still unverified; do not silently substitute `open-connect`.

On 2026-09-30, the Classic input policy was narrowed to encryption without an
explicit MITM requirement, matching BTstack's Level 2 intent. Pinned ESP-IDF
5.5.1 otherwise promotes `BTA_SEC_AUTHENTICATE | BTA_SEC_ENCRYPT` to a MITM
requirement during HID service registration. Both HID host registration and
ordinary target opens now request only `BTA_SEC_ENCRYPT`; Bluedroid adds the
authentication prerequisite for encryption *after* deciding whether MITM is
required. The separately authorized `open-connect` exception remains
`BTA_SEC_NONE`. Generated source and its compilation were checked before
flashing application SHA256
`B78460D1AE7C1BC84F2E89A350224DAD6A02E0C1AC2144E761F8E0F373BDD0A2`
to the identified ESP32. After reboot, ESP32 reported the new policy and both
boards showed UART online; PC1 reconnected to Pico using its saved encrypted
bond. FILCO was not in pairing mode during automatic retries, so encryption
and actual key delivery with this policy still require physical pairing.
The saved FILCO target remains paused for a deliberate retry; no unencrypted
consent was enabled. Legacy pairing may still ask for a PIN even though MITM
is not required. After the successful flash and initial status checks, all
local serial ports disappeared from the host, so no later live-device check
was possible; the reason for the USB disappearance is unknown.

## USB Host / 2.4 GHz mouse receiver experiment: 0.4.21

This separate experiment's acceptance target is **real mouse movement received from a USB
2.4 GHz receiver**, not storage access or external Bluetooth. USB HID interfaces
with supported signed relative X/Y layouts are polled in report protocol;
8-, packed 12-, 16- and up to 32-bit axes and report IDs are decoded from the
descriptor, not guessed from byte positions. `usb mouse` shows report/movement
counts, last deltas/buttons and accumulated movement. A first nonzero X/Y
report prints `FIRST MOTION`; later progress is rate-limited. Data is captured
only and **not forwarded to PCs**. Keyboard/vendor-only and unsupported layouts
are explicitly ignored. MSC is disabled: no flash-drive files are read or written.

This is also groundwork for a separate USB Bluetooth controller for Classic
inputs. **It does not yet move the keyboard to a Bluetooth dongle.** The onboard
Bluetooth stack, input retry/security policy, PC output and stored bonds remain
unchanged. No second BTstack instance or external Bluetooth pairing is started.
The current standard/rollback baseline remains **0.4.11**, not this experiment.

```powershell
.\tools\build.ps1 -Jobs 8 -OpenClassicInputTest -UsbHostProbe
```

The isolated output is `build\usb-host-probe\pico_bt_hub.uf2`, identifying as
`0.4.21-open-test-usb-host-probe`. This option requires BLE output. Without
`-UsbHostProbe`, USB CDC and the existing build configuration are retained.

In this experiment the native USB controller is **Host only**, not a USB serial
device. The console uses UART0 at **115200 baud, 8N1, no flow control**:
Pico GP0 (physical pin 1, TX) to the adapter RXD; GP1 (pin 2, RX) to TXD; and a
common GND. Use verified 3.3V TTL signals, not RS-232 or 5V TX signals. Leave the
adapter's 5V/3.3V supply pins disconnected. Power the serial adapter from the PC.

The Pico USB data port connects to the hub's upstream port; the mouse receiver
goes in a downstream port. Supply Pico and USB Host VBUS correctly: supplying
VSYS alone does not energize the USB connector's VBUS, and a powered hub must
not be assumed to backfeed its upstream port. Do not join independent 5V
supplies or connect the running Pico Host port to a PC USB host. Verify the
actual hub/cable power arrangement before powering this setup.

For the current fixed Host wiring, the same protected 5V source powers the hub
and Pico VBUS (physical pin 40), with GND on pin 38. The former external VSYS
(pin 39) connection is removed; do not bridge pins 39 and 40. This arrangement
must be disconnected before attaching Pico USB to a PC.

For flashing, disconnect the hub and external Pico supply, enter BOOTSEL and
connect Pico directly to the PC. Load with verification **without automatic
execution** (`picotool load -v`, not `-x`). Unplug from the PC before powering
the Host wiring again. UART `bootsel` remains available once this image is
running, but its BOOTSEL state does not survive loss of power: after removing
external power, hold the physical BOOTSEL button while plugging into the PC.
Firmware loading must not erase pairing/configuration storage.

Version 0.4.12 introduced the probe. Version 0.4.13 adds read-only physical VBUS
presence from CYW43 GPIO2, root-port data connection state, USB frame number,
task-poll count and queued-event status. Physical VBUS presence is a digital
threshold, not a voltmeter reading or proof of adequate supply under load.
Read errors explicitly report unknown voltage. The USB controller's forced
VBUS-detect override is not used as evidence of actual power.

Version 0.4.14 adds `usb trace`: the last 8 KiB of TinyUSB enumeration/control
transfer diagnostics are kept in RAM, including logs produced in USB interrupts.
An independent snapshot replays at most 64 bytes per USB task poll while live
capture continues. Overwritten bytes, truncated messages and formatting errors
are reported explicitly; no trace is written to flash. Trace output may
interleave with ordinary console messages. No external HCI transfers or Bluetooth key data are captured. USB HID payloads
are not dumped by the level-2 TinyUSB trace; mouse counters are printed separately.
`usb status` also shows a read-only EP0/register snapshot and a saturating receive
timeout count. Timeout messages are rate-limited to powers of two.

The 0.4.14 hardware trace identified repeated receive timeouts on the initial
address-zero SETUP at Hub port 3, with no failure delivered to enumeration.
Version 0.4.15 extends the hash-pinned HCD overlay to stop that retry loop and
return a single failed transfer to TinyUSB's existing bounded enumeration retry
logic. Late completion indications for the failed EP0 are discarded without
discarding other endpoints' buffer completions. It also selects SOF for a
full-speed root bus or keep-alive for low speed, never both; low-speed children
behind a full-speed hub continue to use preambles. These changes follow the
upstream native Host implementation's
[bus timing](https://github.com/hathach/tinyusb/commit/084ef43be654) and
[timeout handling](https://github.com/hathach/tinyusb/blob/530c6708dd6f/src/portable/raspberrypi/rp2040/hcd_rp2040.c)
approach without importing its unrelated endpoint scheduler rewrite.
The vendored SDK and normal USB CDC build are unchanged. Hardware enumeration
success must be established separately; an error recovery patch is not proof
that the attached peripheral responds.

Version 0.4.16 fills the native driver's previously empty root-port reset hook:
the controller emits the USB reset pulse before enumeration, with completion
state recorded in the RAM trace. An active control transfer is not silently
discarded to issue a reset. It also adds HID reception with failed transfers
delivered as errors rather than interpreting partial/stale data as motion.
These additions still require hardware confirmation of receiver enumeration
and nonzero movement reports before the receiver task is considered complete.

Hardware observation of 0.4.16 found a startup regression: with the Hub attached,
root bus reset repeatedly produced remove/attach events and restarted enumeration,
preventing the USB task from returning to the console/Bluetooth run loop.
Removing only the Hub data cable restored the console and exposed the repeated
reset/remove/attach sequence in the RAM trace. Version 0.4.17 masks only the
connection-change interrupt during the reset pulse, acknowledges the reset's
speed changes before restoring the previous interrupt mask, and leaves other
interrupts enabled. A duplicate reset cannot replace the saved mask; a rejected
reset cannot acknowledge unrelated events. The upper layer still checks physical
connection after its reset/debounce delay. Hardware observation confirmed that
0.4.17 boots normally with the Hub attached, restores the console and reconnects
PC1. Downstream initial SETUP requests still timed out; no HID interface mounted
and no real mouse movement was received.

Version 0.4.18 enables RP2350 `LINESTATE_TUNING.MULTI_HUB_FIX` immediately after
the USB controller reset, before enabling Host operation. The
[SDK register definition](https://github.com/raspberrypi/pico-sdk/blob/2.3.1/src/rp2350/hardware_regs/include/hardware/regs/usb.h)
documents this normally disabled bit as extending Host inter-packet/turnaround
timeouts to accommodate Hub delays. An atomic SET changes only this bit, leaving
the other line-state settings untouched. The overlay applies only to the native
Host experiment on RP2350; RP2040 and ordinary USB CDC builds are unchanged.
`usb status` reads the actual register and reports `Hub-turnaround-fix=1` when
enabled (normally `linestate-tuning=000000fc`), not merely a build-time flag.
Production-init tests check placement after reset, before Host enable, and
preservation of the other bits; a separate RP2040 compilation checks the guard.
This addresses a documented hardware timing option, but receiver enumeration
and real movement still require hardware verification.
Version 0.4.18 is preserved as
`backups\pico_bt_hub-0.4.18-usb-hub-timing.uf2` and was loaded with Flash
verification on 2026-09-27 to Pico serial `F1812912A883CCE2`. Post-flash metadata
confirmed the version and that the board remained in BOOTSEL; no automatic
execution was requested. After restoring Host wiring, 0.4.18 booted normally
and PC1 reconnected. Hardware readback confirmed
`linestate-tuning=000000fc Hub-turnaround-fix=1`, but the first address-zero
SETUP still failed on ports 1 and 4, with the timeout count stable at 16.
No HID interfaces mounted and no real mouse movement was received. Enabling
the documented timing bit alone does **not** resolve the observed failure;
reflashing this same image is not a further diagnostic step.

Version 0.4.19 adds an experimental read-only digital wire diagnostic because no
alternate Hub/direct-receiver adapter is available. A claimed DMA channel/timer
samples `USBPHY_DIRECT` before the first root and first downstream address-zero
8-byte device-descriptor SETUP. Each capture is limited to 2048 samples (8 KiB)
at a nominal `clk_sys / 4` rate; retries do not overwrite it. This reads the
upstream D+/D- pin states without setting PHY overrides, changing GPIO wiring,
or capturing routine HID reports. DMA allocation, timeout and bus errors are
reported explicitly, and only the claimed resources are cleaned up. The short
DMA activity can affect bus arbitration, so this is a diagnostic, not a passive
external analyzer or a claimed fix.

`usb wire` replays the two snapshots in paced UART lines. Decode a complete log
with Python 3 (standard library only):

```powershell
python -B .\tools\decode-usb-wire.py .\build\your-uart-capture.log
```

The decoder checks SYNC, NRZI/bit stuffing, EOP, PID, token CRC5 and data CRC16.
It distinguishes a decoded SETUP/DATA0/ACK sequence from an incomplete sample.
SOP detection also accepts one intermediate SE0/SE1 sample at a J-to-K edge,
as observed in the real 0.4.20 root capture. This does not modify raw samples or
relax SYNC, packet, EOP or CRC validation; affected packets are annotated.
Sampling rate is nominal, not independently calibrated: the successful root
transaction must decode correctly before the failed child window is interpreted.
Missing/invalid decoding does not establish a malformed on-wire packet or an
absent device response. This cannot measure analog signal quality, voltage, or
the Hub's downstream pins. The receiver-movement acceptance target is unchanged.
Version 0.4.19 was flashed and verified on the target Pico, then started after
safe reconnection to the Host topology. Actual captures now contain a valid
root address-zero SETUP (`2d 00 10`), DATA0 with the expected eight-byte request
and valid CRC16, followed by ACK (`d2`). The first downstream port's window
contains three complete identical SETUP/DATA0 pairs with valid CRCs but no
decoded ACK; a fourth pair is cut short by the capture boundary, not evidence
of a malformed transmitted packet. These are real digital samples, not the
decoder's synthetic tests. This supports correctly formatted requests on the
Pico side, but does not show the Hub's downstream signals or establish whether
forwarding, port reset, clock compatibility or electrical conditions cause the
missing response. Hardware status still reports 16 RX timeouts and no mounted
HID interfaces; real receiver movement remains unverified.

Version 0.4.20 addresses an observed recovery gap, not a proven cause of the
first missing ACK. When downstream enumeration exhausts its retries, the
Host clears that port's `PORT_ENABLE` and reads its status back before
enumerating another device. It never requests `PORT_POWER` changes. Only after
the port is verified disabled are reserved device resources released and Hub
polling resumed. This prevents failed default-address devices from being left
enabled while later devices enumerate. Successful enumeration and root-port
reset behavior are retained; there is no special-case CH342 driver or UART
command. `usb status` reports cleanup activity, verified-disable count and errors.

If disable/readback fails, still reports enabled, or takes over two seconds,
new enumeration is blocked and an explicit error is retained. The code does
not reuse a possibly outstanding control buffer or claim a successful disable.
Unplug/reconnect the Hub's **upstream data connection** to clear this blocked
state while keeping the established safe Host power arrangement; do not connect
the running Pico Host USB port to a PC. Late cleanup callbacks are ignored.

For a fresh same-port receiver observation, wait for enumeration/cleanup to
finish, save the existing `usb wire` dump, then issue `usb wire arm`. It retains
the root reference and rearms only the previously captured downstream Hub/port.
Keep Pico and Hub powered, unplug/replug the receiver at that same port, then
read `usb status`, `usb mouse` and `usb wire`. Each new dump records its capture
generation and start time; replaying an old dump does not increment generation.
Rearming during capture, replay, enumeration or blocked cleanup is rejected.
The decoder accepts both legacy 0.4.19 dumps and the new metadata.

Version 0.4.20 was observed after verified flashing and safe Host reconnection.
Its actual startup trace confirms both failed ports (1 and 4) are disabled and
read back as `0101`, with `disabled=2`, `errors=0` and `blocked=0`. Port 1 cleanup
completes before port 4 enumeration begins. Startup still has no HID mount;
the new root sample decodes SETUP/DATA0/ACK and the child sample has three
complete valid SETUP/DATA0 pairs without a decoded ACK, followed by a pair
truncated by the capture boundary. This validates cleanup, not mouse recovery.
The powered same-port receiver hotplug comparison has also completed.
Generation 1 records a new request after the Hub reported receiver removal,
reinsertion and port reset; the CH342 USB port had already been disabled.
The root reference still decodes correctly, but the fresh child capture again
has three complete valid request pairs without a decoded ACK. Cleanup then
reports `disabled=3`, `errors=0`, `blocked=0`, with no HID mount or movement.
Thus cleanup and powered hotplug alone did not resolve the missing response.
This does not rule out shared-power effects, physical reset/forwarding or
clock/signal compatibility; the UART VBUS presence flag is not a voltage
measurement. Downstream electrical observations and real receiver movement
remain outstanding.

### 0.4.21: power-good and debounce before downstream reset

This version corrects a software ordering gap. It was flashed and verified on
2026-09-27, then started after safe Host reconnection. **The actual receiver
enumeration still fails; no HID interface or real mouse movement was received.**
The pinned Hub driver previously issued `PORT_RESET` before USBH's 450 ms
connection delay. USB 2.0 section 9.1.2 and Figure 7-29 instead require at least
100 ms of connection debounce **before** reset, followed by a separate reset
recovery interval. The
[upstream TinyUSB enumeration implementation](https://github.com/hathach/tinyusb/blob/29851564af08cb4909175315d20c71f5eca13040/src/host/usbh.c#L1681-L1865)
also waits 150 ms before checking and resetting an external Hub port.

The new hash-guarded overlays leave the vendored SDK unchanged:

- After the last successful `PORT_POWER` request, the Hub waits
  `bPwrOn2PwrGood * 2 ms` before polling connection notifications and finishing
  configuration. The observed CH334 descriptor requests **96 ms**; zero and
  the maximum 510 ms value are also supported.
- A Hub connection notification no longer resets the port. USBH first owns
  the enumeration, then polls port status at 10 ms intervals for **150 ms**
  of stable connection. Latched connection changes restart that interval;
  stale reset-change is cleared before a new reset. Duplicate notifications
  do not interrupt preparation; other attaches remain queued.
- Only then is `PORT_RESET` issued. The host waits 20 ms before checking,
  polls for reset completion and port enable, clears reset-change, waits
  **10 ms of recovery**, and rechecks readiness before the first address-zero
  device-descriptor request. A still-incomplete reset times out at 200 ms;
  preparation has a two-second overall limit.
- New waits use the existing cooperative poll, not blocking sleeps. Physical
  root-port reset timing is unchanged. Disconnects cancel pending preparation;
  transfer/status failures use the existing disable/readback recovery. A lost
  callback blocks further enumeration without reusing its buffer, and late
  callbacks cannot advance another operation or a reused Hub address.

`usb status` adds preparation activity, phase (0 debounce, 1 reset, 2 recovery),
pending transfer, reset/ready/error counts and the last preparation error.
Trace messages include requested/elapsed power-good time, measured stable time
before reset, and recovery time before descriptor access. Reaching `ready`
means only that port preparation completed, **not** that a HID device mounted
or real movement was received.

The prepared image is
`backups\pico_bt_hub-0.4.21-usb-pre-reset-timing.uf2`; the 0.4.20 diagnostic and
standard 0.4.11 recovery images are retained. Native tests execute the generated
Hub callbacks and USBH preparation/first-descriptor path, including exact
timing boundaries, clock wrap, reconnect debounce, reset polling, unplug,
failure recovery, duplicate attaches and stale completions. Flashing used the
established manual BOOTSEL procedure and did not automatically start USB Host.
The user subsequently disconnected the Pico's PC USB cable and restored Host
wiring. COM12 confirms `firmware=0.4.21-open-test-usb-host-probe`, PC1 BLE ready,
preparation `resets=2 ready=2 errors=0`, and cleanup
`disabled=2 errors=0 blocked=0`. The retained trace shows port 4 reset after
163 ms of stable connection, with 18 ms recovery before descriptor access.
The 8192-byte trace ring overwrote 10439 earlier bytes, so this capture does
not retain the exact power-good or port 1 preparation timing messages.

The new boot's digital capture has a valid root SETUP/DATA0/ACK reference.
The receiver on port 1 still shows three complete, CRC-valid SETUP/DATA0
request pairs without a decoded ACK, followed by a fourth truncated DATA0.
Root/child `started_us` are 1378442/1886699, both generation 0 of the new boot;
these are not replays of the earlier 0.4.20 powered-hotplug capture.
Final `RX-timeouts=16`, no HID mount and no mouse movement confirm that the
ordering correction alone did not resolve the fault. No additional rearm,
reset or hotplug was performed during collection.

Evidence: `build\usb-wire-0421-hardware-20260927-234512.log`,
`build\usb-wire-0421-decoded-20260927-234512.json`, and
`build\usb-wire-0421-status-20260927-2345.log`. Upstream-only digital sampling
still cannot distinguish a request not reaching the receiver from a response
not returning through the Hub. Actual downstream observations remain needed;
port preparation, PC operation and synthetic tests are not mouse acceptance.

A read-only Windows comparison on 2026-09-27 confirmed the same Hub `1A86:8091`
working at USB 2.0 high speed, with UGREEN receiver `2B89:0043` at full speed on
port 1. The receiver has EP0 size 8 and three HID interfaces; interface 1 is a
boot mouse with interrupt-IN endpoint `0x82`, maximum packet 8 and interval
2 ms. Port 4 currently holds a full-speed CH342 dual serial device `1A86:55D2`.
This establishes working PC enumeration, not the Hub's full-speed upstream
operation on Pico, and does not establish a faulty Hub. PC operation and
synthetic decoder tests do not meet the Pico mouse-movement acceptance target.

`usb status` also shows observed hubs/devices, VID:PID, configuration state and
standard Bluetooth HCI interface/endpoint candidates. Identification checks
class E0/01/01 plus interrupt-IN and bulk-IN/OUT endpoints; it does not establish
Classic support, chipset firmware readiness or HID compatibility. Vendor-class
adapters may require a separate driver. Unplugging clears the old address's
metadata. The observer does not claim interfaces or submit HCI commands; the
TinyUSB Hub and HID drivers handle their respective interfaces. Limits are
two hubs, four non-hub devices and four HID interfaces.
The configuration flag comes from `tuh_mounted()`, which is set before class
driver configuration finishes; it is not proof that hub downstream ports work.

Hardware observation on 2026-09-27: 0.4.13 was deployed with the existing bonds
preserved and PC1 reconnected. Hub `1A86:8091` exposed a full-speed `09/00/00`
interface; physical VBUS presence and root data connection both reported 1,
and USB frame/task counters advanced. No downstream interfaces appeared with
the attached peripherals. Earlier supposed flash-drive-only/empty-hub controls
were inconclusive: the user later clarified that the dongle was still attached.

With 0.4.15 deployed, failed enumeration now exits after the initial request
and three retries per port: EP0 becomes inactive, the Hub processes subsequent
port events, and the timeout count stopped at 16 in the two-device observation.
PC1 and the saved pairing metadata were preserved. This verifies the timeout
recovery, **not successful downstream device enumeration**. After the user
explicitly removed the dongle and connected only the flash drive, a new port-1
attach/reset was observed, but its first address-zero SETUP still failed.
The current cable set cannot connect the drive directly to Pico without the
Hub, so that isolation comparison has not been performed. Hub/full-speed
compatibility, downstream electrical conditions and remaining Host defects
are not yet distinguished; these results do not establish a faulty dongle or
working external HCI.

USB events are serviced from the BTstack run loop with a 1 ms timer and a
non-waiting task call. Enumeration itself can contain SDK delays, and UART
diagnostic printing is synchronous: this bring-up image is **not a latency
benchmark**. TinyUSB enumeration failures are logged rather than interpreted as
Bluetooth failures. `status` still reports the original input/output links.

## Historical working milestone: 0.4.0 (2026-09-20)

That hardware milestone demonstrated a Classic Level 0 keyboard and a
bonded BLE mouse controlling either of two encrypted BLE HID computer
connections. Both computers stayed connected while the user switched keyboard
and mouse input to computer 2 and back to computer 1 using the hotkeys.

| Capability | Current milestone |
| --- | --- |
| Real keyboard and mouse forwarding | Working on both enrolled computers |
| `Ctrl+Alt+Right` / `Ctrl+Alt+Left` | Next / previous ready computer; release the chord after each switch |
| Persistent configuration | Input identity/authorization, reserved PC identities, and selected PC |
| Reconnection | Known inputs retry with 1/2/4/5-second capped backoff; device wake-up can still be needed |
| Simultaneous hardware links | Four demonstrated: two inputs and two computers |
| Four computers / six total links | Still pending hardware confirmation |

To reproduce that **specific working configuration**, use the 0.4.0 source
revision and build with:

```powershell
.\tools\build.ps1 -OpenClassicInputTest
```

The firmware reports `0.4.0-open-test`. The optional build mode still intentionally
allows an **unencrypted, unauthenticated keyboard-to-Pico link**; it is not a
security-hardened release. BLE mouse/computer connections retain encryption.
The default build without that switch keeps the stricter Classic input policy
and does not activate saved Level 0 inputs. No manufacturer-specific branch is
needed for the demonstrated setup.

The working firmware artifact has SHA256
`148CF9116134D96A900B1B10343C542DBA52BD6451A06C866743B2CB0D11A487`.
Build timestamps/tool environment may change this hash on a later rebuild.
Binary backups, pairing keys, captured device logs, and downloaded toolchains are
kept out of Git. This historical checkpoint is superseded by the current
standard baseline above.

## Build on Windows

The target is `pico2_w`, not `pico_w` or the non-wireless Pico 2.
The project uses Pico SDK **2.3.1**, C/C++, and BTstack.

Run from the project directory in PowerShell:

```powershell
.\tools\bootstrap.ps1
.\tools\build.ps1
```

The bootstrap script downloads build dependencies locally. The `.deps`, `.tools`,
and `build` directories are generated; they are not application source.
The firmware artifact is `build\pico_bt_hub.uf2`.

The build scripts do not flash hardware.
If Git cannot reach GitHub through your network, bootstrap accepts an explicit
`-GitProxy` URL. It applies only to that invocation and does not change global
Git or Windows proxy settings.

## Mouse-latency tuning

### Optional single-PC Classic output experiment

The `classic-hid-output` branch preserves the usable BLE baseline on `main` at
`0c642f2` (0.4.3). Classic output is **opt-in**, not a replacement for the
four-slot BLE build:

```powershell
.\tools\build.ps1 -Jobs 8 -OpenClassicInputTest -ClassicOutputTest
```

This produces `build\classic-output\pico_bt_hub.uf2` by default, without
overwriting the normal build directory. The firmware identifies itself as
`0.4.3-classic-output-test-acl8` and advertises the name `Pico2W Classic HID Test`.
Omit `-ClassicOutputTest` to build the original BLE output again.

The experiment retains both input slots and their existing authorizations,
but offers **only one Classic HID computer, PC1**. There is no BLE computer
advertising, no second Classic PC, and no disconnect/reconnect hotkey switching.
The composite keyboard/mouse report descriptor is shared with the BLE backend.
Classic avoids the BLE output connection-interval floor; it does **not** promise
a 1 ms report rate or remove the BLE mouse-to-Pico hop.

After boot, wait for the inputs to reconnect, then use USB serial:

```text
classic target AA:BB:CC:DD:EE:FF
pair justworks
```

Use the **computer's Bluetooth adapter address**, not the keyboard address.
From that computer's Bluetooth settings, add `Pico2W Classic HID Test` within
120 seconds. The PC initiates the HID channels; hub-initiated Classic PC
reconnection is not implemented in this first experiment. Pairing closes
automatically once encrypted HID output is ready, or with `pair off`.
Only the explicitly selected address is admitted. `pair keyboard` / console
numeric-comparison enrollment are not implemented for this output experiment.

PC output requires a saved Classic link key, Level 2 security and a 16-byte
encryption key; Just Works provides **no MITM protection**. The separately
authorized Level 0 keyboard exception never enables unencrypted PC output.
The target is saved only after successful secure HID establishment, under
the separate `PHC1` tag. The BLE `PHO1` record, BLE bonds, and input records are
not overwritten. A saved Classic target cannot silently be replaced.

Both Classic profiles use the same control/interrupt PSMs. A pinned,
generated-source BTstack overlay registers a single incoming-service
dispatcher and retains channel ownership through each channel's lifetime.
Selected input connections go to the HID host; the selected PC goes to the
HID device only after security succeeds. Incoming PC security waits are
bounded: an initial enrollment may require retrying from Windows rather
than leaving an unauthenticated HID channel pending indefinitely.
Vendored SDK sources are not edited.

`status` identifies the output transport; `latency` / `latency trace` report
Classic software queue-to-**L2CAP submission**, not queue-to-ATT or radio
delivery. For a fair comparison, also inspect the BLE mouse's actual interval
and peripheral latency: the mouse can still renegotiate its low-power policy.
Classic multi-PC operation and lower end-to-end latency remain unproven until
hardware trials.

The default Classic build spaces ordinary keyboard/mouse L2CAP submissions by at least
**2,500 microseconds** (up to 400 reports/second **combined**, not per device).
A microsecond one-shot worker shares BTstack's async context; there is no
busy-wait or millisecond rounding to approximate 2.5 ms. The first report after
idle can send immediately. Empty ticks produce no reports; missed deadlines
are not replayed as catch-up bursts. Controller backpressure can make the actual
gap longer, and safety neutral releases bypass this pacing. This is a software
submission limit, **not a configured Classic air-link polling interval**.
`latency` exposes the configured period and observed submission gaps; the gap
statistics include idle periods below 60 seconds and are not end-to-end latency.
It also reports PC1's observed Classic link mode (active/hold/sniff/park) and
mode interval separately; software pacing does not establish the link's power mode.

The first Classic hardware trial exposed excessive software queueing
(mouse average about 114 ms, maximum about 585 ms in that capture). Its send
readiness path repeatedly scanned the Flash-backed Classic bond database.
Bond verification is now retained only for the current verified connection:
ordinary reports do not reread Flash, while live encryption, channel ownership,
and authorization checks remain. Disconnect or security failure invalidates
verification; a new connection must verify its bond again. `latency` includes
bond-lookup counts/timing to distinguish connection setup from the send path.

The next capture reduced mouse software queueing to about 1.3 ms on average,
but still showed individual waits above 100 ms and the user reported severe
stutter. It also showed the mouse requesting latency 44 roughly every 30 seconds,
with each accepted change taking several seconds to return to latency 0.
Thus the Flash fix and 2.5 ms pacing alone did not resolve the mouse problem.

In this Classic experiment's `latency fast` mode, initial BLE input negotiation
is allowed to settle. Once an input actually reaches latency 0, BTstack's
connection-parameter acceptance range rejects further nonzero-latency requests
via either L2CAP or HCI, instead of repeatedly accepting and reversing them.
This range is global, so the guard is confined to the Classic-output experiment,
which has no BLE PC links. Interval and supervision-timeout bounds are unchanged.
The original range is restored in normal mode or when no BLE input remains.
An applied BLE input update that nevertheless reintroduces nonzero peripheral
latency re-arms a request for `latency=0`.
Existing connection/encryption/quiet-window guards remain, and repeated requests
are separated by at least 10 seconds. A failed request is not retried indefinitely
without another applied peer update. `latency normal` disables this policy.
The original BLE-output build keeps its one-shot policy. Neither build can
guarantee peer acceptance or zero sensor-to-screen latency.

The guarded hardware trial still exhibited intermittent mouse-only stalls:
the input report counter stopped while the BLE ACL stayed connected, keyboard
reports continued, PC1 remained in Active mode, and the output queue was empty.
Reconnecting only the mouse restored reports temporarily. Returning to normal
mode also preceded recovery, but a remote-initiated mouse disconnect/reconnect
occurred in between, so this does not isolate the guard as the cause or prove
the stall fixed. The current board was left in persisted `latency normal`
(mouse 11.25 ms / latency 44), with the 2,500 us output pacing retained.
Do not describe the latency-zero guard as validated for sustained mouse use.
PC1 also needed scoped re-pairing after reboot due to authentication failure;
reliable saved-Classic-PC reconnection remains unresolved.

The flow-diagnostic build changes no radio parameters. `latency trace` now counts
incoming BLE-input ACL packets and unfragmented ATT notifications/indications
before GATT/HIDS dispatch, with per-attribute counts, lengths and ages. Fragmented
traffic is counted separately, not reassembled or mistaken for missing reports.
These counters reset with `latency reset` and follow ACL generations. `status`
adds HIDS callback, callback rejection, parse failure and accepted mouse/keyboard
counts since each BLE HID discovery. No report payloads or keys are retained.
Comparing these stages during continuous physical movement distinguishes missing
controller deliveries from notification dispatch/parser loss; an idle gap alone
does not demonstrate a stall. Earlier parsed-report counters alone could not make
that distinction.

The September 23 capture reproduced silence in normal mode with the guard off:
the mouse remained connected but no incoming ACL packets reached the host during
the 30-second keyboard-disconnected sample. The user reported complete loss of
motion. Two recorded last-report times were about 30 seconds after encryption
completed; this suggests checking protocol transaction deadlines, not assuming
that subscriptions expire. Recorded mouse disconnects carried reason `0x13`
(remote termination), without a preceding local mouse disconnect command.
That reason does not identify why the peer terminated the connection.

Investigation found a concrete Classic-build omission: replacing the BLE output
module also removed the application's only ATT-server initialization. BTstack's
GATT client does not answer incoming ATT requests; its dispatcher drops them
when no server handler exists. A mouse can act as both HID server and ATT client,
so successful discovery and notifications do not exclude an unanswered reverse
request. Classic output now initializes an independent, static GAP/GATT database
to answer MTU/discovery/read requests or return ATT errors. It exposes no BLE HID
service and does not enable BLE advertising.

The deployed correction captured mouse-originated Read By Group Type (`0x10`),
Read By Type (`0x08`) and Read (`0x0a`) requests, with corresponding responses or
ATT errors from Pico. These are precisely the requests the old initialization
path would discard. One unchanged mouse connection subsequently delivered
reports more than seven minutes after encryption; its parsed counter increased
from 3,468 to 6,043 during a 60-second three-link sample, with no disconnect in
that sample and no rejected/invalid reports. This supports the reverse-request
timeout explanation for the earlier approximately 30-second cutoff, but does
not attribute every remote `0x13` termination to that defect or prove all stutter
resolved. Earlier post-flash reconnects still occurred.

The extended trace retains bounded RX/TX ATT control-opcode counts and first/last
timestamps, excluding notification payloads. This preserves evidence of requests
and responses during discovery even after many motion reports. Fragmented packets
are not decoded; `untracked-opcodes` reports exhausted histogram capacity. These
are HCI-side observations, not proof of peer reception. Classic `latency` also
separates request-to-`CAN_SEND_NOW` waiting from total queue-to-L2CAP waiting, so
stack/controller availability can be distinguished from software pacing. The
latest capture measured a 126.445 ms maximum wait for `CAN_SEND_NOW`, close to
the 126.970 ms maximum total mouse queue wait. This locates an observed output
stall at stack/controller send availability rather than the 2.5 ms pacing timer;
it does not distinguish controller-credit batching, transport scheduling, RF
retries, or PC-side behavior. Input-arrival gaps also remain to be correlated
with physical motion.

After this reflash Windows retained the Classic pairing but did not initiate
reconnection. A user-authorized restart of only the Pico's Windows Classic HID
device node restored encrypted PC1 output using the existing key. The firmware
still lacks hub-initiated Classic PC reconnection; this scoped recovery does not
implement reliable automatic reconnection.

The next controlled change raises only the Classic experiment's host-side
`MAX_NR_CONTROLLER_ACL_BUFFERS` cap from 3 to 8. The actual CYW43439 reports
8 Classic buffers and a separate 8-buffer LE pool; BTstack still clamps the
Classic allowance to what the controller reports. The normal BLE-output build
keeps its original cap of 3. The software report queue, 2,500 us pacing and BLE
mouse parameters are unchanged. This uses existing controller capacity, not
additional radio bandwidth or eight computer connections.

`latency` now records controller-reported capacities and classifies each send
request's initial HCI state: no controller credit, shared packet buffer reserved,
transport unavailable with credit available, or HCI ready. These are initial
snapshots, not a trace of every state transition during the wait. In the 8-buffer
capture all five `CAN_SEND_NOW` waits of at least 10 ms began without credit;
HCI-ready requests completed within 156 us. Thus credit exhaustion is directly
observed for those delays, but why credits were delayed remains unproven.

| Separate approximately 60-second motion captures | 3-buffer cap | 8-buffer cap |
| --- | --- | --- |
| Mouse submissions in the measured window | 2,416 | 1,728 |
| Mean MCU queue-to-L2CAP time | 1.53 ms | 0.91 ms |
| Mouse queue waits at least 10 ms | 58 (2.40%) | 6 (0.35%) |
| Maximum mouse queue wait | 126.97 ms | 131.15 ms |

These are different physical-motion samples, not matched workloads or
sensor-to-screen measurements. The user reported noticeably smoother motion
with occasional remaining stutter, so the 8-buffer cap is retained for this
experiment. No links disconnected during the latest window. The unchanged
worst-case spike means the problem is improved, not fully resolved; raising the
cap above controller capacity would not be a solution.

Repeated PC authentication failures with `0x23` were also observed while Pico
immediately initiated authentication on an incoming ACL. The Classic backend
now gives the incoming PC 500 ms to initiate authentication before requesting
its own security upgrade if still needed. Reports remain blocked until the same
bond, Level 2, 16-byte-key and two-HID-channel checks succeed. The next connection
encrypted using the saved key without a Pico authentication request or collision.
This does not add outgoing PC reconnection or relax failure handling.

#### Mouse-only 7.5 ms interval trial

The optional interval trial retains the 8-buffer cap and 2,500 us Classic output
pacing:

```powershell
.\tools\build.ps1 -Jobs 8 -OpenClassicInputTest -ClassicOutputTest -MouseInterval7p5
```

It identifies itself as `0.4.3-classic-output-test-mouse7p5` and writes to the
same Classic build directory. In normal mode, only HID-ready BLE inputs with
a mouse role request minimum/maximum interval 6/6 (7.5 ms). Composite mouse
devices are included; keyboard-only inputs and PC output links are excluded.
The request retains the current peripheral latency and supervision timeout;
returning from fast mode instead restores their saved baseline values.
Encryption, input-radio-busy, serialized-update and quiet-window guards remain.
A subsequently applied peer interval change can re-arm a request, spaced at
least 10 seconds from the previous request. Rejection or timeout alone does
not trigger retries. Normal mode does not enable the latency-zero rejection
guard. Peer/controller acceptance is still required; `latency` reports the
actual interval, not just the desired value.

The hardware accepted 7.500 ms while retaining peripheral latency 44 and
supervision timeout 2,160 ms. PC1 remained at 2,500 us software pacing, and all
three links became ready using their saved authorizations. Windows required
one separately authorized restart of the Pico HID node; FILCO needed waking.
The user reported occasional very good response followed by noticeably laggy
motion, so the interval change has **not** resolved intermittent stutter.

In a subsequent 60-second capture with all three links established, there were
1,930 mouse reports and 1,766 output submissions after movement merging. The
short input-gap mean was 9.51 ms (gaps above 100 ms excluded). Mean MCU
queue-to-L2CAP time was 1.76 ms, with 76 waits of at least 10 ms (4.30%) and a
65.57 ms maximum. All 55 `CAN_SEND_NOW` waits of at least 10 ms started without
controller credit; their maximum was 65.06 ms. There were no observed parameter
changes, disconnects or output errors in this window. Reports occupied only
the first approximately 21 seconds, so this is not a confirmed continuous-motion
comparison; absent reports do not distinguish idle from a stall. Different
workloads also prevent attributing the change in slow-wait frequency solely to
7.5 ms. These measurements do not include controller residence or screen latency.

Omit `-MouseInterval7p5` to build the preceding Classic policy again. Its saved
working image, with the mouse previously negotiating 11.25 ms, is
`backups\pico_bt_hub-classic-acl8-mouse11p25.uf2`, SHA256
`E0F23A5CBB858E7A168436E44DEAA1C474B1B74C69A564EBF4A0F5C40A5424C2`.
The trial is off by default and cannot be enabled in the BLE-output build.

#### 7.5 ms output pacing trial

To retain the mouse's 7.5 ms input interval and reduce the Classic output rate
limit independently:

```powershell
.\tools\build.ps1 -Jobs 8 -OpenClassicInputTest -ClassicOutputTest -MouseInterval7p5 -ClassicOutput7p5
```

This build identifies itself as
`0.4.3-classic-output-test-mouse7p5-output7p5`. Ordinary keyboard/mouse
submissions share a minimum gap of 7,500 us, approximately 133.33 reports/second
combined. The 8-buffer cap, input latency/timeout and security policy are
unchanged. The first report after idle still sends as soon as possible; this is
not a mandatory 7.5 ms collection window. Pending pure motion with unchanged
buttons merges using the existing queue, while button transitions, wheel
events and keyboard events remain ordered. Safety neutral releases bypass
pacing, and missed opportunities do not generate catch-up bursts.

This may reduce burst pressure at the cost of additional pacing delay; it does
not guarantee an available controller credit or a radio transmission every
7.5 ms. The input BLE interval and output software timer are not synchronized.
Use `latency` to distinguish pacing/queue wait from `CAN_SEND_NOW` wait.
Omit only `-ClassicOutput7p5` to return to the 2.5 ms output limit while retaining
the 7.5 ms input trial. Neither trial flag affects the normal BLE-output build.

The 7.5 ms output trial was deployed with all three links ready, the mouse at
7.500 ms / latency 44 / timeout 2,160 ms, and the Classic cap still 8. Its
60-second sample contained 1,823 mouse inputs and 1,666 mouse submissions.
Mean mouse queue-to-L2CAP time was 4.00 ms, with 45 waits of at least 10 ms
(2.70%) and a 78.21 ms maximum. There were 29 slow `CAN_SEND_NOW` waits, all
starting without controller credit, with a 72.57 ms maximum. No disconnects or
parameter changes were observed in the window. This differs from the earlier
2.5 ms sample (1.76 ms mean, 65.57 ms maximum), but input workloads and keyboard
activity were different; it is not a matched or end-to-end latency comparison.
Reducing the rate limit did not eliminate long waits and did add pacing delay.

The user's final requested configuration is **7.5 ms mouse input, 2.5 ms
Classic output, and 8 controller buffers**, not 11.25 ms input. The preceding
firmware was restored from
`backups\pico_bt_hub-classic-mouse7p5-output2p5.uf2`, SHA256
`28850090724E189293B397B515D04D88E34AE890DDBB8463EFF8F26483929FD3`.
Build that configuration with `-MouseInterval7p5` and without
`-ClassicOutput7p5`. Intermittent stutter remains unresolved; neither pacing
setting guarantees timely radio delivery.

The saved rollback image is
`backups\pico_bt_hub-0.4.3-ble-baseline.uf2`, SHA256
`F89C76D5C31E8B69281990E3858DD56C82E152FAE8F6E6843A3E68D0A0A7F025`.
Reflashing it restores the BLE backend using its preserved records; normal
firmware loading does not erase pairing storage. Do not globally clear bonds
or remove all Windows Bluetooth devices to diagnose the experiment. If
Windows reuses a cached BLE device entry, resolve that individual pairing
explicitly before proceeding.

### BLE baseline measurements

The saved 0.4.0 checkpoint was the initial functional baseline. Firmware 0.4.1 adds
on-demand measurements and a reversible performance mode:

```text
latency
latency trace
latency reset
latency fast
latency normal
```

`latency` reports the actual LE connection interval, peripheral latency and
supervision timeout for each live link, recent input-report spacing, and
per-computer output queue statistics. Values are recorded at the Pico:
input gaps describe received reports, and queue times end at a successful
`att_server_notify` submission. They do **not** measure mouse sensor latency,
radio delivery acknowledgement, Windows processing, or screen presentation.
Gaps longer than 100 ms are separated from the short-gap averages. These gaps
may represent normal idle time **or a stall**; the firmware cannot distinguish
them solely from the absence of a report.
No key codes, movement values, or report payloads are logged by these metrics.

`latency fast` enables event-driven queue draining. Starting with firmware 0.4.2,
it requests an exact **7.5 ms** LE interval (minimum and maximum both 6 units)
with zero peripheral latency for the currently selected computer. Previously
the 7.5--15 ms request allowed a computer to retain 15 ms.
Existing BLE inputs retain their 7.5--15 ms request policy with zero peripheral
latency; their requested maximum is capped at the current interval when it is
already below 15 ms, so a 7.5 ms input link is not asked to slow down.
The host/device can reject or choose different
parameters; the printed negotiated values, not the request, determine whether
radio timing changed. By default, inactive computers stay connected and are not accelerated;
a previously accelerated computer is requested back to its baseline when it is
no longer selected. Controller requests are bounded rather than retried in a loop.
Firmware 0.4.3 also waits for input connection/authentication/HID discovery,
scanning and closing operations to finish before optional parameter requests.
It observes a two-second quiet window after radio activity or a selected-PC
change, and waits for an outstanding local or observed peer update to complete
or expire before requesting changes on another link. Reports continue flowing
while requests are deferred; reconnect attempts are not delayed by this policy.
Already-satisfied intervals and peripheral latency are left alone instead of
requesting an identical update. Exact baseline restoration also compares the
supervision timeout. These guards reduce avoidable negotiation traffic; they
do not prove the cause of earlier `0x28` disconnections or prevent all
controller/host-initiated procedures.
Peers can later renegotiate their power-saving parameters; fast mode does not
continually override them unless the activity experiment below is enabled.
Peripheral latency permits skipping idle connection
events, not a fixed delay added to every moving-mouse report.

#### Activity-triggered mouse latency recovery (0.4.5; boot default updated in 0.4.9)

```text
latency mouse-active status
latency mouse-active on
latency mouse-active off
```

Since 0.4.9, this BLE-output-only feature is **automatically ON at boot when
the saved latency mode is fast**. Normal-mode boots (including no saved or
invalid settings) and Classic-output builds keep it OFF. The runtime override
remains RAM-only: `off` lasts until reboot, then the saved fast mode enables
it again. No new setting or Flash record is added. Switching from normal to
fast without reboot retains the existing manual `mouse-active on` behavior.
Let initial negotiation settle; enabling the feature does not itself request
a parameter update. A new valid report from a ready
BLE mouse marks that specific connection for a latency-0 request if its actual
peripheral latency is nonzero. Keyboard reports, Classic mouse reports, unknown
handles and PC links cannot trigger it. No input payloads are logged.

The existing 250 ms service timer submits the request, not the HID callback.
It retains the mouse's **current connection interval and supervision timeout**,
changing only peripheral latency. Existing fast-mode initialization is unchanged.
All connection/security/busy guards, the two-second radio quiet window and
global update serialization still apply. Attempts on that link are spaced at
least 10 seconds apart, including failed attempts. A queued activity trigger
expires after one second without another mouse report, so a delayed/retried
request requires recent activity instead of waking an idle connection indefinitely.
Continuous reports coalesce into one trigger; an update that actually reaches
latency 0 clears it. A successful local API return alone does not mean the
parameters took effect: inspect `latency` and `latency trace`.

Peer power-saving requests remain allowed. The firmware neither forces an
idle value of 44 nor repeatedly requests 0 in the absence of reports. `off`
clears unsubmitted activity triggers; it does **not** undo an in-flight update
or force previously negotiated values back onto the mouse. The mouse may
renegotiate its own idle parameters. `latency normal` also disables this
feature before following the existing baseline-restoration policy. `latency reset` clears its
request counters but retains enabled state, activity triggers and pending
updates. `status` and `latency` expose enabled state and per-mouse request counts.

This cannot remove the wait preceding the first received report, and parameter
updates themselves take time. It is an experiment for subsequent continuous
stutter, not a guarantee of zero latency. Compare OFF and ON on the same image,
with the same links and actual interval/timeout, after reproducing the idle
condition; do not equate a request with a successful improvement.

Versions 0.4.5--0.4.8 defaulted this feature OFF at every boot. On 2026-09-26,
a fresh 0.4.8 boot retained fast mode but lost the previously enabled runtime
mouse-active setting. The mouse subsequently requested peripheral latency 44
after its initial latency-0 update, and no activity-triggered recovery was
enabled. Version 0.4.9 changes only the boot default; the busy/quiet guards,
activity expiry, retry spacing and accepted peer power-saving updates remain
unchanged. The PC standby experiment still defaults OFF.

On 2026-09-25, 0.4.5 was flashed with bonds retained and the experiment was
subsequently enabled from the console. The first 40-second observation received
no mouse reports, so it triggered no update and was not a motion comparison.
After the operator later resumed movement and reported normal operation, the
same mouse connection (`0041`, generation 9) showed one activity request at
uptime 790861 ms and an actual update at 794548 ms: peripheral latency changed
from 44 to 0, with 7.5 ms interval and 2160 ms supervision timeout unchanged.
The request-to-application time was 3687 ms, not physical wake latency.
All four links remained connected across this recovery observation.

This supports further activity-triggered trials, not a proven root cause or
complete fix. Earlier input-connection timeouts and two PC2 `0x28` disconnects
remain separate open issues; the latter also occurred without new mouse
parameter requests. Two mouse output-software waits over 20 ms (maximum
46.413 ms) appeared between the pre-movement and recovery snapshots, which
include idle time and are not a controlled continuous-motion latency sample.

#### Classic input disconnect recovery (0.4.11)

A Classic input that loses its ACL before HID setup finishes must not retain a
stale HID CID forever and block the other input's reconnect queue. After three
seconds in disconnect-pending, the input timer checks the actual BTstack ACL
registry. Only when **no live or pending Classic ACL exists for that peer** does
it retire that peer's orphaned HID/SDP channels, detach the old SDP callback,
release descriptor/context storage and resume the existing serial retry queue.
If the transport still owns the peer, cleanup is deferred and logged; this is
not permission to run competing connection attempts or reset the controller.

The SDK 2.3.1 / BTstack eb0bb8b5 overlays also propagate synchronous SDP/channel
creation failures, track the SDP channel before it opens, and reject late
events belonging to a previous channel. Recovery is scoped by peer and profile
owner: other SDP requests and PC connections are not reset. Late ACL arrival
during closing no longer erases the original retry decision. Manual pause,
authentication failure and forget still prevent automatic reconnect.

Existing bonds, saved input identities, explicit Level 0 forwarding consent,
selected PC, latency policy and boot defaults are unchanged. Tests exercise
the generated production HID/SDP code and L2CAP reaper as well as the input
state machine; hardware is still required to verify a particular radio failure.

#### Classic input Central preference (0.4.6)

Classic HID inputs now prefer Pico as the Central, with the keyboard/mouse as
the Peripheral. This follows the HID Host role recommendation, rather than
assuming that every keyboard must always be a Peripheral: the profile permits
both roles. A Windows direct-connect capture on 2026-09-25 confirmed the FILCO
link with the PC as Central using successful Role Change and Role Discovery
events; earlier Pico captures showed the opposite arrangement. This is a real
role difference, not proof that it caused the observed typing latency.

In the normal BLE-output build, incoming Classic ACLs request the Central role
during acceptance, like the observed Windows host. If a selected Classic input
still connects with Pico as Peripheral, it gets at most one additional
`gap_request_role(..., HCI_ROLE_MASTER)` request per ACL connection. In the
experimental Classic-output build, the global incoming policy stays unchanged
for PCs; only selected Classic input links receive that targeted request.
BLE input/output roles, connection intervals, mouse buffering, HID routing,
security and stored bonds/forwarding consent are unchanged.

This is a preference, not a guarantee: role negotiation may fail. A refused
request is logged without disconnecting an otherwise usable input, and later
role changes do not trigger a retry loop. `status` reports the actual
`Pico-role` (`0=central`, `1=peripheral`, `255=unknown`) and the preference.
Connection and Role Change logs distinguish a queued request from a confirmed
role; `latency trace` retains the existing live-role history. Check the actual
role after reconnecting before comparing latency. A forgotten input remains
forgotten; this feature does not reauthorize or pair it automatically.

The preference defaults ON via `HUB_CLASSIC_INPUT_CENTRAL`. For a same-version
legacy-role comparison, add `-ClassicInputPeerRole` to the existing
`tools\build.ps1` command (prefer a separate `-BuildDirectory`), or configure
CMake with `-DHUB_CLASSIC_INPUT_CENTRAL=OFF`. Keep the other build options and
link topology identical. `latency normal` does not change this build-time role
preference. Building does not flash the board.

#### Passive Classic mode diagnostics (0.4.7)

`latency classic` prints a compact, read-only snapshot for each Classic ACL:
Pico's role, the last successfully observed mode (`active`, `hold`, `sniff`,
`park`), mode interval in 0.625 ms units and microseconds, event age and last
status. It reports `unknown` until a successful HCI Mode Change is observed;
neither an input gap nor being Central establishes a Sniff mode/interval.
It sends no HCI query or tuning command and does not exit Sniff, change link
policy, disable power saving, or request role changes.

The last successful Sniff Subrating event is recorded separately, including
maximum TX/RX latency and minimum remote/local timeout in 0.625 ms units.
These are last-observed parameters, not proof that Sniff is currently active
or a fixed HID report period. Failed events retain the preceding successful
state and are explicitly reported. Both event types also appear in the bounded
`latency trace` control history, tagged with handle and connection generation.
The link snapshot, statuses and event counts are since connection and survive
`latency reset`; disconnect/reuse starts a new unknown snapshot. No input
contents are retained. Even compact printing can perturb timing, so capture
snapshots before/after typing rather than continuously.

On 2026-09-25, a different same-model FILCO was confirmed HID-ready with Pico
Central on 0.4.6, but the operator reported very laggy typing without perceived
lost keys. Repeated input-report gaps near 67.5/135/202.5 ms warrant checking
the actual link mode; they are not measurements of physical key latency.
During that observation, 38 new PC1 keyboard notifications each had less than
1 ms of measured MCU enqueue-to-ATT-submission wait (mean about 518 us).
This rules out a large software queue in that window, not radio/host latency.
Version 0.4.7 adds observations only; it does not claim to fix this symptom.

#### Standby PC interval experiment (0.4.8; 15 ms since 0.4.10)

```text
latency pc-standby status
latency pc-standby on
latency pc-standby off
```

This BLE-output-only experiment is **default OFF and RAM-only** and requires
`latency fast`. It requests an exact **15 ms** interval (12 units of 1.25 ms)
and peripheral latency 0 for ready, authenticated, subscribed **unselected PC
connections**. It leaves input-device policy unchanged. The selected PC still
requests 7.5 ms / latency 0. This is a request to the PC's Central, not direct
control of radio time slots, and not proof that airtime contention caused lag.
The idle PC's current supervision timeout is retained if valid at 15 ms;
otherwise a 2-second timeout is requested. Links stay connected.

On selection changes, the newly selected PC's fast request takes priority over
slowing the former PC. Reports are not held waiting for parameter negotiation,
so initial input can still use the previous slower interval. The existing
two-second quiet window, global local/peer-update serialization, encryption and
input-radio-busy guards apply. A release barrier or unavailable selected PC
defers the experiment's PC requests. Switching is therefore **not guaranteed
to restore 7.5 ms immediately**. Each policy transition gets one attempt;
rejection, timeout or a different applied interval does not trigger a retry
loop, forced disconnection, or repeated overriding of peer changes.

`status`, `latency`, and `latency pc-standby status` distinguish requested
standby state from the **actual negotiated** interval, peripheral latency and
pending state. Successful submission/acceptance alone is not application:
look for `actual-interval=15.000ms`, `latency=0`, and no pending update.
`latency trace` retains the usual parameter request/application metadata.

`off` requests previously tuned standby PCs back to their original baseline,
not necessarily 7.5 ms; the selected PC remains fast. In-flight requests finish
before restoration is attempted. Baselines remain protected until restoration
actually succeeds. `latency normal` disables this experiment before restoring
the normal policy, reboot disables it, and `latency reset` preserves its state.
Pairing, bonds, input authorization and selection persistence are unchanged.
Use this switch for a same-firmware A/B comparison; do not simultaneously
change mouse settings or assume the 15 ms request cured intermittent lag.

On 2026-09-25, 0.4.8 was flashed and this experiment enabled. Both PCs
accepted 100 ms when unselected and returned to 7.5 ms when selected, with
all four links restored. Observed fast requests took approximately 1.2--1.3
seconds from submission to application, in addition to the policy's quiet
window before submission. The operator reported noticeable mouse delay just
after switching, improving later. This is a demonstrated transition cost,
not evidence that receive buffers are full. The mouse also independently
renegotiated its own power parameters; its contribution remains unisolated.

Version 0.4.10 replaces that 100 ms standby target with 15 ms to reduce the
slow-interval penalty immediately after selection, before 7.5 ms takes effect.
It does not shorten the quiet window or guarantee faster negotiation, and
it offers less reduction in connection-event frequency than the 100 ms trial.
The same runtime switch is reused; mouse-active's fast-boot ON default,
input policies and saved device records are unchanged.

#### Returning to baseline

`latency normal` restores the baseline send scheduling and requests each tuned
link's earlier connection parameters. Neither mode changes encryption, bond
keys, slot selection, or input authorization. Mode selection is saved separately
from device identities and restored at reboot. With no saved mode, startup uses
normal mode so baseline measurements can be collected first.

`latency trace` prints bounded RAM-only timing and connection history without
key codes, button states, movement values, PINs, or raw report payloads. It adds
long input-gap observations, slow output queue waits, link/parameter events,
controller completed-packet statistics, and run-loop timing. Routine reports
are counted rather than printed continuously; this is not a Flash log.
Controller completed-packet counts include other ACL traffic and represent
controller buffer-credit returns, **not** per-report delivery acknowledgements,
radio retransmission counts, or end-to-end latency. Dumps themselves consume
processing/USB time, so collect them after a movement sample, not continuously.
`latency reset` clears measurement history/counters without disconnecting
devices, discarding queued input, changing pairing, or cancelling pending
parameter procedures. Long-term stability must still be observed on hardware.

Version 0.4.4 adds a separate 96-entry protocol history: input reconnect backoff
and attempt boundaries, encrypted/discovery/ready/first-HID/first-router
milestones, each HID input subscription's local API result and raw ATT completion,
and RX/TX ATT Error Response request opcode, attribute handle and error code.
It is independent of the limited opcode-summary table; saturation there cannot
hide these details. Link generations distinguish reused connection handles.
Evictions are counted, and `latency reset` also clears this history.
An ATT `Attribute Not Found` response (`0x0a`) often terminates discovery normally;
it is not automatically a connection failure. "First router submission" can
still be held by the existing neutral-input safety gate, and no milestone
measures physical mouse motion onset or Windows display latency.

For comparison, reset the counters, move the mouse continuously for the same
duration on the same selected computer, read `latency`, and repeat in the other
mode. Keep other computer links connected throughout. A shorter BLE interval
can increase radio duty cycle and mouse battery use; do not trade stable
multi-device operation for a requested interval the controller cannot sustain.

In the firmware 0.4.1 manual same-PC comparison, mean mouse queue-to-ATT submission time fell
from 6.345 ms (767 packets) to 1.437 ms (1,353 packets); the operator reported
more responsive motion with normal clicks and scrolling. PC1 remained at
15 ms and the mouse remained at 7.5 ms. The mouse accepted latency 0 initially,
then renegotiated latency 44. This is an indicative queue/scheduling result,
not an end-to-end latency measurement or proof of a radio-interval improvement.

The first 0.4.3 hardware trace showed both PCs at 7.5 ms after recovery, all four
links ready, and user-confirmed input/switching operation. The recorder observed
real controller-credit events and skipped a redundant request for an already
7.5 ms PC. It also retained a PC1 supervision timeout (`0x08`) shortly after a
parameter request, followed by automatic reconnection at 7.5 ms. This is not
evidence that the earlier intermittent disconnections have been eliminated.

A later stutter capture contained repeated mouse input gaps around 232--240 ms,
while software queue waits remained around 0.5 ms. The operator confirmed
stuttering during continuous movement. Reapplying the mouse's 7.5 ms /
peripheral-latency-0 parameters without rebooting restored subjective smoothness;
both PC intervals stayed at 7.5 ms. This is a useful diagnostic lead, not proof
of a single cause: PC2 subsequently reconnected after another `0x28` error.
The current policy still permits later peer parameter updates; it does not
permanently enforce zero peripheral latency or disable device firmware sleep.

### Optional BLE mouse-buffer experiment

```text
mouse-buffer on
mouse-buffer off
mouse-buffer status
```

This is a **default-on, RAM-only** BLE output experiment: rebooting enables it.
The BLE firmware version has a `-mouse-buffer` suffix. A runtime `off` lasts until
the next reboot; it is not written to Flash.
It is independent of `latency fast|normal`, changes no connection parameters or
pairing records, and is explicitly unsupported by the Classic output backend.
`off` removes the experimental hold immediately, retains queued input in order,
and resumes baseline sending/merging; it does not disconnect devices or discard
movement. Actual sending still requires transport readiness and available credits.

With the experiment enabled:

- The first pure movement can use any real available ACL credit. A new movement
  burst means an empty output queue and either no movement since the last queue
  reset or at least **100ms without nonzero X/Y input**, measured by the run-loop
  millisecond clock. An empty queue alone does not restart this allowance.
  Only the first packet bypasses the reserve if a large delta needs splitting.
- Subsequent pure movement normally submits only with **at least five free ACL
  slots**, leaving four. The observed controller has eight LE slots shared by
  all BLE links, not eight per mouse/PC; other traffic can consume the reserve.
  Adjacent pure movement with unchanged buttons is merged in the Pico's existing
  64-event queue. There is no attempt to fill a 251-byte ACL packet: a mouse HID
  report remains six bytes regardless of accumulated movement.
- The oldest queued movement has a **7,500us intentional-hold budget**. Merging,
  splitting and `latency reset` do not restart its deadline. A microsecond timer
  wakes the sender at the deadline; controller-credit returns also wake it.
  At expiry the reserve is relaxed, but zero real credits still means waiting
  for ATT/controller availability, not repeatedly scheduling an expired timer.
- Keyboard reports, mouse button changes, wheels and zero-motion reports are
  ordered boundaries. They are not merged with pure movement or with one another.
  Their arrival cancels reservation-based waiting for earlier movement, which
  must be submitted **before** the boundary; clicks never overtake movement.
  Safety releases retain their existing priority and queue-clearing semantics.

The 7.5ms value limits deliberate accumulation, **not** total queue, radio or
Windows latency. Run-loop scheduling, transport backpressure and errors can delay
submission further; the controller-credit count is not a mouse acknowledgement.
If the deadline timer cannot be armed, firmware logs an error and disables only
this experiment, retaining the queue and resuming the baseline path.

`mouse-buffer status` (also included in `status` and `latency`) shows the policy,
per-PC waiting state, shared free credits, `hold-entries` and `deadline-sends`.
`hold-entries` counts transitions into reservation waiting, not reports;
`deadline-sends` counts successful submissions that relax the four-slot reserve
after expiry, not delivered packets. `latency reset` clears these counters but
leaves the experiment and pending deadlines intact. Enabling it with existing
queued data cannot undo merges already performed while it was off.

For A/B measurements, keep the same `latency` mode and negotiated interval,
set `mouse-buffer on` or `off` **before** `latency reset` and `output-test`.
During an active generator run, `mouse-buffer off` and `mouse-buffer status`
remain available; enabling requires stopping the run first. Disabling mid-run
is safe but mixes policies within that sample. Performance benefit remains
unverified until a hardware comparison is performed.

On 2026-09-25, the default-on image `0.4.3-open-test-mouse-buffer` was flashed
with existing bonds retained. M720, FILCO and PC1 reconnected, and the operator
reported much better mouse feel. This was not a controlled buffering on/off
comparison. `latency fast` negotiated 7.5ms/latency 0 for both BLE links, but
the mouse subsequently restored peripheral latency 44 before the sample;
PC1 retained 7.5ms/latency 0.

In the following 30-second physical-mouse sample, the first post-capture
`latency` snapshot counted 3,701 mouse enqueues/submissions, no merges or errors,
eight reservation holds and no deadline-reserve overrides. Queue-to-ATT mean
was 537us, maximum 3,424us, with no waits reaching 5ms. Windows recorded 3,198
nonzero-motion events: mean arrival gap 9.382ms, p95 15.408ms, maximum 71.946ms.
Two gaps exceeded 50ms (71.946ms and 70.910ms), and none reached 100ms.
These Windows timings include host scheduling and are not absolute RF latency.
Counts are not a packet-loss calculation.

Pico's input-gap mean excluding >100ms gaps was 8.124ms, with a 22.587ms maximum
in that short-gap group. One >100ms idle-or-stall gap was also counted; the
subsequent 31.1-second trace reported it as 127.545ms. The MCU collection begins
slightly before the Windows capture, and that later trace also extends beyond
it, so neither its 3,794-report count nor its 4,089us output maximum should be
substituted for the earlier snapshot. The trace recorded no disconnections or
parameter changes. There were no keyboard reports in this measurement window;
the operator deferred a separate keyboard sample and retained the running setup.

### Isolated minimum-security Classic input experiment

Normal builds still use Classic Level 2. To make a clearly marked diagnostic
image for testing an unpaired Classic input at the lowest local requirements:

```powershell
.\tools\build.ps1 -OpenClassicInputTest
```

This image sets **Classic Security Mode 2, Level 0**, before HID services are
registered, disables Classic SC-only, and declares NoInputNoOutput/no-MITM/no-
bonding SSP requirements. BLE mouse security and computer-side BLE HID
permissions are unchanged.

After flashing, `status` must show `classic-level=0 classic-mode=2 open-test-build=1`.
Use only an explicitly selected, not-already-bonded input:

```text
open-test 2 AA:BB:CC:DD:EE:FF
```

No PIN is automatically supplied or guessed. A PIN request is recorded and
declined. Standard SSP Just Works may complete without user interaction.
Existing bonds are never erased; a pre-existing bond to the target causes the
experiment to refuse, since that would not test a fresh unauthenticated connection.

The test bypasses the application's encrypted/bonded-target prerequisite.
For a newly selected target it defaults to **not forwarding test reports** and
does not save the target until `bridge on` explicitly authorizes it.
`HID-ready` plus increasing `reports=` proves the HID data path is open;
`encryption-key=0` additionally establishes that it is unencrypted. `saved=0`
before authorization is intentional. If the peer initiates automatic pairing,
the SDK can still receive a key; this is distinct from saving an input target.

Automatic input enrollment is disabled in this image. An explicitly selected
Level 0 input that has reached HID-ready can reconnect automatically during the
same Pico uptime, with the same address and security policy. It is not enrolled
again and no PIN is guessed. Once `bridge on` has saved the target, it also
reconnects automatically after Pico power loss. New PC enrollment is initially disabled. An already
saved BLE mouse remains configured.
`diag` records outgoing authentication requests and both directions of L2CAP
connection requests/responses, so a peer's security refusal can be distinguished
from a locally initiated requirement.

This is not the normal operating configuration. Rebuild without
`-OpenClassicInputTest` and flash that image to restore the default security policy.

### Explicitly forwarding the unencrypted keyboard

For a controlled end-to-end trial, connect the Level 0 keyboard, release all its
keys, and explicitly opt in:

```text
bridge on
pair keyboard
```

`bridge on` permits only the explicitly connected Level 0 input(s) to use normal
HID parsing/routing, and allows PC enrollment in this experimental image. Since
0.4.0 this command also saves each ready Level 0 target and its forwarding
authorization, with a versioned CRC-checked record. A previously saved mouse
record is retained without re-pairing.
The keyboard-to-Pico radio link remains unencrypted and unauthenticated; an
encrypted PC link does not make the entire path secure.

The PC displays its own fresh six-digit code. Type that code on the keyboard
already connected to the Pico, then press Enter. Do not reuse an old code from
an earlier keyboard enrollment. Pairing-code capture suppresses ordinary input
until computer pairing finishes and all physical keys/buttons are released.

`bridge off` disables forwarding of Level 0 inputs, releases their logical state,
and closes any active PC enrollment window. Normal authenticated input devices
are unaffected. The disabled setting is also saved: after reboot the known
keyboard reconnects but cannot forward until you re-enable it. A new Level 0
device never inherits a different device's permission. Use `input forget N` to
remove its saved target and permission without touching mouse or computer bonds.

Persistence does not create a cryptographic bond for a Level 0 keyboard:
`saved=1` means its address and your authorization were saved, not that its
wireless link is encrypted. A normal build without `-OpenClassicInputTest`
retains but refuses to activate such records. Corrupt records or failed writes
are reported and do not enable routing. If disabling permission cannot be
persisted, it is disabled for the current uptime but the console explicitly
warns that reboot state is uncertain; recover storage before relying on it.

## Power-on recovery after initial setup

After one successful enrollment/authorization, keep devices on their original
Bluetooth channels. On startup the Pico loads saved mouse/keyboard targets,
reconnects them, restores their individual forwarding permissions, and advertises
for the bonded computers to reconnect. No new `connect`, `bridge on`, or pairing
command is needed on each normal boot.

Computer slots are reserved by the bonded computer identity, not by which one
connects first. The last explicitly selected computer is saved. If it is offline,
ordinary input waits rather than going to another computer; `Ctrl+Alt+Right/Left`
can explicitly select another ready slot. Encrypted identity resolution must
complete before a computer is allowed to receive input.
After successful bonded encryption, a redundant re-encryption-start notification
does not revoke an already verified, still-encrypted connection with the same
stored identity. Fresh pairing, encryption loss, identity changes and reported
authentication failures still suspend/reject routing. This avoids leaving a
reconnected PC permanently waiting for a second completion event.
Four slot identities and the selection are stored together in a CRC-protected
`PHO1` record, independently of input authorization and Bluetooth bond keys.
Writes occur only when claiming a slot or changing selection, not on every
reconnect or input report. An unreadable/corrupt record or failed write locks
output instead of assigning an arbitrary new computer to the old selected slot.
Such a storage fault requires deliberate record maintenance; an input reset
does not repair or erase computer identities.

For upgrades from pre-0.4 firmware, existing BLE/Classic bond keys and bonded
input records are kept. Reconnect and run `bridge on` once for a previously
RAM-only Level 0 keyboard. Existing bonded PCs acquire stable slots when they
first reconnect successfully to the new firmware. Do not erase/re-pair devices
merely to upgrade this configuration.

Recovery is not instantaneous and cannot wake every device remotely: sleeping
keyboards/mice may need a key press or movement, and PCs must have Bluetooth
enabled and permit reconnection. Keys held during connection are suppressed
until released, so pairing or wake-up keystrokes are not replayed.

## Connect and flash the board

Writing and compiling the firmware does not require a connected board.
Flashing and connection-capacity experiments do.

1. Use a USB **data** cable, not a charge-only cable.
2. Hold BOOTSEL while plugging in the Pico 2 W.
3. Copy `build\pico_bt_hub.uf2` to the board's new BOOTSEL USB drive.
4. The board reboots and exposes a USB serial port.
5. Open that port in a serial terminal, using 115200 baud if the terminal asks.
6. Enter `help`, then `status`. Commands finish with Enter.

USB provides power and the administration console. It is not the keyboard/mouse
output path. Do not copy firmware to an unrelated USB drive or assume an existing
COM port belongs to the Pico.
The HID companion Battery Service reports a fixed 100% for this USB-powered
prototype, not a measured battery charge. Its PnP identity matches the Pico SDK's
RP2350 USB prototype identity; it is not a separately allocated commercial product.

The console command `bootsel` releases input, stops routing, and reboots the board
to the firmware drive. Pairing keys live in the SDK's flash-backed bond store and
normally survive a firmware update.

## First: connect the keyboard and mouse to the Pico

No computer-side Bluetooth connection is needed for this step. Ordinary input
is not saved for later replay while there is no ready computer.

### Automatic enrollment without typing device addresses

With the board already powered and running, briefly press and release
**BOOTSEL**. This requests a **60-second input enrollment window**. Alternatively
enter `enroll` over USB serial.

Put the keyboard and mouse into their own Bluetooth pairing modes. The Pico
looks for standard HID candidates over BLE and Classic Bluetooth and connects
one candidate at a time, checking its report descriptor before accepting its
keyboard/mouse role. Existing saved input devices are not silently replaced.
The goal is one keyboard and one mouse, with either radio transport for each.
Automatic enrollment seeks two physical input devices even if the first one
advertises a composite keyboard/mouse descriptor. If a single composite device
is all you need, `enroll stop` keeps it without waiting for another device.
BLE automatic candidates must advertise as connectable and discoverable, with
HID service/appearance information in their advertisement or scan response.
Nonstandard advertisements may require the manual selection commands below.

Just Works pairing is automatically accepted only for the selected authorized
candidate. Devices that require a passkey, legacy PIN, or genuine numeric
comparison still require the corresponding device-specific action; consult the
USB console for the code/prompt. A device name or strong signal alone is not
proof of identity. Enroll only in a trusted environment.

Firmware 0.3.2 explicitly allows BLE Legacy pairing for inputs that do not
support LE Secure Connections, while still preferring Secure Connections and
requiring 16-byte encryption keys. BLE Legacy pairing is not Classic BR/EDR.
Computer enrollment remains Secure-Connections-only; input connection attempts
wait while that global Security Manager policy is active.

Successful input identities are stored persistently alongside the SDK's bond
store. Later boots reconnect those known inputs, not arbitrary nearby devices.
BLE privacy identities are used rather than treating a changing advertisement
address as a permanent identity. If a device cannot provide usable bonding
information, the console reports the problem instead of promising persistence.
Enrollment ends when the required inputs are enrolled, the window expires, or
`enroll stop` is entered.

The status LED blinks slowly during enrollment. A steady LED means the radio is
running, not that six Bluetooth links have been proven.

**Press BOOTSEL after startup for these controls. Holding it while plugging in
USB still enters the normal firmware-loading bootloader.** Firmware ignores a
button held across startup until it is released. Runtime sampling briefly
pauses flash access from SRAM; this firmware intentionally leaves core 1 unused
and must not add concurrent XIP DMA readers without redesigning this sampler.

### Manual input selection and diagnostics

Put a keyboard or mouse into its Bluetooth pairing mode and scan:

```text
scan classic
scan ble
scan stop
```

The console prints device addresses and, for BLE, public/random address type.
Run one scan at a time; wait for it to finish or use `scan stop` before another
scan or a `connect` command. Scanning does not authorize arbitrary devices to
pair. Connect each selected
device explicitly to input slot 1 or 2:

```text
connect 1 classic AA:BB:CC:DD:EE:FF
connect 2 ble random 11:22:33:44:55:66
```

For a BLE device advertised with a public address:

```text
connect 1 ble public AA:BB:CC:DD:EE:FF
```

Use the address/type actually printed for your device. The addresses above are
placeholders. A BLE privacy address can change; rescan if an old address stops
working.

Some devices require a passkey:

- A displayed passkey may need to be typed on the physical keyboard and
  confirmed with its Enter key.
- A remote displayed passkey can be entered with
  `passkey <ticket> <six decimal digits>`.
- Older Classic devices may require `pin <ticket> <PIN>`.
- Use `confirm <ticket> no` to reject a numeric comparison.

Connect one input at a time and inspect `status` before adding the next.
Successful bonded manual connections are remembered too. Do not use a manual
connection command to silently replace a saved target; forget that input first.
After connecting or reconnecting, release that device's keys/buttons before
ordinary forwarding. Held keys at the moment of pairing are not replayed to an
already connected computer.

After adding the computer connections below, the final connection targets for
the three input combinations are:

| Inputs | Desired HCI link counts |
| --- | --- |
| Two Classic HID devices | 2 Classic, 0 LE Central, 4 LE Peripheral |
| Two BLE HOGP devices | 0 Classic, 2 LE Central, 4 LE Peripheral |
| One of each | 1 Classic, 1 LE Central, 4 LE Peripheral |

`disconnect 1` or `disconnect 2` removes an input connection. It does not switch
or disconnect the computers and is not the same as forgetting a saved input.
It pauses that input's automatic reconnect until the next reboot or an explicit
manual reconnect. Temporary radio loss otherwise triggers bond-only retries.

### Reconnection after power-off or sleep

Firmware 0.3.7 uses retry backoff of **1, 2, 4, then at most 5 seconds**, continuing
until the known device returns or the operator pauses/removes it. After a
successful connection the backoff resets. Attempts are serialized and alternate
between due input slots so an absent device does not monopolize initiation.

An automatic BLE connection attempt has a five-second budget; a failed attempt
must be cancelled and completed by the controller before another begins.
The backoff cap does **not** promise end-to-end recovery within five seconds:
radio link-loss detection, an in-flight connection/cancellation, encryption,
HID discovery, and another device's connection attempt take additional time.
First-time enrollment/PIN entry keeps its longer deadline.

Bonded inputs reconnect with their saved identity and key, including BLE
privacy identities. Transient disconnects/timeouts during encryption are retried
without deleting keys or silently authorizing new pairing. Missing keys or
explicit authentication rejection stop automatic retries and require operator
action. PC pairing/enrollment deliberately pauses new input connection attempts;
existing working keyboard, mouse, and PC links are not reset for a retry.

The Level 0 keyboard follows the same capped retry backoff after it has been
explicitly selected and become ready. In 0.4.0, `bridge on` also persists its
target and permission for the next Pico boot.
Manual `disconnect` remains a pause, not a transient failure, so it does not
immediately reconnect against the operator's request.

The pinned Pico SDK 2.3.1 BTstack build includes a small reconnect overlay in
`patches\btstack`, applied by `cmake\HubBtstackReconnect.cmake` to a generated
copy of `hci.c` under `build\generated\btstack`. It leaves `.deps\pico-sdk`
unmodified. The overlay reuses a pending central connection when an enhanced
completion reports the same identity as a plain public/static-random address,
and releases pending direct-connection state after terminal failures. Otherwise
an orphaned pending context can cause subsequent `gap_connect` calls to return
`0x0c` indefinitely. Explicit cancellation and internal privacy-list restart
remain distinct; established keyboard/PC connections are not freed.

The overlay checks the SDK version, canonical source hash, and unique source
anchors. Updating SDK/BTstack requires reviewing this overlay; it fails the build
rather than silently applying to a different source revision. Its tests exercise
the generated production handler and original GAP connection APIs, with
controller transport/allocation/event delivery mocked.

### Input HOGP completeness and idle recovery (0.4.4)

The companion GATT flow starts **after HID input is ready**, so device metadata
does not become an extra forwarding prerequisite. It discovers Device
Information/PnP ID, reads HID Information flags, and discovers the optional
Scan Parameters Service. If present, it writes Scan Interval Window using
Write Without Response, enables an available Scan Refresh notification, and
resends the parameters on a valid refresh. Commands are retried on temporary
GATT/controller backpressure with a bounded deadline, not a busy loop.
Each input owns its state and listener; disconnect cancels its application
flow without affecting other inputs or PC links.

`status` reports `[hogp N]` state, validated PnP identifiers, RemoteWake and
NormallyConnectable flags, service presence, subscription result, errors and
the number of scan-parameter **submissions** (not peer acknowledgements).
Missing Scan Parameters on a device is normal. Failed or missing mandatory
metadata is explicitly reported as degraded rather than silently called
complete; it does not disconnect an otherwise usable HID input. This does not
claim full HOGP qualification, and does not add Suspend/Exit Suspend commands.

The scan hint uses the existing initiating-scan settings: 60 ms interval and
30 ms window, now explicitly shared with the connection setup. It describes
the active connection attempt, **not continuous scanning during retry backoff**.
The 1/2/4/5-second retry backoff, 5-second LE attempt deadline, BLE connection
parameter policy, security requirements and mouse-buffer policy are unchanged.
Do not interpret idle-to-first-report gaps as measured wake latency; compare
the protocol milestones before changing any of these policies.

`cmake\HubBtstackHids.cmake` generates a pinned copy of `hids_host.c`, leaving
the SDK untouched. Failed local submissions, asynchronous discovery/CCCD
errors, or missing/malformed Report References cannot silently become a
successful HID connection. Partial notification listeners are cleaned up on
failure. The native tests compile the actual generated SDK file, exercise
every nonzero ATT completion status at each subscription position, and cover
cleanup, immediate/scheduling failures and successful multi-report setup.
Repeated requests for the same already-queued GATT callback are coalesced,
not treated as connection failures. A fatal discovery/subscription error also
removes that callback before freeing the client, avoiding stale queued work.

The corrected 0.4.4 hardware run on 2026-09-25 identified the M720 as PnP
`046d:b015`, version `0013`, HID Information flags `03`. It **does not expose
Scan Parameters Service** on this connection; the companion flow completed
without errors. All three input report subscriptions (`002c`, `0030`, `0034`)
returned ATT success. From LE connection at uptime 207238 ms, encryption/
discovery began at 207465 ms, HID became ready at 209426 ms, and the first
router submission occurred at 209430 ms. Thus this connection spent 227 ms
before encrypted discovery and 1961 ms in HID setup; this is **not measured
physical wake latency**. The companion flow finished 498 ms after HID readiness.
Observed ATT errors were `Attribute Not Found` discovery termination responses.
With both PCs and the keyboard subsequently online, the mouse remained on the
same connection generation after at least 61 seconds without input reports.
These observations do not establish the cause of intermittent idle lag or
prove that a future subscription/reconnection cannot fail.

Later in the same run, the operator reported persistent stutter even during
continuous movement after idle. The 18:09 controlled-motion capture retained
the same mouse handle `0041` / generation 3, all four links, and parameters
7.5 ms / peripheral latency 44 / supervision timeout 2160 ms. Connection,
parameter-update and protocol histories did not change. Across the latency
snapshots bracketing the 20-second movement prompt (21.744 seconds between
snapshots), 166 mouse reports arrived, including 88 inter-report gaps over
100 ms; retained slow samples repeatedly measured about 232.5--240 ms.
The 166 corresponding output timing samples all stayed below 1 ms, averaging
546 us from MCU enqueue to successful ATT submission, with no new merges or
notification errors. Across the separate trace snapshots, the raw input
notification count and parsed mouse report count both increased by 199.
This localizes a visible stall to input arrival at the host, not a long output
software queue or repeated HID initialization. It does **not** distinguish
mouse power behavior, controller/radio scheduling, or radio interference,
nor measure PC delivery time. Peripheral latency 44 is legal and is not
proven causal; a mouse-only parameter comparison remains necessary. Slow
history is throttled to one sample per source per 250 ms, so spacing between
printed records is not the actual report period.

## Replacing a mouse or keyboard

No reflash or full factory reset is needed:

| Action | Effect |
| --- | --- |
| Hold BOOTSEL for about 8 seconds while running | Forget both input devices and enroll replacements |
| `inputs reset` | Same input-only reset from the console |
| `input forget 1` | Forget input slot 1 and enroll its replacement, preserving slot 2 |
| `input forget 2` | Forget input slot 2 and enroll its replacement, preserving slot 1 |
| Short BOOTSEL press / `enroll` | Fill missing input roles without forgetting existing ones |
| `enroll stop` | Close input enrollment, retaining successfully saved inputs |

Use `status` to identify which slot contains the keyboard or mouse. They are
assigned as devices connect, so do not assume that slot 1 is always the keyboard.

**Input reset deletes only the selected input identities and their corresponding
input bonds. It does not erase firmware or clear computer pairing records.**
For an unbonded Level 0 input, it deletes only the saved target/forwarding
authorization, not any unrelated Classic keys. Automatic enrollment remains
disabled in the Level 0 build; use `open-test` and `bridge on` for a replacement.
Computer Bluetooth links are not intentionally disconnected. Input state is
released safely; release any held physical keys/buttons before continuing.
Fast LED blinking during a long hold indicates that a reset request was accepted;
release the button and look for enrollment's slower blinking.

The Pico cannot erase a keyboard/mouse's own saved bond remotely. Put replacement
devices into pairing mode; when re-enrolling an old device, clear/re-pair its
corresponding Bluetooth slot if it still insists on using the old key.

## Second: pair a computer using the connected keyboard

Firmware 0.2.0 adds `pair keyboard`. It advertises keyboard-only pairing I/O
capability and requests authenticated passkey entry instead of requiring a
numeric-comparison response over USB.

1. Ensure the physical keyboard shows `HID-ready` and `status` reports
   `keyboard-ready=1`. The mouse can already be connected too.
2. Enter `pair keyboard` in the USB console. This opens a 120-second window and
   pauses ordinary keyboard/mouse forwarding, including to previously connected PCs.
3. Using that computer's own existing input method, open its Bluetooth settings
   and add `Pico2W Keyboard Mouse Hub`. An unpaired Pico cannot yet control the
   computer's settings UI.
4. When the computer displays its six-digit passkey and the Pico requests entry,
   release any held keys, type the code on the keyboard connected to the Pico,
   and press Enter. Top-row and keypad digits work; leading zeroes are preserved.
5. Backspace edits the code. Escape cancels a pending code. `pair off` cancels
   enrollment from the console. The code is neither echoed nor sent as ordinary
   HID input to any PC.
6. The window closes after the pairing result. Release all keys/buttons, inspect
   `status`, and use `select <slot>` to choose the newly ready computer.

Before the passkey prompt, keys are consumed but not accumulated as a code.
After submitting it, input stays paused until pairing completes or is cancelled.
Ambiguous simultaneous digit presses, rollover, modified digits, and a seventh
digit clear the partial code and request re-entry. Once typing begins, one
physical input slot owns that code; another keyboard cannot append digits.
Losing the typing keyboard, a timeout, or the computer disconnecting clears the
entry. No partial code survives a later pairing attempt.

Pair computers one at a time. Repeat `pair keyboard` for computers 2 through 4;
existing computer links remain connected. Pairing new input devices and automatic
input reconnect attempts wait until computer enrollment has ended, because the
Bluetooth Security Manager's I/O capability setting is global.

Each computer needs encryption and both report subscriptions before showing
`ready=1` and `notify(k/m)=1/1`. Paired-but-disconnected computers do not count.
Computer slots are fixed to authenticated bonded identities when first
registered. They remain reserved while offline and survive Pico restart;
reconnection order does not change the selected destination. Inspect `status`
for saved/offline slot identities.

### Console-only pairing and isolated output diagnostics

`pair justworks` opens a 120-second PC enrollment window without code entry.
It declares NoInputNoOutput while retaining Secure Connections and 16-byte
encryption keys, but does not provide authenticated MITM protection. Select the
Pico in the computer's Bluetooth settings; the OS may still request a connection
confirmation or refuse this association method. The window closes after a PC
pairing result. Use this only in a trusted environment.

This command also works in the Level 0 experimental image without `bridge on`,
so the PC output connection can be enrolled independently. It does not enable
input forwarding: the unencrypted keyboard still needs an explicit `bridge on`.

If Windows has forgotten an old BLE bond while the Pico still retains it,
BTstack reports `PIN_OR_KEY_MISSING` before continuing the new Pairing Request.
Only an explicitly open, authorized pairing window lets a provisional,
unencrypted output link continue that handshake. It remains unable to send
input until fresh pairing, identity verification and 16-byte-key encryption
complete; the original authentication timeout still applies. Closed-window
requests, established-link failures and other security errors remain rejected.
No unrelated bonds or logical PC reservations are erased.

The original `pair on` mode is still available without a connected keyboard.
It uses the console's display/confirmation workflow, including
`confirm <ticket> yes` only after comparing both six-digit displays, or
`passkey <ticket> <six digits>` when requested.

To isolate the output radio before adding input devices, `select 1`,
`tap 0x04`, and `move 20 0` explicitly send an A key and cursor motion to the
selected ready PC. Use a blank text editor. These commands are rejected while
physical input peers are connected, a switch is pending, or keyboard enrollment
is active; they never run automatically.

Both BLE and Classic output images provide `output-test 7500` and
`output-test 2500`: a **30-second**, internally timed mouse-motion generator.
First use `disconnect 1` and `disconnect 2`, wait for both inputs to show
`paused`, and confirm that only the selected encrypted PC link remains. Saved targets
and bonds are preserved. Stop scans/enrollment and close pairing before starting.
The generator moves horizontally by one count per event, reversing every
64 events; it never presses a key/button or scrolls. It keeps the normal output
queue, motion merging, controller credits and configured output pacing intact.
Thus `7500` changes the **test event generation period**, not normal output pacing
or the BLE connection interval. BLE uses its existing ATT notification scheduler;
Classic retains its configured minimum submission period. For BLE, record the
negotiated interval from `latency`; `latency fast` requests 7.5ms/latency 0 for
the selected PC but requires peer acceptance and also changes queue draining.
Wait for negotiation to settle before resetting counters and starting a sample.
Changing the selected PC or connecting another peer aborts the generator.

Use `latency reset` before each test; afterward inspect `output-test status`,
`latency`, and `latency trace`. The independent microsecond timer records
generator lateness and skipped periods instead of sending catch-up bursts.
Do not use repeated serial `move` commands to measure 2.5ms performance:
the console itself polls every 10ms. `output-test stop`, `release`, loss of
PC readiness, or input/radio activity stops the generator. Stopping discards
pending test motion and sends neutral safety reports; these are included in
the safety/CAN_SEND counters, not ordinary mouse submissions. The generator is
inactive at boot and never changes persistent settings. Resume the saved input
targets explicitly after the experiment.

Pico enqueue-to-L2CAP timings still do not measure radio delivery. A separate
PC-side, device-filtered Windows Raw Input recorder can measure report arrival
intervals, which also include Windows/message-thread scheduling. Neither metric
alone is absolute sensor-to-display latency. Motion merging and zero-motion
filtering mean different generated/submitted/Raw Input counts are not by
themselves evidence of packet loss.

On 2026-09-25, the same Windows PC was measured with both physical inputs paused
and no other PC connected. Classic used its 2.5ms minimum submission period and
eight controller buffers. BLE used `latency fast`, with **7.5ms / latency 0**
actually negotiated. These compare the existing backends, not protocol-only
limits: Classic and BLE have different queue schedulers.

| 30s sample | Generated | Submitted (L2CAP / ATT) | Merges | Mean / maximum MCU queue wait |
| --- | ---: | ---: | ---: | --- |
| Classic, 7.5ms generator | 3,999 | 3,999 | 0 | 0.601 / 0.721ms |
| Classic, 2.5ms generator | 11,999 | 8,947 | 3,051 | 2.464 / 29.716ms |
| BLE, 7.5ms generator, motion-only repeat | 3,999 | 3,999 | 0 | 0.420 / 0.567ms |
| BLE, 2.5ms generator, motion-only repeat | 11,999 | 11,977 | 22 | 0.415 / 45.384ms |

Classic's 2.5ms sample discarded one remaining merged event at automatic stop;
all samples used the ordinary motion-merging path and no buttons/keys. The BLE
repeat recorded Windows **nonzero-motion Raw Input events** at 131.0 / 132.8 per
second for the 7.5 / 2.5ms generators. Mean gaps were 7.635 / 7.530ms; maximum
gaps were 31.621 / 37.248ms; gaps >=20ms numbered 6 / 7, with none >=50ms.
Each lasted 30 seconds with no reconnect, parameter update or notification
error. The 2.5ms BLE repeat still had two MCU queue waits >=10ms, including
the 45.384ms maximum: higher throughput did not eliminate every outlier.

The first BLE pass submitted 3,999 / 11,995 reports and had maximum MCU waits
0.547 / 13.110ms. Its unfiltered Windows recorder included a 56.609ms **last**
gap after the high-load stimulus ended, motivating a second pass with a
per-event motion/no-motion flag and neutral reports excluded. Do not silently
discard this first sample or treat its unclassified last gap as a proven
in-motion stall. Classic's older Windows samples included neutral events
(maximum 28.960 / 31.247ms; >=20ms counts 50 / 64), so those event statistics
are not identically filtered. These short, non-interleaved samples support
better BLE submission throughput in this setup, not guaranteed 400Hz Windows
delivery, zero latency, or a measured benefit with live keyboard/mouse inputs.
Local logs are `build\classic-output\output-only-*-20260925-*`,
`build\ble-output-only-*-20260925-*`, and
`build\ble-output-motion-*-20260925-*`.

## Switching and input safety

- `Ctrl+Alt+Right`: select the next ready computer.
- `Ctrl+Alt+Left`: select the previous ready computer.
- Unready computer slots are skipped. If none is available, selection does not
  change.
- Release **all held keys and mouse buttons** after a switch before resuming.
- Neutral keyboard and mouse reports for the previous computer are submitted to
  Bluetooth before accepting input for the next computer, while the old link
  remains usable. Notifications have no application-level delivery acknowledgement.
- No input is deliberately replayed after a disconnected/unready period.
- Enter `release` for an emergency all-keys/all-buttons release.

The final arrow key of a switching chord is consumed. Ctrl/Alt presses may
already have reached the old computer before the chord can be recognized.
Unexpected radio loss prevents a guaranteed final release packet; the computer
also has to handle the disconnected HID device.

This first version provides ordinary keyboard keys/modifiers, up to six
non-modifier keys at once, five mouse buttons, relative X/Y motion, and a vertical
wheel. Media keys, vendor-specific functions, absolute-position devices,
horizontal scrolling, and keyboard LED synchronization back to physical input
devices are not promised. Six-key rollover is handled by releasing output and
waiting for physical keys to be released rather than inventing key state.
Input descriptor limits are 4 KB, 16 input reports, and three HID services per
device. Unsupported descriptor layouts (including global Push/Pop and revisited
Report IDs) are rejected explicitly, so this is not yet universal HID support.

## Pairing and administration

New PC pairing is closed at startup. `pair keyboard` or `pair on` temporarily
permits it; an
explicit input `connect` authorizes pairing that selected input peer.
Known bonded peers can re-encrypt without opening a new pairing window.

Keyboard-based computer enrollment requires passkey authentication and rejects
Just Works. Input devices and console-only enrollment can still use Just Works,
which does not provide man-in-the-middle protection. Pair only in a
trusted environment, check comparison numbers, and close the pairing window
after enrollment. Anyone with access to the USB console can administer the hub
and request input. This prototype has no console password.

`bonds` lists peer identities without printing cryptographic keys. To remove a
bond, disconnect all peers first (turn off computer Bluetooth temporarily if it
automatically reconnects), then use one of:

```text
unbond classic AA:BB:CC:DD:EE:FF
unbond public AA:BB:CC:DD:EE:FF
unbond random AA:BB:CC:DD:EE:FF
```

Also remove the Pico from that peer's paired-device list. If the HID descriptor
changes in a future firmware, remove and re-pair it on the computers to avoid
cached descriptor problems.

## What to record during hardware experiments

Save console output locally, especially:

- Firmware version and the input transport combination.
- Four ready computer slots, two ready inputs, and six HCI connection handles.
- Exact HCI command/connection/disconnection status codes.
- Whether inactive computer links stay connected during active typing/mousing.
- Repeated switching, held modifiers/buttons, input power-off, computer sleep,
  and reconnection behavior.

A seventh attempted peer may be briefly visible while being rejected. The host
memory pool has eight entries for such transient traffic; this is **not a claim
that the controller supports eight connections**. The application limits remain
two inputs and four computers.

If six-link operation fails, preserve the error codes before changing hardware:
host pool exhaustion, profile bugs, controller limits, and computer power policy
are different causes.

`[SMP ...] TX/RX` diagnostics show only pairing-request/response capability fields,
security-request flags, and failure reasons. They deliberately omit raw HCI
packets, passkeys, cryptographic keys, and keyboard/mouse report payloads.

Firmware 0.3.3 adds generic Classic pairing diagnostics without changing the
0.3.2 authentication policy or PIN submission behavior. `[PAIR ...]` lines
identify PIN requests, PIN command transmission/acknowledgement, authentication,
encryption, link-key creation (type only), and disconnect reasons. `[APP ...]`
records show request tickets, API return codes, and expired/revoked requests.
All timestamps are milliseconds since the Pico booted.

After a failed attempt, enter:

```text
diag
status
```

`diag` replays the latest 40 protocol/application events from RAM, oldest first,
even if no serial terminal was open when they happened. It is not a copy of all
console output. `status` also includes each input's last authentication, HID,
and disconnect statuses (`0xff` means that phase was not observed).
Read these before rebooting; the history deliberately is not written to Flash
and disappears on reboot. No PIN values, link keys, or typed HID input are added
to this diagnostic history. Existing manually entered serial commands can still
echo their arguments, so redact PIN commands before sharing a full console log.

`name <Bluetooth address>` is an optional read-only Classic device-name query
for identifying a discovered endpoint, independent of its manufacturer.

## Source layout

| Path | Responsibility |
| --- | --- |
| `src\main.c` | USB console, pairing authorization, HCI connection diagnostics |
| `src\hub_output.c`, `src\hub.gatt` | Four separate BLE HID computer contexts |
| `src\hub_output_store.c` | Atomic identity-slot and selected-computer persistence |
| `src\hub_input.c` | Two Classic/BLE input peers and HID report parsing |
| `src\hub_router.c` | Input aggregation, hotkeys, release/suppression rules |
| `src\hub_passkey.c` | Local six-digit pairing-code entry, never normal HID output |
| `src\hub_button.c`, `src\hub_bootsel.c` | Debounced enrollment/reset gestures and SRAM-safe BOOTSEL sampling |
| `include\btstack_config.h` | Fixed host pools and dual-mode stack features |
| `tests` | Native C router tests with mocked Bluetooth output |

The routing tests use a host C compiler, not the ARM toolchain. On Windows the
following script can install a small, pinned TinyCC compiler locally and run
the tests without Visual Studio:

```powershell
.\tools\test-router.ps1 -BootstrapCompiler
```

Subsequent runs need only `.\tools\test-router.ps1`. This compiler is used only
for host tests; firmware still uses Arm GNU. With an existing native compiler
and CMake on PATH, the tests can alternatively be run through CTest:

```powershell
cmake -S tests -B build\tests
cmake --build build\tests
ctest --test-dir build\tests --output-on-failure
```

Bluetooth hardware behavior is outside the scope of these host tests.
