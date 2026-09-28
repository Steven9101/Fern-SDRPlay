// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// The module's own gain control, what gain = auto means: the highest step
// of the gain ladder (models.h) that keeps the RSP's converter out of
// clipping with room to spare. It is FernSDR's RTL-SDR and RX-888 modules'
// control, unchanged; for an RSP the steps are minus the gain reduction.
//
// A signal that keeps clipping is lost to every listener of the band, and
// the splatter of a converter driven past its limit lands all over the band,
// so a gain that keeps clipping comes down soon: a step once three tenths of
// a second in the last second clipped, 6 dB when those clipped heavily. A
// crash of static or a spark clips for a millisecond or so, which costs
// nobody much, while a gain lowered for it would cost the whole band its
// sensitivity for good: a tenth or two of a second that clip do not bring
// the gain down, and the gain still goes up past them. It goes up a step
// once nothing but such crashes has clipped for a while, and the peaks,
// all but the highest twentieth of them, would stay 6 dB under full scale
// one step higher. The while is 5 s in the first minutes, so that a band
// finds its gain soon after it starts, and a minute afterwards, so that a
// listener hears the level change rarely.
//
// The API's AGC (gain = agc) acts within milliseconds on the IF gain alone,
// which makes the level of a band pump with every strong signal.
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fern {

struct GainControlTiming {
    // Clipping is judged over windows of this length.
    std::chrono::milliseconds window{100};
    // After a change the USB transfers in flight still carry samples taken
    // at the old gain.
    std::chrono::milliseconds settle{1500};
    // Nothing but crashes clipped for this long before a step up: at first,
    // and later.
    std::chrono::milliseconds hold_start{5000};
    std::chrono::milliseconds hold{60000};
    std::chrono::milliseconds start_phase{120000};
};

class GainControl {
public:
    using Clock = std::chrono::steady_clock;

    // steps: the tuner's gains in tenths of a dB, ascending; start: the index
    // in use now. samples and clipped are the stream's counters at `now`.
    GainControl(std::vector<int> steps, size_t start, Clock::time_point now, uint64_t samples, uint64_t clipped,
                GainControlTiming timing = GainControlTiming());

    // One look at the stream: its counters, which only grow, and the largest
    // distance of I or Q from the converter's midpoint since the last look,
    // 0 to 128. Returns the step to use from now on; reason() says why it
    // changed.
    size_t update(Clock::time_point now, uint64_t samples, uint64_t clipped, unsigned peak);

    size_t step() const { return step_; }
    double gain_db() const { return steps_[step_] / 10.0; }
    const std::string& reason() const { return reason_; }
    // True while the lowest step keeps clipping: then only less signal helps,
    // a filter or an attenuator in front of the RSP.
    bool clipping_at_lowest() const { return clipping_at_lowest_; }

    // A window clips when more than this share of its samples do.
    static constexpr double clip_limit = 1e-4;
    // And clips heavily beyond this share.
    static constexpr double clip_heavy = 1e-2;
    // How many clipping windows of the last ten bring the gain down.
    static constexpr int clip_windows = 3;
    // A step up must leave the peaks under this distance from the midpoint:
    // 6 dB under full scale.
    static constexpr unsigned peak_limit = 64;

private:
    void change(size_t to, Clock::time_point now, std::string why);
    void start_hold(Clock::time_point now);

    std::vector<int> steps_;
    size_t step_;
    GainControlTiming timing_;
    Clock::time_point started_;
    Clock::time_point window_start_;
    Clock::time_point settled_;
    uint64_t window_samples_;
    uint64_t window_clipped_;
    unsigned window_peak_ = 0;
    // The shares clipped in the last ten windows, newest last.
    std::array<double, 10> recent_{};
    size_t recent_count_ = 0;
    size_t recent_next_ = 0;
    // The hold before a step up: when it began, how many windows it has seen
    // and how many of them clipped, and the peaks of the others.
    Clock::time_point hold_since_;
    unsigned hold_windows_ = 0;
    unsigned hold_clipping_ = 0;
    std::array<unsigned, 129> hold_peaks_{};
    bool clipping_at_lowest_ = false;
    std::string reason_;
};

}  // namespace fern
