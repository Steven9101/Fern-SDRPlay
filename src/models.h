// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// What the module knows about each RSP: its inputs and switches, the gain
// reduction of each LNA state (API Specification 3.15, section 5), the gain
// ladder that gain = auto climbs, and how a band's sample rate becomes the
// API's converter rate, decimation, IF mode and filter. All of it is plain
// computation, so the tests check it without an API.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "failure.h"

namespace fern {

enum class Model { rsp1, rsp1a, rsp1b, rsp2, rspduo, rspdx, rspdx_r2 };

// From DeviceT.hwVer; empty for a model this module does not know.
std::optional<Model> model_from_hw(unsigned hw_version);
const char* model_name(Model model);

// The antenna setting. automatic is the model's first input: A, or tuner 1
// of an RSPduo. hiz is the RSP2's Hi-Z input or the RSPduo's tuner 1 Hi-Z.
enum class Antenna { automatic, a, b, c, hiz, tuner1, tuner2 };
const char* antenna_name(Antenna antenna);
std::optional<Antenna> antenna_from_name(const std::string& name);

// The input a request resolves to on a model at a centre frequency, or why
// it cannot: an input the model lacks, or a frequency beyond the input's
// range.
std::optional<Failure> resolve_antenna(Model model, Antenna requested, double center_hz, Antenna& out);

// The switches, per model and input.
bool has_bias_tee(Model model, Antenna input);
bool has_rf_notch(Model model);
bool has_dab_notch(Model model);
bool has_am_notch(Model model, Antenna input);
bool has_hdr(Model model);

// HDR works at these centres only (API Specification 3.15, sdrplay_api_Init).
const std::vector<double>& hdr_frequencies();
bool is_hdr_frequency(double center_hz);

// The gain reduction in dB of each LNA state, index = state, for a model,
// input and centre frequency; with hdr, the RSPdx's HDR row.
std::vector<int> lna_reductions(Model model, Antenna input, double center_hz, bool hdr);

// The IF gain reduction the API takes with the normal minimum (MAX_BB_GR).
constexpr int min_if_reduction = 20;
constexpr int max_if_reduction = 59;

struct GainStep {
    unsigned lna_state = 0;
    int if_reduction = 0;
    int reduction = 0;  // the LNA's and the IF's together, dB
};

// The steps gain = auto moves along, from the most gain reduction to the
// least, i.e. in ascending gain, about 3 dB apart. At each total the LNA
// keeps as much gain as it can while the IF reduction stays at or under 40
// dB: the LNA's gain sets the noise figure, and an IF with room either way
// lets the next step be a small one.
std::vector<GainStep> gain_ladder(const std::vector<int>& lna_reductions);
// The step gain = auto starts from: the middle of the ladder's range.
size_t ladder_start(const std::vector<GainStep>& ladder);
// The step nearest to a total reduction.
size_t ladder_nearest(const std::vector<GainStep>& ladder, int reduction);

enum class IfMode { zero, low };
const char* if_mode_name(IfMode mode);

// How the API makes a band's sample rate.
struct RatePlan {
    double fs_hz = 0;         // the converter, devParams->fsFreq.fsHz
    unsigned decimation = 1;  // 1, or 2 to 32
    int if_khz = 0;           // 0, or 1620 for low IF
    int bandwidth_khz = 0;    // the tuner's IF filter
    bool wide_band = false;   // ctrlParams.decimation.wideBandSignal
    double output_hz = 0;     // what the callback delivers: the band's rate
};

constexpr double min_fs_hz = 2000000;
constexpr double max_fs_hz = 10660000;
constexpr int bandwidths_khz[] = {200, 300, 600, 1536, 5000, 6000, 7000, 8000};

// bandwidth_khz 0 chooses the widest filter that fits in the band. Fails
// with ErrorCode::invalid and says which rates work.
std::optional<Failure> plan_rate(uint32_t sample_rate, IfMode mode, int bandwidth_khz, RatePlan& out);

}  // namespace fern
