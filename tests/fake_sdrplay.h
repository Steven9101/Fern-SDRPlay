// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// A stand-in for SDRplay's libsdrplay_api.so, built from the module's own
// declarations (src/rsp_api.h), that the tests load through the module's
// real loader. It lists configurable RSPs, keeps the parameter structs the
// module writes into, records every call, and streams a known pattern
// through the stream callback at the rate the parameters imply, with gaps,
// resets, overloads, removal and stalls on request.
//
// The unit tests reach its state through fake_sdrplay_state(); a program
// run by the command line tests configures it with the environment
// variable FAKE_SDRPLAY, e.g. "devices=255:A1,4:B2;busy=B2;version=3.14".
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "rsp_api.h"

namespace fake {

namespace r = fern::rsp;

struct DeviceSpec {
    unsigned char hw = r::hw::rsp1a;
    std::string serial;
    bool valid = true;
    // Selected by another process: on the bus, but GetDevices leaves it out.
    bool busy = false;
    r::RspDuoMode duo_modes = r::duo_mode::single_tuner | r::duo_mode::dual_tuner | r::duo_mode::master;
};

struct Config {
    float version = 3.15f;
    std::vector<DeviceSpec> devices;
    // Errors to return from each call; 0 for success.
    r::ErrT open_error = 0;
    r::ErrT version_error = 0;
    r::ErrT lock_error = 0;
    r::ErrT get_devices_error = 0;
    r::ErrT select_error = 0;
    r::ErrT params_error = 0;
    r::ErrT init_error = 0;
    r::ErrT update_error = 0;
    // Uninit returns this and leaves the stream running, as the API might.
    r::ErrT uninit_error = 0;
    // Update fails with update_error only when its reason has one of these.
    r::ReasonForUpdate update_error_on = 0;
    // How many such updates fail before they succeed again; 0 for all.
    int update_error_count = 0;
    // Milliseconds some calls take before they return.
    int lock_ms = 0;
    int init_ms = 0;
    int uninit_ms = 0;

    // The stream.
    unsigned chunk = 1024;  // samples per callback
    double rate_factor = 1.0;
    bool counts_before_decimation = false;
    uint64_t gap_after = 0;  // callbacks; 0 for never
    uint32_t gap_samples = 0;
    uint64_t reset_after = 0;
    uint64_t overload_after = 0;
    uint64_t corrected_after = 0;
    uint64_t remove_after = 0;
    uint64_t fail_after = 0;
    uint64_t stall_after = 0;
};

struct UpdateRecord {
    r::ReasonForUpdate reason;
    r::ReasonForUpdateExt1 ext1;
    unsigned lna_state;
    int if_reduction;
    r::AgcControl agc;
};

// The sample pattern: I counts up through -8192..8191, Q mirrors it, so
// that a test can check every pair and its order. With loud, the pattern is
// four times as large and clipped, as a converter driven too hard.
inline int16_t pattern_i(uint64_t s) { return static_cast<int16_t>(static_cast<int>(s % 16384) - 8192); }
inline int16_t pattern_q(uint64_t s) { return static_cast<int16_t>(-1 - pattern_i(s)); }

struct State {
    std::mutex mutex;
    Config cfg;
    std::vector<std::string> calls;
    bool opened = false;
    bool locked = false;
    bool listed_while_locked = false;
    bool selected_while_locked = false;
    int selected = -1;  // index into cfg.devices
    r::TunerSelect selected_tuner = 0;
    r::RspDuoMode selected_mode = 0;
    bool initialised = false;
    r::DevParamsT dev{};
    r::RxChannelParamsT a{};
    r::RxChannelParamsT b{};
    r::DeviceParamsT params{};
    // As the module had written them when it called Init.
    r::DevParamsT init_dev{};
    r::RxChannelParamsT init_channel{};
    std::vector<UpdateRecord> updates;
    int acks = 0;
    int failed_updates = 0;
    double output_rate = 0;

    std::atomic<bool> loud{false};
    std::atomic<uint64_t> callbacks{0};
    std::atomic<bool> stop{false};
    std::thread streamer;
    r::CallbackFnsT cb{};
    void* ctx = nullptr;
};

}  // namespace fake

extern "C" fake::State* fake_sdrplay_state();
// Stops a running stream and forgets everything, the configuration too.
extern "C" void fake_sdrplay_reset();
