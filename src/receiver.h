// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// Opens the API, selects the RSP the settings name, sets it up and starts
// the stream; applies live changes; stops it again. Every call into the API
// happens here, on the session's thread, under the watchdog.
//
// The API's rules this follows (API Specification 3.15): sdrplay_api_Open
// first and sdrplay_api_Close last, "otherwise the service can be left in an
// unknown state"; the API lock only from GetDevices to SelectDevice, and
// released on every path, because while one process holds it every call of
// every other process waits (measured with API 3.15: even ApiVersion);
// Uninit, ReleaseDevice and Close on the way out.
#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "api.h"
#include "failure.h"
#include "json.h"
#include "models.h"
#include "rsp_api.h"
#include "settings.h"
#include "stream.h"
#include "watchdog.h"

namespace fern {

// What the hardware was set to.
struct Effective {
    RatePlan plan;
    double center = 0;
    Antenna antenna = Antenna::a;
    GainMode gain = GainMode::automatic;
    unsigned lna_state = 0;
    int if_reduction = 40;
    bool bias_tee = false;
    bool rf_notch = false;
    bool dab_notch = false;
    bool am_notch = false;
    bool hdr = false;
    double ppm = 0;
    bool dc_correction = true;
    bool iq_correction = true;
    IfMode if_mode = IfMode::zero;
};

struct Identity {
    Model model = Model::rsp1a;
    unsigned hw_version = 0;
    std::string serial;
    float api_version = 0;
};

// The API AGC's set point when gain = agc: within the range the
// specification allows at every sample rate (section 2.5.3), and the one
// SoapySDRPlay3 and SDR++ use.
constexpr int agc_set_point_dbfs = -30;

class Receiver {
public:
    using Clock = std::chrono::steady_clock;

    // usb_devices_dir: where presence.h counts RSPs.
    Receiver(Api& api, Watchdog& watchdog, std::string usb_devices_dir = "/sys/bus/usb/devices");
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    // Everything up to a running stream into `stream`, within the deadline.
    // On failure everything opened is closed again.
    std::optional<Failure> open(const OpenRequest& request, Stream& stream, Clock::time_point deadline);

    // Applies a set; validation failures change nothing. When the API
    // refuses, the fields are put back as they were.
    std::optional<Failure> apply(const LiveChange& change);
    // Moves the tuner while streaming. FernSDR's module API 1 restarts a
    // band to change its centre, so the session never calls this.
    std::optional<Failure> retune(double center);

    // For gain = auto.
    const std::vector<GainStep>& ladder() const { return ladder_; }
    size_t ladder_step() const { return ladder_step_; }
    std::optional<Failure> set_ladder_step(size_t step);

    std::optional<Failure> acknowledge_overload();

    const Effective& effective() const { return effective_; }
    const Identity& identity() const { return identity_; }
    bool is_open() const { return initialised_; }
    // The LNA reduction table in use, for tests and the log.
    const std::vector<int>& lna_table() const { return lna_table_; }

    json::Value device_json() const;
    json::Value settings_json() const;
    json::Value settings_json(const LiveChange& change) const;

    // Uninit, ReleaseDevice, Close, each as far as the deadline allows. False
    // when a call did not return in time.
    bool close_by(Clock::time_point deadline);

private:
    std::optional<Failure> api_failure(const char* call, rsp::ErrT err, rsp::DeviceT* device) const;
    std::optional<Failure> hung(const char* call) const;
    std::optional<Failure> choose(rsp::DeviceT* devices, unsigned count, const DeviceSelector& selector,
                                  rsp::DeviceT& chosen);
    std::optional<Failure> plan(const OpenRequest& request, Model model, Effective& out,
                                std::vector<int>& table) const;
    std::optional<Failure> check_switches(const Effective& e) const;
    rsp::RxChannelParamsT* channel() const;
    void write_fields(const Effective& e);
    void reasons_between(const Effective& from, const Effective& to, rsp::ReasonForUpdate& reason,
                         rsp::ReasonForUpdateExt1& ext1) const;
    std::optional<Failure> update(const char* what, rsp::ReasonForUpdate reason, rsp::ReasonForUpdateExt1 ext1);
    std::optional<Failure> change_to(const Effective& next);
    void release_and_close(Clock::time_point deadline);

    Api& api_;
    Watchdog& watchdog_;
    std::string usb_devices_dir_;
    bool api_open_ = false;
    bool selected_ = false;
    bool initialised_ = false;
    rsp::DeviceT device_{};
    rsp::DeviceParamsT* params_ = nullptr;
    rsp::CallbackFnsT callbacks_{};
    Identity identity_;
    Effective effective_;
    std::vector<int> lna_table_;
    std::vector<GainStep> ladder_;
    size_t ladder_step_ = 0;
};

}  // namespace fern
