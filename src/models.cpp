// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "models.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "rsp_api.h"

namespace fern {

std::optional<Model> model_from_hw(unsigned hw_version) {
    switch (hw_version) {
    case rsp::hw::rsp1: return Model::rsp1;
    case rsp::hw::rsp1a: return Model::rsp1a;
    case rsp::hw::rsp1b: return Model::rsp1b;
    case rsp::hw::rsp2: return Model::rsp2;
    case rsp::hw::rspduo: return Model::rspduo;
    case rsp::hw::rspdx: return Model::rspdx;
    case rsp::hw::rspdx_r2: return Model::rspdx_r2;
    default: return std::nullopt;
    }
}

const char* model_name(Model model) {
    switch (model) {
    case Model::rsp1: return "RSP1";
    case Model::rsp1a: return "RSP1A";
    case Model::rsp1b: return "RSP1B";
    case Model::rsp2: return "RSP2";
    case Model::rspduo: return "RSPduo";
    case Model::rspdx: return "RSPdx";
    case Model::rspdx_r2: return "RSPdx-R2";
    }
    return "RSP";
}

const char* antenna_name(Antenna antenna) {
    switch (antenna) {
    case Antenna::automatic: return "auto";
    case Antenna::a: return "a";
    case Antenna::b: return "b";
    case Antenna::c: return "c";
    case Antenna::hiz: return "hiz";
    case Antenna::tuner1: return "tuner1";
    case Antenna::tuner2: return "tuner2";
    }
    return "auto";
}

std::optional<Antenna> antenna_from_name(const std::string& name) {
    for (Antenna a : {Antenna::automatic, Antenna::a, Antenna::b, Antenna::c, Antenna::hiz, Antenna::tuner1,
                      Antenna::tuner2})
        if (name == antenna_name(a))
            return a;
    return std::nullopt;
}

namespace {

constexpr double mhz = 1e6;

bool one_input(Model m) { return m == Model::rsp1 || m == Model::rsp1a || m == Model::rsp1b; }
bool is_dx(Model m) { return m == Model::rspdx || m == Model::rspdx_r2; }

std::string mhz_text(double hz) {
    char text[32];
    std::snprintf(text, sizeof text, "%.6g MHz", hz / mhz);
    return text;
}

Failure invalid(std::string message) { return Failure{ErrorCode::invalid, std::move(message)}; }

}  // namespace

std::optional<Failure> resolve_antenna(Model model, Antenna requested, double center_hz, Antenna& out) {
    const std::string name = model_name(model);
    // The RSP1's range is SoapySDRPlay3's; the others' the datasheets'.
    const double lowest = model == Model::rsp1 ? 10e3 : 1e3;
    const double highest = 2000 * mhz;
    if (center_hz < lowest || center_hz > highest)
        return invalid("the " + name + " tunes from " + (model == Model::rsp1 ? "10 kHz" : "1 kHz") +
                       " to 2 GHz; the band's centre " + mhz_text(center_hz) + " is outside that");

    Antenna resolved = requested;
    if (one_input(model)) {
        if (requested != Antenna::automatic && requested != Antenna::a)
            return invalid("the " + name + " has one antenna input; leave module.antenna at auto (or a)");
        resolved = Antenna::a;
    } else if (model == Model::rsp2) {
        if (requested == Antenna::automatic)
            resolved = Antenna::a;
        else if (requested != Antenna::a && requested != Antenna::b && requested != Antenna::hiz)
            return invalid("an RSP2 has the antenna inputs a, b and hiz; module.antenna cannot be " +
                           std::string(antenna_name(requested)));
    } else if (model == Model::rspduo) {
        if (requested == Antenna::automatic)
            resolved = Antenna::tuner1;
        else if (requested != Antenna::tuner1 && requested != Antenna::tuner2 && requested != Antenna::hiz)
            return invalid("an RSPduo has the inputs tuner1, tuner2 and hiz (tuner 1's Hi-Z input); module.antenna "
                           "cannot be " +
                           std::string(antenna_name(requested)));
    } else {
        if (requested == Antenna::automatic)
            resolved = Antenna::a;
        else if (requested != Antenna::a && requested != Antenna::b && requested != Antenna::c)
            return invalid("an " + name + " has the antenna inputs a, b and c; module.antenna cannot be " +
                           std::string(antenna_name(requested)));
    }
    // The Hi-Z inputs have gain tables up to 60 MHz only (section 5), and
    // the RSPdx-R2 datasheet gives antenna C 1 kHz to 200 MHz.
    if (resolved == Antenna::hiz && center_hz > 60 * mhz)
        return invalid("the " + name + "'s Hi-Z input works up to 60 MHz; the band's centre is " +
                       mhz_text(center_hz) + ". Use another input");
    if (resolved == Antenna::c && center_hz > 200 * mhz)
        return invalid("the " + name + "'s antenna C works up to 200 MHz; the band's centre is " +
                       mhz_text(center_hz) + ". Use a or b");
    out = resolved;
    return std::nullopt;
}

bool has_bias_tee(Model model, Antenna input) {
    switch (model) {
    case Model::rsp1: return false;
    case Model::rsp1a:
    case Model::rsp1b: return true;
    case Model::rsp2: return input == Antenna::a || input == Antenna::b;
    // The RSPduo feeds its bias tee to tuner 2 (RSPduo Introduction), the
    // RSPdx-R2 to antenna B (its datasheet); the RSPdx has the same inputs.
    case Model::rspduo: return input == Antenna::tuner2;
    case Model::rspdx:
    case Model::rspdx_r2: return input == Antenna::b;
    }
    return false;
}

bool has_rf_notch(Model model) { return model != Model::rsp1; }
bool has_dab_notch(Model model) { return model != Model::rsp1 && model != Model::rsp2; }
bool has_am_notch(Model model, Antenna input) {
    return model == Model::rspduo && (input == Antenna::tuner1 || input == Antenna::hiz);
}
bool has_hdr(Model model) { return is_dx(model); }

const std::vector<double>& hdr_frequencies() {
    static const std::vector<double> list = {135000, 175000, 220000, 250000,  340000,
                                             475000, 516000, 875000, 1125000, 1900000};
    return list;
}

bool is_hdr_frequency(double center_hz) {
    for (double f : hdr_frequencies())
        if (center_hz == f)
            return true;
    return false;
}

std::vector<int> lna_reductions(Model model, Antenna input, double f, bool hdr) {
    // Section 5 of the API Specification 3.15, footnotes dropped: the RSP1's
    // states 2 and 3 include the mixer's reduction, which is why its table is
    // not in order.
    const std::vector<int> rsp1a_am = {0, 6, 12, 18, 37, 42, 61};
    const std::vector<int> rsp1a_vhf = {0, 6, 12, 18, 20, 26, 32, 38, 57, 62};
    const std::vector<int> rsp1a_uhf = {0, 7, 13, 19, 20, 27, 33, 39, 45, 64};
    const std::vector<int> rsp1a_l = {0, 6, 12, 20, 26, 32, 38, 43, 62};
    const std::vector<int> hiz = {0, 6, 12, 18, 37};
    switch (model) {
    case Model::rsp1:
        if (f < 420 * mhz)
            return {0, 24, 19, 43};
        if (f < 1000 * mhz)
            return {0, 7, 19, 26};
        return {0, 5, 19, 24};
    case Model::rsp1a:
    case Model::rsp1b:
    case Model::rspduo: {
        if (model == Model::rspduo && input == Antenna::hiz)
            return hiz;
        // The RSP1B's 7-state row ends at 50 MHz, the others' at 60.
        const double am_top = model == Model::rsp1b ? 50 * mhz : 60 * mhz;
        if (f < am_top)
            return rsp1a_am;
        if (f < 420 * mhz)
            return rsp1a_vhf;
        if (f < 1000 * mhz)
            return rsp1a_uhf;
        return rsp1a_l;
    }
    case Model::rsp2:
        if (input == Antenna::hiz)
            return hiz;
        if (f < 420 * mhz)
            return {0, 10, 15, 21, 24, 34, 39, 45, 64};
        if (f < 1000 * mhz)
            return {0, 7, 10, 17, 22, 41};
        return {0, 5, 21, 15, 15, 34};
    case Model::rspdx:
    case Model::rspdx_r2:
        if (hdr)
            return {0, 3, 6, 9, 12, 15, 18, 21, 24, 25, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60};
        if (f < 12 * mhz)
            return {0, 3, 6, 9, 12, 15, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60};
        if (f < 50 * mhz)
            return {0, 3, 6, 9, 12, 15, 18, 24, 27, 30, 33, 36, 39, 42, 45, 48, 51, 54, 57, 60};
        if (f < 60 * mhz)
            return {0,  3,  6,  9,  12, 20, 23, 26, 29, 32, 35, 38, 44,
                    47, 50, 53, 56, 59, 62, 65, 68, 71, 74, 77, 80};
        if (f < 250 * mhz)
            return {0,  3,  6,  9,  12, 15, 24, 27, 30, 33, 36, 39, 42, 45,
                    48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84};
        if (f < 420 * mhz)
            return {0,  3,  6,  9,  12, 15, 18, 24, 27, 30, 33, 36, 39, 42,
                    45, 48, 51, 54, 57, 60, 63, 66, 69, 72, 75, 78, 81, 84};
        if (f < 1000 * mhz)
            return {0, 7, 10, 13, 16, 19, 22, 25, 31, 34, 37, 40, 43, 46, 49, 52, 55, 58, 61, 64, 67};
        return {0, 5, 8, 11, 14, 17, 20, 32, 35, 38, 41, 44, 47, 50, 53, 56, 59, 62, 65};
    }
    return {0};
}

std::vector<GainStep> gain_ladder(const std::vector<int>& lna) {
    std::vector<GainStep> ladder;
    if (lna.empty())
        return ladder;
    constexpr int knee = 40;
    constexpr int spacing = 3;
    const int lowest_lna = *std::min_element(lna.begin(), lna.end());
    const int highest_lna = *std::max_element(lna.begin(), lna.end());
    const int most = highest_lna + max_if_reduction;
    const int least = lowest_lna + min_if_reduction;
    for (int total = most;; total -= spacing) {
        if (total < least)
            total = least;
        // The state with the least LNA reduction that keeps the IF at or
        // under the knee; failing that, the one leaving the IF the least.
        int chosen = -1;
        for (size_t s = 0; s < lna.size(); ++s) {
            const int if_gr = total - lna[s];
            if (if_gr < min_if_reduction || if_gr > knee)
                continue;
            if (chosen < 0 || lna[s] < lna[static_cast<size_t>(chosen)])
                chosen = static_cast<int>(s);
        }
        if (chosen < 0) {
            for (size_t s = 0; s < lna.size(); ++s) {
                const int if_gr = total - lna[s];
                if (if_gr < min_if_reduction || if_gr > max_if_reduction)
                    continue;
                if (chosen < 0 || lna[s] > lna[static_cast<size_t>(chosen)])
                    chosen = static_cast<int>(s);
            }
        }
        if (chosen >= 0) {
            const GainStep step{static_cast<unsigned>(chosen), total - lna[static_cast<size_t>(chosen)], total};
            if (ladder.empty() || ladder.back().reduction != step.reduction)
                ladder.push_back(step);
        }
        if (total == least)
            break;
    }
    return ladder;
}

size_t ladder_nearest(const std::vector<GainStep>& ladder, int reduction) {
    size_t best = 0;
    for (size_t i = 0; i < ladder.size(); ++i)
        if (std::abs(ladder[i].reduction - reduction) < std::abs(ladder[best].reduction - reduction))
            best = i;
    return best;
}

size_t ladder_start(const std::vector<GainStep>& ladder) {
    if (ladder.empty())
        return 0;
    return ladder_nearest(ladder, (ladder.front().reduction + ladder.back().reduction) / 2);
}

const char* if_mode_name(IfMode mode) { return mode == IfMode::low ? "low" : "zero"; }

std::optional<Failure> plan_rate(uint32_t rate, IfMode mode, int bandwidth_khz, RatePlan& out) {
    RatePlan plan;
    plan.output_hz = rate;
    if (mode == IfMode::low) {
        // Low IF: the converter runs at 6 MHz with the IF at 1.620 MHz, one
        // of the API's down-conversion cases (sdrplay_api_Init), which
        // delivers 2 MHz; decimation halves that up to five times.
        // SoapySDRPlay3 and SDR++ both use it so.
        for (unsigned d = 1; d <= 32; d *= 2) {
            if (static_cast<double>(rate) * d == 2000000.0) {
                plan.fs_hz = 6000000;
                plan.decimation = d;
                plan.if_khz = rsp::if_khz::if_1_620;
                break;
            }
        }
        if (plan.fs_hz == 0)
            return invalid("with module.if_mode = low the band's sample_rate must be 2000000, 1000000, 500000, 250000, "
                           "125000 or 62500, not " +
                           std::to_string(rate));
    } else if (rate >= min_fs_hz && rate <= max_fs_hz) {
        plan.fs_hz = rate;
    } else if (rate < min_fs_hz) {
        for (unsigned d = 2; d <= 32; d *= 2) {
            if (static_cast<double>(rate) * d >= min_fs_hz) {
                plan.fs_hz = static_cast<double>(rate) * d;
                plan.decimation = d;
                break;
            }
        }
        // As SoapySDRPlay3 does in zero IF.
        plan.wide_band = true;
    }
    if (plan.fs_hz == 0)
        return invalid("an RSP delivers 62500 to 10660000 samples a second; the band's sample_rate " +
                       std::to_string(rate) + " is outside that");

    const int widest = mode == IfMode::low ? 1536 : 8000;
    if (bandwidth_khz == 0) {
        plan.bandwidth_khz = bandwidths_khz[0];
        for (int bw : bandwidths_khz)
            if (bw * 1000.0 <= rate && bw <= widest)
                plan.bandwidth_khz = bw;
    } else {
        // A filter wider than the band lets what lies beyond its edges fold
        // into it.
        if (bandwidth_khz * 1000.0 > rate && bandwidth_khz != bandwidths_khz[0])
            return invalid("module.bandwidth " + std::to_string(bandwidth_khz) +
                           " kHz is wider than the band's sample rate, so signals outside the band would fold into "
                           "it; choose at most " +
                           std::to_string(rate / 1000) + " kHz, or auto");
        if (bandwidth_khz > widest)
            return invalid("with module.if_mode = low the IF filter can be at most 1536 kHz");
        plan.bandwidth_khz = bandwidth_khz;
    }
    out = plan;
    return std::nullopt;
}

}  // namespace fern
