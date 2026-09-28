# How the module uses SDRplay's API

What the module asks of SDRplay's API, and where each fact comes from. The
module was written from these facts; no code was copied from the projects
named here, and nothing from SDRplay's files is in this repository.

Sources:

- SDRplay API Specification v3.15 (41 pages, revision history ending
  "3.15, 10th May 2024, Added RSPdxR2 Support"),
  <https://sdrplay.com/wp-content/uploads/2026/04/SDRplay_API_Specification_v3.15.pdf>.
  Cited as "spec" with its section.
- SDRplay's Linux installer `SDRplay_RSP_API-Linux-3.15.2.run`, SHA-256
  `3a97ca76...82fe3`, from <https://www.sdrplay.com/api/>: its
  `install_lib.sh`, and the installed library and service, examined with
  `readelf`, `nm` and `strings` and by calling the API (no disassembly; the
  licence forbids it).
- The RSP1A datasheet v2.3, the RSPdx-R2 datasheet v1.1, the RSPduo
  Introduction V3 and SDRplay's "RSPdx's HDR Mode Usage" note.
- For comparison only: SoapySDRPlay3 `48bd8b4`, SDR++ `sdrplay_source`
  `8c9f5ee`, ka9q-radio `bb5ce03`.

"Measured" below means observed on this machine with API 3.15.2 installed
and no RSP attached (x86_64, Ubuntu 24.04).

## Loading the API

The API has two halves: a library the application loads and a service
(`sdrplay_apiService`, `sdrplay.service`) that owns the USB devices (spec
section 6). The installer puts the library at
`/usr/local/lib/libsdrplay_api.so.3.15` with the links `.so.3` and `.so`,
and runs `ldconfig`.

The module does not link the library. It calls
`dlopen("libsdrplay_api.so.3")`, then `libsdrplay_api.so`, then
`/usr/local/lib/libsdrplay_api.so.3`, with `RTLD_NOW`, and resolves the 14
functions it uses with `dlsym`; spec section 6 shows the same pattern for
Windows. A missing library or function is reported with the download page.
The library is never unloaded, since its threads may outlive
`sdrplay_api_Close`.

The types are declared in `src/rsp_api.h` from spec section 2.
`tests/abi_check.cpp` compares every size, offset and constant the module
uses with SDRplay's installed headers; with the 3.15 headers on x86_64 they
match (`DeviceT` 96 bytes, `DevParamsT` 64, `RxChannelParamsT` 144,
`StreamCbParamsT` 20). The layout changed between 3.07 and 3.14 (`DeviceT`
gained `valid`, `RspDuoTunerParamsT` gained `resetSlaveFlags`), so the
module calls `sdrplay_api_ApiVersion` and refuses anything but 3.14 and
3.15; the 3.14 and 3.15 headers differ only in the version and the RSPdx-R2
id. `sdrplay_api_InvalidServiceVersion` means library and service differ.

Measured: the library imports `shm_open`, process-shared robust
`pthread_mutex` and condition variables, `mmap` and `syslog`, and no socket
functions; the service imports `socket` and `connect` as well as libusb. With
the service running, `/dev/shm` holds `Glbl\sdrSrvCmdSema`,
`Glbl\sdrSrvComMtx`, `Glbl\sdrSrvComShMem` and `Glbl\sdrSrvRespSema`, mode
0666. With it stopped, `sdrplay_api_Open` fails at once with
`sdrplay_api_Fail` and the extended message "Could not open file mapping
object", and the library prints `shm_open: No such file or directory` on
stderr (fd 2, FernSDR's log). The library writes nothing to stdout.

## Opening an RSP

In this order (spec sections 1, 3 and 4):

1. `sdrplay_api_Open`. "The first function call must be to
   sdrplay_api_Open() and the last must be to sdrplay_api_Close() otherwise
   the service can be left in an unknown state" (spec 1).
2. `sdrplay_api_ApiVersion`, checked as above.
3. `sdrplay_api_LockDeviceApi`, then `sdrplay_api_GetDevices` with room for
   16 (`SDRPLAY_MAX_DEVICES`).
4. The device chosen by serial, index or as the only one. `hwVer` gives the
   model: 1 RSP1, 255 RSP1A, 6 RSP1B, 2 RSP2, 3 RSPduo, 4 RSPdx, 7 RSPdx-R2
   (spec 2.1.2). A device whose `valid` is 0 is not ready. For an RSPduo the
   module sets `tuner` to A (tuner 1) or B (tuner 2) and `rspDuoMode` to
   single tuner; an RSPduo that offers only slave mode is in use elsewhere.
   Everything that depends on the model is checked here, before the device
   is taken.
5. `sdrplay_api_SelectDevice`, then `sdrplay_api_UnlockDeviceApi`.
6. `sdrplay_api_GetDeviceParams`, which returns pointers into structures the
   API owns; the module writes its settings into them. The tuner's are in
   `rxChannelB` for RSPduo tuner 2 and `rxChannelA` otherwise.
7. `sdrplay_api_Init` with the stream and event callbacks.

Every failure after step 1 undoes what was done: unlock, `ReleaseDevice`,
`Close`, in that order.

`sdrplay_api_GetDevices` leaves out a device another process has selected
(spec 3.7: once selected, a device "is no longer available for other
applications"). The module counts USB devices with vendor id 1df7 in
`/sys/bus/usb/devices` (the installer's udev rule lists product ids 2500,
3000, 3010, 3020, 3030, 3050 and 3060) to tell an RSP in use from a missing
one; RSPs have no serial number in their USB descriptors, so only the count
is known.

### The lock

Spec 3.4: "Once locked, no other applications will be able to use the API."
Measured: while one process holds the lock, another process's
`sdrplay_api_ApiVersion` does not return, for at least 70 seconds; `Open`
still succeeds. When the holding process is killed with SIGKILL, the waiting
call returns at once and the next process gets the lock without delay
(the lock is a robust mutex, which fits the library's use of
`pthread_mutexattr_setrobust` and `pthread_mutex_consistent`). Killing a
process that waits inside a call left the API usable in every trial.

So the module holds the lock only from `GetDevices` to `SelectDevice`, and
releases it on every path before it closes the API. Every call runs under a
watchdog: the whole open gets 12 seconds (FernSDR wants `ready` within 15),
`--list-devices` 8 (FernSDR allows 10), a live change 3, and stopping 3.5
(FernSDR sends SIGKILL 4 seconds after stop; its SIGTERM at 2 only reaches
the module's signalfd). A call that outlives its deadline cannot be
taken back, so the module reports `busy` and exits, which frees the lock.

## Sample rate

Spec 2.3.2: `devParams->fsFreq.fsHz` is the converter rate, 2 MHz by
default. The datasheets give 2 to 10.66 MHz, 14 bits up to 6.048 MHz, 12 to
8.064, 10 to 9.216 and 8 above. The module sets:

- 2 to 10.66 MHz: `fsHz` = the band's rate, no decimation.
- below 2 MHz: `fsHz` = the rate times the smallest of 2, 4, 8, 16, 32 that
  reaches 2 MHz, with `ctrlParams.decimation` enabled at that factor and
  `wideBandSignal` set. The specification lists no decimation factors;
  SoapySDRPlay3 uses these, and sets `wideBandSignal` in zero IF.
- `if_mode = low`: `fsHz` 6 MHz, `ifType` 1620 kHz, `bwType` at most 1536
  kHz, one of the down-conversion cases of spec 3.15 ("(fsHz == 6000000) &&
  (bwType <= sdrplay_api_BW_1_536) && (ifType == sdrplay_api_IF_1_620)"),
  and decimation 1 to 32. SoapySDRPlay3 and SDR++ both treat this case as
  2 MHz out; the specification does not state the rate.

The filter `tunerParams.bwType` (values in kHz, spec 2.4.2) is the widest of
200, 300, 600, 1536, 5000, 6000, 7000, 8000 kHz that fits into the band's
rate; the API's default of 200 kHz would leave most of a 2 MHz band empty.

Because the delivered rate is partly inference, the module measures it from
0.5 to 2.5 seconds after the first samples and stops with an error if it is
more than 10 % from the announced one.

## Tuning

`tunerParams.rfFreq.rfHz`, a double, set before `Init` and changed live with
`Update_Tuner_Frf`. The API reports no tuned frequency; `ready.center` is the
value set. The tuning step and any residual offset are not documented.

## Gain

`tunerParams.gain.gRdB` is the IF gain reduction, 20 to 59 dB with
`minGr = NORMAL_MIN_GR` (spec 2.4, `MAX_BB_GR` 59); `LNAstate` indexes the
tables of spec section 5, which the module carries per model, input and
frequency band. The state counts match the header constants (RSP1A 10, 7 in
the AM band, 9 in L band; RSP2 9, 5 on Hi-Z, 6 at 420 MHz; RSPduo 10, 5, 7,
9; RSPdx 28, 19, 20, 25, 27, 21, 19, 22 in HDR), which a test checks. The
RSP1B's 7-state row ends at 50 MHz, the RSP1A's at 60. The RSP1's table is
not monotonic (its state 2 is mixer reduction only), so the ladder sorts by
total reduction.

`ctrlParams.agc.enable` defaults to `AGC_50HZ` (spec 2.5.2), so the module
sets `AGC_DISABLE` explicitly unless `gain = agc`, which uses
`AGC_CTRL_EN` ("Latest AGC scheme") with a set point of -30 dBFS, valid at
every rate (spec 2.5.3). Gain changes go through `Update_Tuner_Gr`, AGC
changes through `Update_Ctrl_Agc`.

## Switches

| Setting | RSP1A, RSP1B | RSP2 | RSPduo | RSPdx, RSPdx-R2 |
|---|---|---|---|---|
| bias tee | `rxChannel.rsp1aTunerParams.biasTEnable`, `Rsp1a_BiasTControl` | `rsp2TunerParams.biasTEnable`, `Rsp2_BiasTControl` | `rspDuoTunerParams.biasTEnable`, `RspDuo_BiasTControl` | `devParams.rspDxParams.biasTEnable`, Ext1 `RspDx_BiasTControl` |
| broadcast notch | `devParams.rsp1aParams.rfNotchEnable`, `Rsp1a_RfNotchControl` | `rsp2TunerParams.rfNotchEnable`, `Rsp2_RfNotchControl` | `rspDuoTunerParams.rfNotchEnable`, `RspDuo_RfNotchControl` | `rspDxParams.rfNotchEnable`, Ext1 `RspDx_RfNotchControl` |
| DAB notch | `rsp1aParams.rfDabNotchEnable`, `Rsp1a_RfDabNotchControl` | none | `rfDabNotchEnable`, `RspDuo_RfDabNotchControl` | `rfDabNotchEnable`, Ext1 `RspDx_RfDabNotchControl` |
| input | one | `antennaSel` A=5, B=6; `amPortSel` AMPORT_1 for Hi-Z | tuner A/B at selection; `tuner1AmPortSel` AMPORT_1 for Hi-Z | `rspDxParams.antennaSel` A=0, B=1, C=2 |

The RSP1B takes the RSP1A's structures and flags, the RSPdx-R2 the RSPdx's
(spec 1). The RSPduo's MW notch is `tuner1AmNotchEnable`
(`RspDuo_Tuner1AmNotchControl`). Where the bias tee sits comes from the
datasheets and the RSPduo introduction (tuner 2; RSPdx-R2 antenna B). The
Hi-Z inputs' tables end at 60 MHz, and the RSPdx-R2 datasheet gives antenna
C 1 kHz to 200 MHz; the module refuses a centre beyond either. HDR is
`rspDxParams.hdrEnable`, which works only at the centres spec 3.15 lists.
`ppm` is `devParams.ppm` (`Dev_Ppm`), DC and I/Q correction
`ctrlParams.dcOffset` (`Ctrl_DCoffsetIQimbalance`).

The module leaves `loMode`, `dcOffsetTuner`, `mode` (isochronous USB) and the
reference clock outputs at the API's defaults. Two errata of spec 3.17,
found by comparing it with the headers: the descriptions of
`Update_Tuner_DcOffset` and `Update_Tuner_LoMode` are swapped, and
`Update_RspDx_HdrBw` names `devParams` where the field is in `rxChannel`.

## Streaming

The stream callback (spec 3.21) gets I and Q as separate `short` arrays;
the module interleaves them into little-endian s16 pairs, copies them into a
lock-free ring and returns. It measures the peak and counts a sample as
clipped when I or Q reaches 32512 (the specification does not say how the
converter's 14 to 8 bits sit in the 16, so the threshold allows for the
coarsest). A writer thread empties the ring into fd 1. Nothing reaches fd 1
before `ready`.

`StreamCbParamsT.firstSampleNum` numbers the samples (32 bits); the
specification does not define it. The module expects each callback to start
where the last ended and counts a jump forward as samples the API lost, as
ka9q-radio does. Whether the number counts delivered samples, samples before
decimation, or in low IF samples at the 6 MHz converter rate is not
documented, so the module learns the step per delivered sample: 1, 3, the
decimation or three times it, once two pairs of callbacks in a row agree.
Pairs seen while learning are counted once the step is known, so a gap
between the first callbacks is not lost. A callback with `reset` set
restarts the numbering on purpose, and the step is learnt again.

The event callback (spec 3.22) records and returns: `PowerOverloadChange`
marks the samples until `Overload_Corrected` as clipped, and every message,
detected or corrected, is acknowledged with `Update_Ctrl_OverloadMsgAck`,
as the specification's example does. The acknowledgement comes from the
session's thread within 100 ms rather than from the callback, so that all
calls into the API come from one thread. `DeviceRemoved` and
`DeviceFailure` end the module with `lost`, as does a stream that stops for
1.5 seconds.

## Stopping

`sdrplay_api_Uninit`, which ends the callbacks, then `ReleaseDevice` and
`Close`, all within 3.5 seconds (spec 3.16 and 3.8), half a second short of
FernSDR's SIGKILL. `StopPending` concerns
an RSPduo master with a running slave, which this module never is.

## Not verified without an RSP

- Whether `firstSampleNum` counts before or after decimation, and whether
  gaps in it match real losses.
- The rates the API delivers in low IF and with decimation; the rate check
  guards against a wrong assumption.
- The level at which samples clip, and whether the API's overload events
  come before the samples clip.
- The tuning step and residual offset against FernSDR's 10 Hz rule.
- The gain tables and the ladder in practice, HDR, antenna switching, bias
  tee and notches on each model.
- A reported SoapySDRPlay3 comment that callbacks stop when the rate crosses
  2,685,312 Hz while streaming; the module never changes the rate while
  streaming, and the stall check would catch it.
- What the service does when a module is killed while it holds a selected
  RSP.
