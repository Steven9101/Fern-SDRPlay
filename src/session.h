// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// The protocol loop of docs/MODULES.md in FernSDR: commands on one fd,
// samples on another, events on a third.
#pragma once

#include <chrono>
#include <cstddef>
#include <string>

#include "api.h"
#include "gain_control.h"
#include "presence.h"

namespace fern {

struct SessionIo {
    int commands = 0;
    int samples = 1;
    int events = 3;
    // Readable when the module should stop (a signalfd for SIGTERM and SIGINT
    // in the real program); -1 for none.
    int stop = -1;
};

struct SessionOptions {
    std::chrono::milliseconds stats_interval{1000};
    // FernSDR gives up on a module after 2 s without samples; noticing a
    // stalled RSP a little earlier lets the module say why.
    std::chrono::milliseconds stall_timeout{1500};
    // FernSDR wants ready within 15 s of open; the API gets 12 of them.
    std::chrono::milliseconds open_timeout{12000};
    // Stopping the writer and Uninit, ReleaseDevice and Close share this.
    // FernSDR sends SIGTERM 2 s after stop, which only reaches the module's
    // signalfd and so does not cut this short, and SIGKILL after 4 s. A
    // module killed inside the API may leave the service with the RSP
    // selected, so the API gets all but half a second of that.
    std::chrono::milliseconds shutdown_timeout{3500};
    // The delivered rate is measured over this window, from half a second
    // after the first samples, and must lie within 10 % of the announced
    // one: a factor the module has wrong about the API shows up here.
    std::chrono::milliseconds rate_window{2000};
    // The ring between the stream callback and fd 1; 0 for half a second of
    // samples at the band's rate, and at least 8 MiB.
    size_t ring_bytes = 0;
    GainControlTiming gain_timing;
    std::string usb_devices_dir = default_usb_devices_dir;
    // When an API call hangs: report and _exit() (the real program), or
    // only report, so that a test can go on once the fake returns.
    bool exit_on_hang = true;
};

struct SessionResult {
    int status = 0;
    // False when an API thread may still call into the module: the caller
    // must then leave with _exit() instead of returning from main().
    bool clean = true;
};

SessionResult run_session(Api& api, const SessionIo& io, const SessionOptions& options = SessionOptions());

}  // namespace fern
