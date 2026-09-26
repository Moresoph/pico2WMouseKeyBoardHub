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

The saved 0.4.0 checkpoint remains the functional baseline. Firmware 0.4.1 adds
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
