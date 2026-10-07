# Native Hanshow Q-N profiles on Telink

The native target driver supports Pro-154Q-N C3HW2 / HSEL4Q_01_54M_39
(200 × 200, panel 1035) and Pro-266Q-N E3LW2 / HSEL7_02_66M_30
(152 × 296, panel 1036). These names identify observed hardware profiles;
they do not establish a commercial controller part number or compatibility with
other flex/PCB revisions. Scoped hardware qualification is recorded solely in
[the hardware checklist](HARDWARE_VERIFICATION_CHECKLIST.md#native-hanshow-q-n--pro-266q-n-2026-10-05).
The [Pro-154Q-N record](HARDWARE_VERIFICATION_CHECKLIST.md#native-hanshow-q-n--pro-154q-n-2026-10-07)
has its separate specimen and build scope.

`src/epd/hanshow_qn.c` uses the target's existing configured GPIO, SPI,
stack-service, uptime, and transfer hooks. It owns panel GPIO initialization and
shutdown. The caller owns the stay-awake hold and supplies a cancellation callback
for its current BLE connection epoch; boot rendering cancels when a client connects so interrogation does not wait for both refreshes. The callback
runs throughout reset, waveform loading, image transfer, and BUSY waits.

The host maps logical indices to drive codes and transmits complete upper-two-bit
and lower-two-bit components, each in native panel order: transpose for 1035,
reversed rows for 1036. The driver writes
those bytes directly to controller storage. The first component refreshes at the
exact byte boundary, followed by shutdown, power release, reset, and fresh setup
for the second component. Finish validates complete input, refreshes component 1,
and shuts down. Invalid offsets, extra bytes, incomplete finish, cancellation, or
BUSY timeout abort and release panel GPIO. Sixteen accepted drive codes do not
establish sixteen calibrated optical colors; RGB mappings are provisional.

Host RGB conversion offers the existing `observed7` default and an opt-in
`experimental12` profile for panel 1036. The latter adds nominal pink, khaki,
middle-gray, salmon and mauve estimates from the earlier scene experiment.
Its compact dither indices map into the unchanged sixteen logical slots;
preview RGB and transmitted drive codes stay paired. Shared host metadata lives
in `opendisplay-protocol/tests/fixtures/hanshow-rgb-palettes.json`. This adds no
firmware buffers, waveform bytes, protocol fields or color-mode identities.
Panel 1035 retains the seven-state RGB default pending further qualification.

## Controller temperature telemetry

Q-N image startup performs a bounded low-voltage temperature probe before waveform
initialization. A disconnected startup also probes once when boot graphics are
suppressed. The helper uses configured panel pins and existing GPIO/SPI/awake
ownership, waits 110 ms for supply settling, resets, checks the captured revision
response, and requires a fresh BUSY assertion/completion for command `0x40` within
400 ms. It decodes signed whole degrees plus quarter degrees and rejects readings
outside −25 to 50 °C. A recognized response does not establish a controller part
number or sensor calibration.

Successful probes populate existing OpenDisplay temperature telemetry; standard
MSD and `READ_MSD` expose whole degrees truncated toward zero. The C helper returns
centidegrees. Read-only telemetry reports the cached last successful probe; it does
not trigger a fresh conversion or expose its age. The standalone probe sends no
waveform, high-voltage power-on, image or refresh command. Failed probes preserve
the prior successful telemetry and do not change admission of the owner-selected
fixed waveform pair. No automatic band selection or thermal interlock is implied.

## Waveform provisioning

The source tree contains no factory waveform bytes. The default weak provider
returns unavailable, so Q-N admission fails before panel GPIO is initialized.
A private generated provider can override it. The provider holds one explicitly
chosen pair for each panel and retains it unchanged across both passes. It does
not inspect a fabricated temperature reading, automatically select a band, or
assert that manufacturer descriptor limits have been qualified on this driver.

Prepare a private JSON manifest outside the repository, for example:

```json
{
  "profiles": [
    {"panel": "154", "band": 6, "temperature_c": 24.5,
     "records": ["154-pass0.bin", "154-pass1.bin"]},
    {"panel": "266", "band": 6, "temperature_c": 24.5,
     "backup": "266-factory-backup.bin"}
  ]
}
```

For panel 266, omitted `waveform_family` selects the existing `0cnspq1tl`
family and band 6. Select the distinct 404545 family explicitly:

```json
{
  "profiles": [
    {"panel": "266", "waveform_family": "0enspq1yq", "band": 5,
     "temperature_c": 24.5,
     "records": ["404545-pass0.bin", "404545-pass1.bin"]}
  ]
}
```

Each `records` input must be a complete recovered record, in component order:
546 bytes for 154 or `0cnspq1tl`, and 556 bytes for `0enspq1yq`.
Alternatively `backup` must be the owner's exact 512 KiB factory backup;
only the two selected records at the established descriptor addresses are read
into the output. Record stride follows the explicit family. Each record's
SHA-256, length and complete command extension must match the
selected captured profile. Paths resolve against the manifest directory.

```bash
python3 targets/telink-tlsr/tools/provision_hanshow_waveforms.py \
  --manifest /private/owner/manifest.json --output /tmp/hanshow-private-waveforms
```

The output is a fresh directory with owner-only permissions, containing
`hanshow_waveforms.c` and a provenance manifest. Output inside the repository is
refused, and earlier output is never overwritten. Configure a private build with
that generated provider source via
`-DOD_HANSHOW_WAVEFORM_SOURCE=/tmp/hanshow-private-waveforms/hanshow_waveforms.c`;
keep the provider and waveform-bearing binaries
private while redistribution rights remain unresolved. The published source,
provisioning tools, and fingerprints do not grant rights to distribute recovered
manufacturer data. Omitting the input manifest generates an unavailable provider.

Supported explicit selections are 154 band 6 (bench 24.00–25.00 °C), 154 band 7
(25.01–30.00 °C), 266 `0cnspq1tl` band 6 (24.00–25.00 °C), and 266
`0enspq1yq` band 5 (20.01–25.00 °C at the manifest's centidegree precision).
The captured `0enspq1yq` descriptor selects band 5 when 20 < T ≤ 25 °C;
this is a selection rule, not hardware qualification. Those bounds describe captured
profile selection, not validated ambient operating ranges. The provisioning
manifest records the owner's bench temperature; this firmware has no automatic
thermal interlock. Operation outside the manually qualified condition must wait
for a temperature-sensor and band-selection qualification change.

The `0enspq1yq` provider retains each pass's `06`, `60` and `65` command
extension values in a fixed initializer structure. The native driver emits them
once at their normal register positions. This family uses `50:97` on pass 1
and `00:27a9` before power-off; it does not add a command executor, new wire
format or image storage. The `0cnspq1tl` default and 154 register behavior
remain unchanged. Explicit `0enspq1yq` selections for 154 are refused.

## Resource budgets

No image framebuffer, retained component, replay storage, or allocation is used.
The driver state is one pair pointer, one cancellation function pointer, two
32-bit counters, two 16-bit dimensions, and two booleans (24 bytes under the
32-bit ABI; inspect the target map for actual placement/alignment). SPI output
uses the existing byte helper and tiny register command arrays; it allocates no
line buffer. Private const waveforms cost 1,070 bytes per admitted panel plus
provider metadata. Both panels cost 2,140 waveform bytes plus metadata. All
other bands stay outside the binary.
The 404545 family adds two 13-byte initializer structures and 12 bytes to the
provider structure under the 32-bit ABI, for 38 bytes of additional const
metadata. Driver state and image-buffer requirements stay unchanged.

BUSY is active low / ready high. Setup readiness is bounded by 400 ms,
power-on by 2 seconds, each refresh by 200 seconds, and power-off by 400 ms.
The busy loops service BLE and advance the target's extended uptime even during
boot. Existing stack service feeds the watchdog. START/DATA/END and final refresh
notification client budgets must each admit the canonical 215-second window.
The 32 KiB profile limits configuration assembly to 1,536 bytes (larger START
requests are NACKed), uses RX slots 3, link RX/TX FIFO counts 4, inflate scratch
64 bytes, and a 128-byte boot row. The ordinary 64 KiB profile retains its
existing queue/configuration sizes. A 30-second Q-N input-idle interval after a
completed write tears down through `od_core_reset`; an encrypted client must
establish a fresh session before retrying. Time spent in a blocking refresh does
not count against that input-idle interval.

The 32 KiB profile advertises ATT Write Request and notifications on the
OpenDisplay characteristic, omitting Write Without Response
(`OD_TLSR_ATT_WRITE_COMMAND=0`). Hosts must select the advertised write capability;
each request still receives the ordinary OpenDisplay application acknowledgment.
The 64 KiB profile retains both write properties. Bench comparison on the
Pro-266Q-N observed a stall after an early raw DATA chunk with Write Without
Response. This does not establish the underlying SDK failure. The checklist
records the tested Write Request workflows; Write Without Response remains
unqualified on the bounded profile.

Measured software resource budgets using the pinned tc32 toolchain. The first
five rows are retained builds without the Q-N temperature probe; the final three
include temperature telemetry and corrected 154 boot ordering:

| Build | Occupied RAM | Stack + reserve | Finished image |
|---|---:|---:|---:|
| 32 KiB, no waveforms | 28,940 B | 3,828 B | 109,892 B |
| 32 KiB, both private band-6 pairs | 28,940 B | 3,828 B | 112,100 B |
| 32 KiB, private 154 band 6 + 266 `0enspq1yq` band 5 | 28,940 B | 3,828 B | 112,388 B |
| 64 KiB, no waveforms | 35,348 B | 30,188 B | 109,860 B |
| 64 KiB, private 154 band 6 + 266 `0enspq1yq` band 5 | 35,348 B | 30,188 B | 112,356 B |
| 32 KiB, private 154 band 6, temperature probe | 28,940 B | 3,828 B | 111,860 B |
| 32 KiB, no waveforms, temperature probe | 28,940 B | 3,828 B | 110,740 B |
| 64 KiB, no waveforms, temperature probe | 35,348 B | 30,188 B | 110,708 B |

The explicit 404545 candidate adds 288 bytes to the prior 32 KiB private image
with no increase in occupied RAM. These linked software budgets do not measure
runtime stack high-water or establish optical performance.

The occupancy includes copied RAM code, initialization data, BSS, address gaps,
and the 384-byte IRQ stack. Link/post-build gates reserve at least 3,072 bytes for
main stack plus 512 bytes of margin. tc32 prologue inspection found approximately
2,260 bytes on the QR generation path, including its dynamic arrays; this is a
partial static bound, not a measured high-water value or a proof about all SDK
callbacks. The panel-driver fixed frames are 8–32 bytes, and a boot pass-boundary
path has about 1,100 bytes before SDK descendants. Measure full target stack
high-water and BLE latency before release; these source budgets are not hardware measurements.

## Installation and OTA

The native image starts at flash address zero; the alternate 192 KiB OTA bank
occupies `0x40000–0x6ffff`. Telink's SDK initialization can erase that alternate
bank. The OTA service is therefore initialized once per boot, only when the
existing authenticated `ENTER_DFU` command arms it. Ordinary boot and reconnect
preserve the alternate bank; explicitly entering DFU claims it for an update and
can erase factory data or a recovery image stored there. An existing bootable
alternate image must be handled separately before a wired installation.

The SDK erase helper clears the watchdog before each sector, but initialization
and cleanup are synchronous and do not service the BLE stack between sectors.
Latency for the first DFU acknowledgment, connection survival during cleanup,
and subsequent OTA updates remain unqualified on Q-N hardware. Repeated arming
does not reinitialize the SDK or erase an upload already in progress. Linked-code
inspection confirms initialization is absent from normal boot and reachable from
the guarded arming hook; target builds verify the resulting images.

Native configuration storage uses both sectors `0x7a000` and `0x7b000`. A first
configuration can omit the display and set every unused power/ADC pin to `0xff`
to verify storage without driving the panel. For the qualified display profile,
`transmission_modes=0x8b` enables raw, ZIP and direct transfer while suppressing
the boot screen. The canonical `CLEAR_ON_BOOT` bit (`0x80`) makes this target
skip boot drawing. A first configuration write reloads configuration without
scheduling a boot screen. Verify the complete configuration by reading it back
and checking it again after reboot; the flash HAL does not independently verify
each erase/program operation before acknowledging a write.

The ordinary reboot command schedules reset through the SDK main loop. BLE
service continues for at least 100 ms, then reset waits for the SDK TX FIFO to
drain, with a one-second ceiling if transmission stalls. Application dispatch
pauses during that interval so a new panel operation cannot block reset. No
application-level reboot acknowledgment is added; the delay allows the existing
ATT Write Response to leave before disconnect. Tested response delivery is scoped
in the hardware checklist; other devices and wireless OTA still need validation.

## Evidence and licensing

Register/pin facts and recovery fingerprints were inspected read-only in the
local `openepaperlink` hardware reference and `opendisplay/tools/panel_profiles.py`.
The reference `atc-review/panel.c` reports reset sequencing, the 154/266-specific
analog preamble/booster/F0 values, controller component storage, and refresh/shutdown
order. The new driver and provisioning tool were independently authored within
OpenDisplay's architecture; no external application, framework, or factory byte
arrays were copied into tracked source. The reference's code licensing and waveform redistribution
rights do not become rights to ship recovered waveform data.

The default 266 profile uses the 546-byte `0CNSPQ1TL` records associated with
the owner's 704542/E3LW2 family. The explicit 404545/E3LW2 `0ENSPQ1YQ`
selection admits its distinct 556-byte records and command extensions.
Both families use Hanshow stock driver 24, so the driver number alone cannot
select their waveform banks. Hash mismatch is a refusal, not a fallback to another waveform.
A matching record establishes byte provenance, not that another specimen has the
same glass or that a waveform is safe at every temperature. The flexible ribbon
marking is further physical evidence needed when qualifying additional revisions.

## Validation stages

1. Run synthetic driver trace tests for both panels, reset/init commands, all
   byte boundaries, disconnection, timeout, extra bytes, and incomplete END.
2. Run the provisioner's synthetic tests (no recovered data required):
   `python3 -m unittest discover -s targets/telink-tlsr/tools -p 'test_provision_hanshow_waveforms.py'`.
3. Build both 64 KiB and bounded 32 KiB targets, plus shared regression gates;
   verify map/stack and the 192 KiB padded OTA-bank ceiling.
4. After separate hardware authorization, verify GPIO/SPI/BUSY behavior, recovery,
   asymmetric orientation patterns, all sixteen drive codes, two-pass timing,
   interruption recovery, temperature conditions, power, and optical persistence.
5. Measure calibrated RGB mappings and broaden temperature bands independently.
   Mark hardware checklist rows only for tests actually performed.

## Implementation verification record

Source base: `g4bri3lDev/Firmware_Unified`, `feat/telink-tlsr`,
`a5344b401dd8e38d3510221130d59ecdb27bc239`. Telink dependencies are the existing
SDK `78f5e28b85dc6dcdc37f1904364a1203c97b7dfc` and Linux tc32
`8ec375bb95ef63b6d0a4b18008913c03e7928710`; optimization stays `-O2`.
Canonical protocol additions are authored in `OpenDisplay/opendisplay-protocol`,
and its headers are synchronized through its existing tool. Mode 10 is a
transport identity rather than an epaper-dithering optical palette; the latter's
parity script records that explicit exemption. Python and website fixtures use
the same canonical JSON byte for byte.

The final GCC suite passed 87 tests and the Clang sanitizer suite passed 92 tests
(including five fuzz smoke tests). Host tests include both physical sizes, arbitrary DATA boundaries, the actual
64-byte inflater pump across the first refresh boundary, exact LUT lengths using
synthetic data, geometry/scheme refusal, missing-provider refusal, nonsequential
writes, incomplete END, excess bytes, fast/PIPE refusal, BUSY timeout,
disconnection, input-idle teardown, bounded boot rows, and boot cancellation on
new connection. Existing UC/SSD/TI panel traces and shared transfer regressions
remain in the suites. A minimal unsigned-promotion fix in `od_aes_modes.c` allows
the existing AES tests to compile under the strict GCC host gate.

The complete `tools/check.sh --targets` run passed 43 checks, including GCC and
Clang host suites, ASan/UBSan, five 60-second fuzz campaigns, boundary/ownership
ratchets, and the pinned Python wire corpus. Three other-family builds could not
run: ESP-IDF, NCS v3.3.1, and the configured Silicon Labs build tools (including its Ninja path) are unavailable on this
machine. These are unverified builds, not passed gates. The sandbox's
LeakSanitizer tracer failure was resolved by rerunning the unchanged gate outside
the sandbox. Telink's 32/64 KiB and private waveform builds passed separately,
including KNLT/CRC, jump-table, RAM and image-size audits. These are software
checks; subsequent authorized hardware results are recorded in the checklist.
Nothing was pushed or published.

## Further hardware qualification

The checklist records completed checks and their specimen/build scope. The
following stages define remaining qualification work rather than additional
hardware evidence.

| Stage | Evidence required before proceeding |
|---|---|
| Identify | Photograph specimen PCB and flex markings; establish MCU marking, actual SRAM/flash, factory backup provenance and matching record hashes. Confirm the configured pins against each board. |
| Electrical setup | At the provisioned bench temperature, capture supply/reset/SPI/BUSY; confirm active-low enable, ready-high BUSY, dimensions and 535-byte LUT load. Verify safe shutdown and current with no retained supply leakage. |
| Stream and orientation | Capture component lengths, native reversed rows, upper/lower pair ordering, boundary refresh/reset and final END refresh. Use an asymmetric numbered/corner image and exact packed4 input. Verify no replay or retained MCU plane is needed. |
| Optical states | Test all 16 drive codes individually and preserve photographs/temperature/history. Record accepted electrical codes separately from stable optical states; measure the seven provisional RGB states with calibrated equipment before updating palettes. |
| Workflows | Both panels: RGB and exact inputs; plaintext/encrypted; raw/compressed (including all compressed input in START); Python API/CLI and website Tester/Designer; configuration read/write, reconnect and boot screen. Recheck an existing BWR/BWY Telink device on the 64 KiB profile. |
| Fault recovery | Disconnect during initialization, first component, first BUSY wait, boundary initialization, second component and final refresh. Test incomplete/extra input, refused fast/partial/PIPE, stuck BUSY, idle teardown and a fresh complete upload after every interruption. |
| Resource and durability | Paint/measure main and IRQ stack high-water under QR boot, compressed upload and encrypted BLE; monitor queue occupancy, watchdog service and ACK/ATT latency. Measure full refresh duration, energy, pigment persistence and repeated full cycles at the qualified temperature. |

Automatic waveform band selection, calibrated 16-color RGB mappings,
new PCB/flex variants and distribution of manufacturer waveform bytes remain
separate qualification and rights decisions. No generic controller identity or
unmeasured temperature range is inferred from these two named profiles.
