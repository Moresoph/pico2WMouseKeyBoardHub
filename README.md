# Pico 2 W Bluetooth keyboard/mouse hub

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

## Working milestone: 0.4.0 (2026-09-20)

The current hardware setup has demonstrated a Classic Level 0 keyboard and a
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

To reproduce this **specific working configuration**, build with:

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
kept out of Git. Further feature/performance work should build on this checkpoint.

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

The saved 0.4.0 checkpoint remains the functional baseline. Firmware 0.4.1 adds
on-demand measurements and a reversible performance mode:

```text
latency
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
Pauses longer than 100 ms are separated from active input-gap averages.
No key codes, movement values, or report payloads are logged by these metrics.

`latency fast` enables event-driven queue draining and requests a 7.5--15 ms LE
interval with zero peripheral latency for existing BLE input links and the
currently selected computer. The requested maximum is capped at the current
interval when it is already below 15 ms, so a 7.5 ms link is not asked to slow
down. The host/device can reject or choose different
parameters; the printed negotiated values, not the request, determine whether
radio timing changed. Inactive computers stay connected and are not accelerated;
a previously accelerated computer is requested back to its baseline when it is
no longer selected. Controller requests are bounded rather than retried in a loop.
Peers can later renegotiate their power-saving parameters; fast mode does not
continually override them. Peripheral latency permits skipping idle connection
events, not a fixed delay added to every moving-mouse report.

`latency normal` restores the baseline send scheduling and requests each tuned
link's earlier connection parameters. Neither mode changes encryption, bond
keys, slot selection, or input authorization. Mode selection is saved separately
from device identities and restored at reboot. With no saved mode, startup uses
normal mode so baseline measurements can be collected first.

For comparison, reset the counters, move the mouse continuously for the same
duration on the same selected computer, read `latency`, and repeat in the other
mode. Keep other computer links connected throughout. A shorter BLE interval
can increase radio duty cycle and mouse battery use; do not trade stable
multi-device operation for a requested interval the controller cannot sustain.

In a manual same-PC comparison, mean mouse queue-to-ATT submission time fell
from 6.345 ms (767 packets) to 1.437 ms (1,353 packets); the operator reported
more responsive motion with normal clicks and scrolling. PC1 remained at
15 ms and the mouse remained at 7.5 ms. The mouse accepted latency 0 initially,
then renegotiated latency 44. This is an indicative queue/scheduling result,
not an end-to-end latency measurement or proof of a radio-interval improvement.

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

The original `pair on` mode is still available without a connected keyboard.
It uses the console's display/confirmation workflow, including
`confirm <ticket> yes` only after comparing both six-digit displays, or
`passkey <ticket> <six digits>` when requested.

To isolate the output radio before adding input devices, `select 1`,
`tap 0x04`, and `move 20 0` explicitly send an A key and cursor motion to the
selected ready PC. Use a blank text editor. These commands are rejected while
physical input peers are connected, a switch is pending, or keyboard enrollment
is active; they never run automatically.

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
