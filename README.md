# Fern-SDRPlay

Fern-SDRPlay lets [FernSDR](https://github.com/Steven9101/websdr) receive with
an SDRplay RSP. It is an input module: a separate program that FernSDR starts
for a band and talks to over pipes, as FernSDR's `docs/MODULES.md` describes.
FernSDR sends the band's settings; the module opens the RSP through SDRplay's
API, tunes it, and writes its 16-bit I/Q samples to FernSDR. It reports
statistics once a second, clipping among them, sets the gain itself unless
told otherwise, and takes gain, notch, bias tee and frequency correction
changes while it runs.

Unlike FernSDR's other modules, this one cannot carry its driver: an RSP is
driven through SDRplay's API, a proprietary library and service that only
SDRplay may hand out. The operator installs it from SDRplay first; the module
loads it when a band starts.

## Hardware

| Model | Inputs (`module.antenna`) | Bias tee | Notches |
|---|---|---|---|
| RSP1 | one | no | none |
| RSP1A, RSP1B | one | yes | broadcast (FM/MW), DAB |
| RSP2, RSP2pro | `a`, `b`, `hiz` (up to 60 MHz) | on `a` and `b` | broadcast |
| RSPduo | `tuner1`, `tuner2`, `hiz` (tuner 1's Hi-Z input, up to 60 MHz) | on `tuner2` | broadcast, DAB, MW on tuner 1 |
| RSPdx, RSPdx-R2 | `a`, `b`, `c` (up to 200 MHz) | on `b` | broadcast, DAB |

Every model tunes from 1 kHz (the RSP1 from 10 kHz) to 2 GHz and delivers
from 62500 to 10660000 samples a second. The RSPdx and RSPdx-R2 also have
HDR mode below 2 MHz. The RSPduo runs as one tuner; its dual-tuner and
master/slave modes are not supported yet.

## Installing SDRplay's API

The module needs SDRplay API 3.15 (3.14 also works) for Linux, from
<https://www.sdrplay.com/api/>. Download "API 3.15 Linux" there, which gives
`SDRplay_RSP_API-Linux-3.15.2.run`; its SHA-256 is
`3a97ca764263bbe76fb0f2220e6408942357e8864c19e1408a6d6987af382fe3`. Then:

```sh
sha256sum SDRplay_RSP_API-Linux-3.15.2.run
chmod +x SDRplay_RSP_API-Linux-3.15.2.run
sudo ./SDRplay_RSP_API-Linux-3.15.2.run
```

The installer shows SDRplay's licence and installs only once you accept it
(type `y`). It puts the library in `/usr/local/lib`, the service in
`/opt/sdrplay_api`, a udev rule that lets every user open an RSP, and the
systemd unit `sdrplay.service`, which it starts. It supports amd64, arm64
and armhf, and asks `dpkg` which one it runs on, so it expects a Debian-like
system. Check the service, and restart it after plugging in an RSP if the
module does not see it:

```sh
systemctl status sdrplay
sudo systemctl restart sdrplay
```

SDRplay's licence allows its service to send details of the radio devices it
drives to an SDRplay server (section 6). The module itself sends nothing
anywhere.

## Using it with FernSDR

Install the package for your machine from the admin panel, or from a shell:

```sh
sudo -u fernsdr fernsdr --install-module sdrplay-0.1.0-linux-x86_64.fernmod fernsdr.conf
```

Then give a band `source = module`:

```ini
[band:40m]
name                  = 40 m
source                = module
module                = sdrplay
sample_rate           = 2000000
center                = 7100000
signal                = iq
module.device         = serial:2305078C35
module.antenna        = a
module.gain           = auto
module.rf_notch       = yes
```

`fern-sdrplay --list-devices` prints the RSPs the API offers, with their
serial numbers. With only one RSP plugged in, `module.device` can be left
out.

## Settings

`fern-sdrplay --describe` lists them with their help texts; FernSDR's admin
panel shows the same.

| Setting | Default | Live | |
|---|---|---|---|
| `device` | empty | no | `serial:<serial>`, or `index:<n>`; empty for the only RSP |
| `antenna` | `auto` | no | `a`, `b`, `c`, `hiz`, `tuner1`, `tuner2` as the model has them (see Hardware) |
| `gain` | `auto` | yes | `auto`, `manual` or `agc` |
| `lna_state` | | yes | with `manual` or `agc`: 0 (the most gain) up to the model's last state |
| `if_gain_reduction` | | yes | with `manual`: 20 (the most gain) to 59 dB |
| `bias_tee` | no | yes | DC on the antenna input |
| `rf_notch` | no | yes | the FM and MW broadcast notch |
| `dab_notch` | no | yes | the DAB notch |
| `am_notch` | no | yes | the RSPduo's MW notch on tuner 1 |
| `hdr` | no | no | RSPdx HDR mode, at the centres listed below |
| `ppm` | 0 | yes | reference error, -1000 to 1000 ppm |
| `dc_correction` | yes | yes | the API's DC offset removal |
| `iq_correction` | yes | yes | the API's I/Q imbalance correction |
| `if_mode` | `zero` | no | `zero`, or `low` for a 1.62 MHz IF without a centre spike |
| `bandwidth` | `auto` | no | the IF filter in kHz: 200, 300, 600, 1536, 5000, 6000, 7000 or 8000 |

The band's `sample_rate` decides how the API samples: from 2 to 10.66 MHz
the converter runs at that rate; below, it runs at 2, 4, 8, 16 or 32 times
it and the API decimates. With `if_mode = low` the converter runs at 6 MHz
with the IF at 1.62 MHz and the API delivers 2 MHz, or 1 MHz, 500, 250, 125
or 62.5 kHz after decimation; other rates are refused. The IF filter is the
widest that fits into the band unless `bandwidth` says otherwise. HDR works
only with the band's `center` at 135000, 175000, 220000, 250000, 340000,
475000, 516000, 875000, 1125000 or 1900000 Hz.

### Gain

An RSP reduces gain in two places: the LNA state, whose steps depend on the
model, the input and the frequency (from 4 states on the RSP1 to 28 on an
RSPdx above 250 MHz), and the IF gain reduction, 20 to 59 dB.

With `gain = auto` the module walks a ladder of settings about 3 dB apart,
from the most reduction the RSP has to the least. At each step the LNA keeps
as much gain as it can while the IF reduction stays at or under 40 dB, since
the LNA's gain sets the noise figure. The module starts in the middle and
behaves like FernSDR's RTL-SDR and RX-888 modules: it comes down a step
when three tenths of a second of the last second each clipped more than one
sample in 10,000, 6 dB at once when they clipped more than one in 100, and
goes up a step when little but the odd crash has clipped for 5 seconds (a
minute, after the first two minutes) and the peaks would stay 6 dB under
full scale one step higher. A sample counts as clipped when I or Q reaches
32512 of 32767, and every sample counts as clipped while the API reports an
overload of the converter.

`gain = manual` keeps `lna_state` and `if_gain_reduction`; left out, they
start where `auto` would. `gain = agc` hands the IF gain to the API's own
AGC (set point -30 dBFS) and keeps the LNA state; it reacts within
milliseconds and pumps with strong signals, which is why the module keeps it
off unless asked. FernSDR's S-meter calibration holds at the gain it was
made at, so a calibrated band wants `manual`.

### What the module reports

- `ready` gives `format` `s16`, `signal` `iq`, the band's own `sample_rate`,
  and as `center` the frequency the RSP was set to: the API reports no tuned
  frequency of its own. `settings` adds the effective values, among them
  `gain_reduction` (LNA and IF together), `lna_states`, `converter_rate` and
  `decimation`.
- `stats` carry `dropped`: samples the API skipped, from gaps in its sample
  numbers, plus samples FernSDR did not read in time. `clipping` is the share
  of clipped samples, and with `gain = auto`, `gain` is minus the gain
  reduction in dB, 0 being the RSP's most gain.
- Each overload message from the API is acknowledged, as the API expects,
  and logged.
- The RSP unplugged, a failure the API reports, or no samples for 1.5
  seconds end the module with `lost` (status 5), and FernSDR starts it again.
- Two and a half seconds after the first samples, the module compares the
  rate the API delivers with the one it announced and stops if they differ
  by more than 10 %.

## Several RSPs

FernSDR runs one module process per band, and each opens one RSP. Give every
band `module.device = serial:<serial>`: an index is the position in the
API's list, and that list leaves out every RSP another process holds, so an
index can point at a different RSP depending on which bands already run.

The module counts the RSPs on the USB bus (vendor id 1df7) to tell an RSP in
use elsewhere from a missing one. It refuses to pick "the only RSP" when
another is plugged in but held elsewhere, and reports `busy` (status 4) when
the RSP it wants is in use.

While one process holds the API's lock, every call of every other process
into the API waits, even the version check; the module holds the lock only
from listing the devices to selecting one, releases it on every path, and
gives the API 12 seconds to open an RSP (8 for `--list-devices`) before it
reports `busy` and exits. A process that dies holding the lock frees it at
once.

## Containers and sandboxes

The API library talks to the `sdrplay` service through shared memory in
`/dev/shm` and process-shared mutexes; it opens no sockets and no devices.
FernSDR's systemd unit shares `/dev/shm` with the host, and the module works
inside it (tested as an unprivileged user with the unit's sandbox options).
In a container, run the service in the same container or share the host's
IPC namespace and `/dev/shm` (`--ipc=host`).

## Not supported yet

- RSPduo dual-tuner and master/slave modes.
- Packages for aarch64 and armhf; only x86_64 is built.
- The RSP2 and RSPduo reference clock output, the API's extended gain range,
  LO mode, USB bulk mode, AGC tuning and the HDR bandwidth.
- Changing the centre while a band runs: FernSDR restarts a band for that.

No RSP was attached while this version was written. It follows SDRplay's API
Specification 3.15 and was tested against a fake of the API and against the
installed API 3.15 without hardware; `docs/PROTOCOL.md` says which facts come
from where and which still need an RSP to confirm.

## Building

```sh
make                  # build/fern-sdrplay with this machine's compiler
make test             # ABI check, unit, protocol, command line and package tests
make test-asan        # the unit and protocol tests under AddressSanitizer and UBSan
make package          # dist/sdrplay-0.1.0-linux-x86_64.fernmod, built in Debian 10
make package NATIVE=1 # the same with this machine's compiler and glibc
```

The module is a dynamically linked executable: SDRplay's library needs glibc
and libstdc++, and cannot be loaded into a static program reliably. It does
not link the library; it loads `libsdrplay_api.so.3` at run time and uses
declarations of the API's types written from the specification, so building
needs nothing from SDRplay. `make abi-check`, part of `make test`, compares
those declarations field by field with SDRplay's headers where the API is
installed. `make package` builds in a Debian 10 container (GCC 8, glibc 2.28)
and needs docker; the result needs glibc 2.14 and libstdc++ from GCC 6.1 or
later, and runs on Debian 10, Ubuntu 18.04 and newer.

The tests load a fake `libsdrplay_api.so` built from `tests/`, which only
test builds of the program accept (`FERN_SDRPLAY_LIBRARY`); release builds
load SDRplay's library only.

## Licence

GPL-2.0-or-later, with an additional permission to load and use SDRplay's
proprietary API library at run time (SPDX:
`GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception`). The
permission reads, in full:

> As a special exception, the copyright holders of Fern-SDRPlay give you
> permission to combine Fern-SDRPlay with SDRplay Limited's proprietary
> SDRplay API library (libsdrplay_api, in any version, together with the
> service it communicates with), by loading that library into the same
> process at run time, and to copy and distribute the resulting combination,
> even though the SDRplay API library is not licensed under the GNU General
> Public License. You must still follow the GNU General Public License in
> all respects for all of the code used other than the SDRplay API library.
>
> This permission covers the SDRplay API library only. It does not permit
> combining Fern-SDRPlay with any other code that is not compatible with the
> GNU General Public License, and it grants no right in the SDRplay API
> library itself: whether and how that library may be copied is governed by
> SDRplay's own licence, which Fern-SDRPlay's authors do not control.
>
> If you modify Fern-SDRPlay, you may extend this permission to your
> version, but you are not obliged to do so. If you do not wish to do so,
> delete this permission from your version.

See `LICENSE`. The module contains no part of SDRplay's software; SDRplay's
API is under SDRplay's licence, which the operator accepts when installing
it.
