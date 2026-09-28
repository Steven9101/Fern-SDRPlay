// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// The settings the module declares (printed by --describe and copied into
// the package manifest) and the validation of open and set against them.
// Validation here needs no API; what depends on the model found happens in
// receiver.cpp.
#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "failure.h"
#include "json.h"
#include "models.h"

namespace fern {

constexpr const char* module_id = "sdrplay";
constexpr const char* module_name = "SDRplay RSP";
constexpr int module_api = 1;
const char* module_version();

struct DeviceSelector {
    enum class Kind { only, serial, index };
    Kind kind = Kind::only;
    std::string serial;
    uint32_t index = 0;
};

// auto: the module's own control along the gain ladder (gain_control.h);
// manual: lna_state and if_gain_reduction as given; agc: the API's AGC sets
// the IF gain reduction, the LNA state stays as given.
enum class GainMode { automatic, manual, agc };
const char* gain_mode_name(GainMode mode);

struct ModuleSettings {
    DeviceSelector device;
    Antenna antenna = Antenna::automatic;
    GainMode gain = GainMode::automatic;
    std::optional<unsigned> lna_state;
    std::optional<int> if_gain_reduction;
    bool bias_tee = false;
    bool rf_notch = false;
    bool dab_notch = false;
    bool am_notch = false;
    bool hdr = false;
    double ppm = 0;
    bool dc_correction = true;
    bool iq_correction = true;
    IfMode if_mode = IfMode::zero;
    int bandwidth_khz = 0;  // 0 for auto
};

struct OpenRequest {
    uint32_t sample_rate = 0;
    double center = 0;
    ModuleSettings settings;
};

// The settings that may change while samples flow.
struct LiveChange {
    std::optional<GainMode> gain;
    std::optional<unsigned> lna_state;
    std::optional<int> if_gain_reduction;
    std::optional<bool> bias_tee;
    std::optional<bool> rf_notch;
    std::optional<bool> dab_notch;
    std::optional<bool> am_notch;
    std::optional<double> ppm;
    std::optional<bool> dc_correction;
    std::optional<bool> iq_correction;
    bool empty() const;
};

constexpr double max_ppm = 1000;
// The most LNA states any RSP has (the RSPdx above 250 MHz).
constexpr unsigned max_lna_state = 27;

// Validates an open message. Fails with ErrorCode::invalid.
std::optional<Failure> parse_open(const json::Value& message, OpenRequest& out);

// Validates the settings object of a set message: only live settings may
// appear. Fails with ErrorCode::invalid.
std::optional<Failure> parse_set(const json::Value& settings, LiveChange& out);

// The settings list, identical in --describe and in the package manifest.
json::Value settings_schema();
// The complete --describe object.
json::Value describe_module();

}  // namespace fern
