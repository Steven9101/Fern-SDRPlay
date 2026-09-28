// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "settings.h"

#include <cctype>
#include <cmath>
#include <string_view>
#include <vector>

#ifndef FERN_SDRPLAY_VERSION
#error "FERN_SDRPLAY_VERSION must be defined by the build"
#endif

namespace fern {

const char* module_version() { return FERN_SDRPLAY_VERSION; }

const char* gain_mode_name(GainMode mode) {
    switch (mode) {
    case GainMode::automatic: return "auto";
    case GainMode::manual: return "manual";
    case GainMode::agc: return "agc";
    }
    return "auto";
}

bool LiveChange::empty() const {
    return !gain && !lna_state && !if_gain_reduction && !bias_tee && !rf_notch && !dab_notch && !am_notch && !ppm &&
           !dc_correction && !iq_correction;
}

namespace {

enum class Key {
    device,
    antenna,
    gain,
    lna_state,
    if_gain_reduction,
    bias_tee,
    rf_notch,
    dab_notch,
    am_notch,
    hdr,
    ppm,
    dc_correction,
    iq_correction,
    if_mode,
    bandwidth,
};

struct KeyInfo {
    Key key;
    const char* name;
    bool live;
};

// The order here is the order of --describe.
constexpr KeyInfo all_keys[] = {
    {Key::device, "device", false},
    {Key::antenna, "antenna", false},
    {Key::gain, "gain", true},
    {Key::lna_state, "lna_state", true},
    {Key::if_gain_reduction, "if_gain_reduction", true},
    {Key::bias_tee, "bias_tee", true},
    {Key::rf_notch, "rf_notch", true},
    {Key::dab_notch, "dab_notch", true},
    {Key::am_notch, "am_notch", true},
    {Key::hdr, "hdr", false},
    {Key::ppm, "ppm", true},
    {Key::dc_correction, "dc_correction", true},
    {Key::iq_correction, "iq_correction", true},
    {Key::if_mode, "if_mode", false},
    {Key::bandwidth, "bandwidth", false},
};

const KeyInfo* find_key(std::string_view name) {
    for (const KeyInfo& k : all_keys)
        if (name == k.name)
            return &k;
    return nullptr;
}

std::string known_keys() {
    std::string s;
    const size_t n = sizeof all_keys / sizeof all_keys[0];
    for (size_t i = 0; i < n; ++i) {
        if (i > 0)
            s += i + 1 == n ? " and " : ", ";
        s += all_keys[i].name;
    }
    return s;
}

// A value as the operator would recognise it in a message, kept short.
std::string shown(const json::Value& v) {
    std::string s = json::serialize(v);
    if (s.size() > 60)
        s = s.substr(0, 57) + "...";
    return s;
}

std::optional<Failure> invalid(std::string message) { return Failure{ErrorCode::invalid, std::move(message)}; }

bool whole_in_range(const json::Value& v, double lo, double hi, double& out) {
    if (!v.is_number() || !json::is_whole(v.as_number()))
        return false;
    const double d = v.as_number();
    if (d < lo || d > hi)
        return false;
    out = d;
    return true;
}

// SDRplay serial numbers are letters and digits; a little more is allowed
// so that an unexpected one can still be selected.
bool serial_text(std::string_view s) {
    if (s.empty() || s.size() >= 64)
        return false;
    for (char c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_')
            return false;
    return true;
}

std::optional<Failure> parse_device(const json::Value& v, DeviceSelector& out) {
    const std::string wrong = "module.device must be serial:<serial>, index:<n> or empty, not ";
    if (!v.is_string())
        return invalid(wrong + shown(v));
    const std::string& s = v.as_string();
    DeviceSelector sel;
    if (s.empty()) {
        sel.kind = DeviceSelector::Kind::only;
    } else if (s.rfind("serial:", 0) == 0 && serial_text(std::string_view(s).substr(7))) {
        sel.kind = DeviceSelector::Kind::serial;
        sel.serial = s.substr(7);
    } else if (s.rfind("index:", 0) == 0 && s.size() > 6 && s.size() <= 16) {
        uint64_t n = 0;
        for (char c : std::string_view(s).substr(6)) {
            if (c < '0' || c > '9')
                return invalid(wrong + shown(v));
            n = n * 10 + static_cast<uint64_t>(c - '0');
        }
        if (n > UINT32_MAX)
            return invalid(wrong + shown(v));
        sel.kind = DeviceSelector::Kind::index;
        sel.index = static_cast<uint32_t>(n);
    } else {
        return invalid(wrong + shown(v));
    }
    out = sel;
    return std::nullopt;
}

std::optional<Failure> parse_bool(const char* key, const json::Value& v, bool& out) {
    if (!v.is_bool())
        return invalid(std::string("module.") + key + " must be yes or no (a JSON boolean), not " + shown(v));
    out = v.as_bool();
    return std::nullopt;
}

std::optional<Failure> parse_value(const KeyInfo& k, const json::Value& v, ModuleSettings& s) {
    double d;
    switch (k.key) {
    case Key::device:
        return parse_device(v, s.device);
    case Key::antenna: {
        const auto a = v.is_string() ? antenna_from_name(v.as_string()) : std::nullopt;
        if (!a)
            return invalid("module.antenna must be auto, a, b, c, hiz, tuner1 or tuner2, not " + shown(v));
        s.antenna = *a;
        return std::nullopt;
    }
    case Key::gain:
        if (v.is_string() && v.as_string() == "auto")
            s.gain = GainMode::automatic;
        else if (v.is_string() && v.as_string() == "manual")
            s.gain = GainMode::manual;
        else if (v.is_string() && v.as_string() == "agc")
            s.gain = GainMode::agc;
        else
            return invalid("module.gain must be auto, manual or agc, not " + shown(v));
        return std::nullopt;
    case Key::lna_state:
        if (!whole_in_range(v, 0, max_lna_state, d))
            return invalid("module.lna_state must be a whole number from 0 (the most gain) up to " +
                           std::to_string(max_lna_state) + ", as far as the model has states, not " + shown(v));
        s.lna_state = static_cast<unsigned>(d);
        return std::nullopt;
    case Key::if_gain_reduction:
        if (!whole_in_range(v, min_if_reduction, max_if_reduction, d))
            return invalid("module.if_gain_reduction must be a whole number of dB from " +
                           std::to_string(min_if_reduction) + " (the most gain) to " +
                           std::to_string(max_if_reduction) + ", not " + shown(v));
        s.if_gain_reduction = static_cast<int>(d);
        return std::nullopt;
    case Key::bias_tee:
        return parse_bool(k.name, v, s.bias_tee);
    case Key::rf_notch:
        return parse_bool(k.name, v, s.rf_notch);
    case Key::dab_notch:
        return parse_bool(k.name, v, s.dab_notch);
    case Key::am_notch:
        return parse_bool(k.name, v, s.am_notch);
    case Key::hdr:
        return parse_bool(k.name, v, s.hdr);
    case Key::ppm:
        if (!v.is_number() || !std::isfinite(v.as_number()) || std::fabs(v.as_number()) > max_ppm)
            return invalid("module.ppm must be a number from -1000 to 1000, not " + shown(v));
        s.ppm = v.as_number();
        return std::nullopt;
    case Key::dc_correction:
        return parse_bool(k.name, v, s.dc_correction);
    case Key::iq_correction:
        return parse_bool(k.name, v, s.iq_correction);
    case Key::if_mode:
        if (v.is_string() && v.as_string() == "zero")
            s.if_mode = IfMode::zero;
        else if (v.is_string() && v.as_string() == "low")
            s.if_mode = IfMode::low;
        else
            return invalid("module.if_mode must be zero or low, not " + shown(v));
        return std::nullopt;
    case Key::bandwidth:
        if (v.is_string() && v.as_string() == "auto") {
            s.bandwidth_khz = 0;
            return std::nullopt;
        }
        for (int bw : bandwidths_khz) {
            if (v.is_string() && v.as_string() == std::to_string(bw)) {
                s.bandwidth_khz = bw;
                return std::nullopt;
            }
        }
        return invalid("module.bandwidth must be auto, 200, 300, 600, 1536, 5000, 6000, 7000 or 8000 (kHz), not " +
                       shown(v));
    }
    return invalid("internal: unhandled setting");
}

// Unknown keys are refused all at once, so that the operator can fix every
// one of them in one go.
std::optional<Failure> check_keys(const json::Value& settings) {
    std::vector<std::string> unknown;
    for (const json::Member& m : settings.members())
        if (!find_key(m.key))
            unknown.push_back(m.key);
    if (unknown.empty())
        return std::nullopt;
    std::string names;
    for (size_t i = 0; i < unknown.size() && i < 8; ++i) {
        if (i > 0)
            names += ", ";
        const std::string& key = unknown[i];
        names += "module." + (key.size() > 40 ? key.substr(0, 37) + "..." : key);
    }
    if (unknown.size() > 8)
        names += " and " + std::to_string(unknown.size() - 8) + " more";
    return invalid(std::string(unknown.size() == 1 ? "unknown setting " : "unknown settings ") + names +
                   "; this module takes " + known_keys());
}

// The hand-set gain values only mean something where the module does not
// set the gain itself.
std::optional<Failure> check_gain_values(GainMode mode, bool lna_given, bool if_given) {
    if (mode == GainMode::automatic && (lna_given || if_given))
        return invalid(std::string("module.") + (lna_given ? "lna_state" : "if_gain_reduction") +
                       " sets the gain by hand, but module.gain is auto, which sets it itself; set module.gain = "
                       "manual (or agc, for lna_state)");
    if (mode == GainMode::agc && if_given)
        return invalid("module.if_gain_reduction is set by the API's AGC while module.gain = agc; leave it out, or "
                       "set module.gain = manual");
    return std::nullopt;
}

}  // namespace

std::optional<Failure> parse_open(const json::Value& message, OpenRequest& out) {
    OpenRequest req;
    double d;

    const json::Value* signal = message.find("signal");
    if (!signal)
        return invalid("open carries no signal");
    if (!signal->is_string() || (signal->as_string() != "iq" && signal->as_string() != "real"))
        return invalid("signal must be iq or real, not " + shown(*signal));
    if (signal->as_string() != "iq")
        return invalid("an RSP delivers complex (I/Q) samples; set signal = iq for this band");

    const json::Value* rate = message.find("sample_rate");
    if (!rate)
        return invalid("open carries no sample_rate");
    if (!whole_in_range(*rate, 1, max_fs_hz, d))
        return invalid("sample_rate " + shown(*rate) +
                       " Hz is not possible with an RSP; use a whole number of Hz from 62500 to 10660000, such as "
                       "2000000 or 8000000");
    req.sample_rate = static_cast<uint32_t>(d);

    const json::Value* center = message.find("center");
    if (!center)
        return invalid("open carries no center");
    if (!center->is_number() || !std::isfinite(center->as_number()) || center->as_number() <= 0)
        return invalid("center must be the frequency the RSP tunes to, in Hz, not " + shown(*center));
    req.center = center->as_number();

    const json::Value* settings = message.find("settings");
    bool lna_given = false;
    bool if_given = false;
    if (settings) {
        if (!settings->is_object())
            return invalid("settings in open must be an object, not " + shown(*settings));
        if (auto f = check_keys(*settings))
            return f;
        for (const json::Member& m : settings->members()) {
            if (auto f = parse_value(*find_key(m.key), m.value, req.settings))
                return f;
            lna_given = lna_given || m.key == "lna_state";
            if_given = if_given || m.key == "if_gain_reduction";
        }
    }
    if (auto f = check_gain_values(req.settings.gain, lna_given, if_given))
        return f;
    if (req.settings.hdr && !is_hdr_frequency(req.center)) {
        std::string list;
        for (double f : hdr_frequencies())
            list += (list.empty() ? "" : ", ") + std::to_string(static_cast<long>(f));
        return invalid("module.hdr works only with the band's centre at one of " + list +
                       " Hz (the RSPdx's HDR frequencies); the centre is " + shown(*center));
    }
    // The plan is made again with the model known; this catches an
    // impossible rate before the API is touched.
    RatePlan plan;
    if (auto f = plan_rate(req.sample_rate, req.settings.if_mode, req.settings.bandwidth_khz, plan))
        return f;
    out = req;
    return std::nullopt;
}

std::optional<Failure> parse_set(const json::Value& settings, LiveChange& out) {
    if (!settings.is_object())
        return invalid("settings in set must be an object, not " + shown(settings));
    if (auto f = check_keys(settings))
        return f;
    LiveChange change;
    ModuleSettings scratch;
    for (const json::Member& m : settings.members()) {
        const KeyInfo& k = *find_key(m.key);
        if (!k.live)
            return invalid("module." + m.key + " cannot change while the band runs; restart the band to apply it");
        if (auto f = parse_value(k, m.value, scratch))
            return f;
        switch (k.key) {
        case Key::gain: change.gain = scratch.gain; break;
        case Key::lna_state: change.lna_state = scratch.lna_state; break;
        case Key::if_gain_reduction: change.if_gain_reduction = scratch.if_gain_reduction; break;
        case Key::bias_tee: change.bias_tee = scratch.bias_tee; break;
        case Key::rf_notch: change.rf_notch = scratch.rf_notch; break;
        case Key::dab_notch: change.dab_notch = scratch.dab_notch; break;
        case Key::am_notch: change.am_notch = scratch.am_notch; break;
        case Key::ppm: change.ppm = scratch.ppm; break;
        case Key::dc_correction: change.dc_correction = scratch.dc_correction; break;
        case Key::iq_correction: change.iq_correction = scratch.iq_correction; break;
        default: break;
        }
    }
    // With the gain mode in the same set, the values are checked against
    // it here; against the mode in use, by the receiver.
    if (change.gain)
        if (auto f = check_gain_values(*change.gain, change.lna_state.has_value(),
                                       change.if_gain_reduction.has_value()))
            return f;
    out = change;
    return std::nullopt;
}

json::Value settings_schema() {
    json::Value list = json::Value::array();
    for (const KeyInfo& k : all_keys) {
        json::Value s = json::Value::object();
        s.set("key", k.name);
        switch (k.key) {
        case Key::device:
            s.set("type", "string");
            s.set("label", "Device");
            s.set("default", "");
            s.set("help",
                  "Which RSP to use: serial:<serial>, or index:<n>, which changes when another program holds an "
                  "RSP. Leave empty when exactly one RSP is plugged in. With several, give each band serial:. "
                  "fern-sdrplay --list-devices shows them.");
            break;
        case Key::antenna:
            s.set("type", "choice");
            s.set("label", "Antenna input");
            s.set("choices", json::Value::array()
                                 .push("auto")
                                 .push("a")
                                 .push("b")
                                 .push("c")
                                 .push("hiz")
                                 .push("tuner1")
                                 .push("tuner2"));
            s.set("default", "auto");
            s.set("help",
                  "auto is the first input. RSP2: a, b or hiz (up to 60 MHz). RSPduo: tuner1, tuner2 or hiz (tuner "
                  "1's Hi-Z input, up to 60 MHz). RSPdx and RSPdx-R2: a, b or c (up to 200 MHz). The RSP1, RSP1A "
                  "and RSP1B have one input.");
            break;
        case Key::gain:
            s.set("type", "choice");
            s.set("label", "Gain");
            s.set("choices", json::Value::array().push("auto").push("manual").push("agc"));
            s.set("default", "auto");
            s.set("help",
                  "auto sets the LNA state and IF gain itself: the most gain that keeps the converter out of "
                  "overload with 6 dB to spare. manual uses lna_state and if_gain_reduction. agc leaves the IF "
                  "gain to the SDRplay API's AGC, which pumps with strong signals.");
            break;
        case Key::lna_state:
            s.set("type", "number");
            s.set("label", "LNA state");
            s.set("min", 0);
            s.set("max", max_lna_state);
            s.set("help",
                  "With gain manual or agc: the LNA state, 0 for the most gain. How many states there are depends "
                  "on the model, input and frequency (the SDRplay API specification's section 5). Left out, it "
                  "starts where gain = auto would.");
            break;
        case Key::if_gain_reduction:
            s.set("type", "number");
            s.set("label", "IF gain reduction");
            s.set("min", min_if_reduction);
            s.set("max", max_if_reduction);
            s.set("unit", "dB");
            s.set("help",
                  "With gain = manual: the IF gain reduction, 20 dB (the most gain) to 59 dB. Left out, it starts "
                  "where gain = auto would.");
            break;
        case Key::bias_tee:
            s.set("type", "boolean");
            s.set("label", "Bias tee");
            s.set("default", false);
            s.set("help",
                  "Puts DC on the antenna input to power an active antenna or LNA: RSP1A, RSP1B, RSP2 inputs a and "
                  "b, RSPduo tuner 2, RSPdx and RSPdx-R2 input b.");
            break;
        case Key::rf_notch:
            s.set("type", "boolean");
            s.set("label", "Broadcast notch");
            s.set("default", false);
            s.set("help", "The notch filter for the FM and MW broadcast bands. Every model but the RSP1.");
            break;
        case Key::dab_notch:
            s.set("type", "boolean");
            s.set("label", "DAB notch");
            s.set("default", false);
            s.set("help", "The notch filter for DAB broadcasts. RSP1A, RSP1B, RSPduo, RSPdx and RSPdx-R2.");
            break;
        case Key::am_notch:
            s.set("type", "boolean");
            s.set("label", "AM notch");
            s.set("default", false);
            s.set("help", "The RSPduo's MW notch on tuner 1.");
            break;
        case Key::hdr:
            s.set("type", "boolean");
            s.set("label", "HDR mode");
            s.set("default", false);
            s.set("help",
                  "The RSPdx's and RSPdx-R2's high dynamic range mode below 2 MHz. It works only with the band's "
                  "centre at 135000, 175000, 220000, 250000, 340000, 475000, 516000, 875000, 1125000 or 1900000 "
                  "Hz.");
            break;
        case Key::ppm:
            s.set("type", "number");
            s.set("label", "Frequency correction");
            s.set("default", 0);
            s.set("min", -max_ppm);
            s.set("max", max_ppm);
            s.set("unit", "ppm");
            s.set("help", "The reference oscillator's error in parts per million, corrected by the API.");
            break;
        case Key::dc_correction:
            s.set("type", "boolean");
            s.set("label", "DC offset correction");
            s.set("default", true);
            s.set("advanced", true);
            s.set("help", "The API's removal of the DC offset, the spike at the centre of a zero-IF band.");
            break;
        case Key::iq_correction:
            s.set("type", "boolean");
            s.set("label", "IQ imbalance correction");
            s.set("default", true);
            s.set("advanced", true);
            s.set("help", "The API's correction of I/Q imbalance, which otherwise mirrors signals around the centre.");
            break;
        case Key::if_mode:
            s.set("type", "choice");
            s.set("label", "IF mode");
            s.set("choices", json::Value::array().push("zero").push("low"));
            s.set("default", "zero");
            s.set("advanced", true);
            s.set("help",
                  "zero: zero IF, any rate from 62500 to 10660000. low: a 1.62 MHz IF that the API mixes down, "
                  "with no DC offset at the centre, for rates of 2000000 and 1000000, 500000, 250000, 125000 and "
                  "62500.");
            break;
        case Key::bandwidth:
            s.set("type", "choice");
            s.set("label", "IF filter");
            s.set("choices", json::Value::array()
                                 .push("auto")
                                 .push("200")
                                 .push("300")
                                 .push("600")
                                 .push("1536")
                                 .push("5000")
                                 .push("6000")
                                 .push("7000")
                                 .push("8000"));
            s.set("default", "auto");
            s.set("unit", "kHz");
            s.set("advanced", true);
            s.set("help", "The tuner's IF filter; auto is the widest that fits into the band's sample rate.");
            break;
        }
        s.set("live", k.live);
        list.push(std::move(s));
    }
    return list;
}

json::Value describe_module() {
    json::Value d = json::Value::object();
    d.set("api", module_api);
    d.set("id", module_id);
    d.set("name", module_name);
    d.set("version", module_version());
    d.set("kind", "input");
    d.set("settings", settings_schema());
    return d;
}

}  // namespace fern
