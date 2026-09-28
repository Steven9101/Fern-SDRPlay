// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include <algorithm>
#include <cstdlib>

#include "models.h"
#include "test.h"

using fern::Antenna;
using fern::Model;

namespace {

size_t states(Model m, Antenna a, double mhz, bool hdr = false) { return fern::lna_reductions(m, a, mhz * 1e6, hdr).size(); }

}  // namespace

// The state counts the API's headers define (RSPIA_NUM_LNA_STATES and the
// like, section 2 of the specification) must match the tables' lengths.
TEST(lna_tables_have_the_state_counts_of_the_specification) {
    for (Model m : {Model::rsp1a, Model::rspduo}) {
        CHECK_EQ(states(m, Antenna::a, 7), size_t(7));        // _AM
        CHECK_EQ(states(m, Antenna::a, 145), size_t(10));     // all bands
        CHECK_EQ(states(m, Antenna::a, 433), size_t(10));
        CHECK_EQ(states(m, Antenna::a, 1296), size_t(9));     // _LBAND
    }
    CHECK_EQ(states(Model::rsp1b, Antenna::a, 55), size_t(10));  // its AM row ends at 50 MHz
    CHECK_EQ(states(Model::rsp1b, Antenna::a, 45), size_t(7));
    CHECK_EQ(states(Model::rspduo, Antenna::hiz, 7), size_t(5));  // RSPDUO_NUM_LNA_STATES_AMPORT
    CHECK_EQ(states(Model::rsp2, Antenna::a, 14), size_t(9));
    CHECK_EQ(states(Model::rsp2, Antenna::hiz, 14), size_t(5));
    CHECK_EQ(states(Model::rsp2, Antenna::b, 433), size_t(6));  // _420MHZ
    CHECK_EQ(states(Model::rsp1, Antenna::a, 14), size_t(4));
    for (Model m : {Model::rspdx, Model::rspdx_r2}) {
        CHECK_EQ(states(m, Antenna::a, 7), size_t(19));    // _AMPORT2_0_12
        CHECK_EQ(states(m, Antenna::a, 28), size_t(20));   // _AMPORT2_12_50
        CHECK_EQ(states(m, Antenna::a, 55), size_t(25));   // _AMPORT2_50_60
        CHECK_EQ(states(m, Antenna::a, 145), size_t(27));  // _VHF_BAND3
        CHECK_EQ(states(m, Antenna::a, 300), size_t(28));  // all bands
        CHECK_EQ(states(m, Antenna::a, 433), size_t(21));  // _420MHZ
        CHECK_EQ(states(m, Antenna::a, 1296), size_t(19)); // _LBAND
        CHECK_EQ(states(m, Antenna::a, 0.475, true), size_t(22));  // _DX, HDR
    }
    CHECK_EQ(fern::lna_reductions(Model::rsp1a, Antenna::a, 7e6, false).back(), 61);
    CHECK_EQ(fern::lna_reductions(Model::rspdx, Antenna::a, 300e6, false).back(), 84);
}

TEST(every_gain_ladder_climbs_in_small_steps_within_the_api_limits) {
    const Model models[] = {Model::rsp1, Model::rsp1a, Model::rsp1b, Model::rsp2, Model::rspduo, Model::rspdx,
                            Model::rspdx_r2};
    const double freqs[] = {0.2e6, 7e6, 14e6, 28e6, 55e6, 145e6, 300e6, 433e6, 1296e6};
    for (Model m : models) {
        for (double f : freqs) {
            for (Antenna a : {Antenna::a, Antenna::hiz}) {
                if (a == Antenna::hiz && (f > 60e6 || (m != Model::rsp2 && m != Model::rspduo)))
                    continue;
                const std::vector<int> lna = fern::lna_reductions(m, a, f, false);
                const std::vector<fern::GainStep> ladder = fern::gain_ladder(lna);
                REQUIRE(ladder.size() >= 10);
                const int most = *std::max_element(lna.begin(), lna.end()) + fern::max_if_reduction;
                const int least = *std::min_element(lna.begin(), lna.end()) + fern::min_if_reduction;
                CHECK_EQ(ladder.front().reduction, most);
                CHECK_EQ(ladder.back().reduction, least);
                for (size_t i = 0; i < ladder.size(); ++i) {
                    const fern::GainStep& s = ladder[i];
                    REQUIRE(s.lna_state < lna.size());
                    CHECK_EQ(lna[s.lna_state] + s.if_reduction, s.reduction);
                    CHECK(s.if_reduction >= fern::min_if_reduction && s.if_reduction <= fern::max_if_reduction);
                    if (i > 0) {
                        // Strictly more gain each step, never more than 3 dB.
                        CHECK(s.reduction < ladder[i - 1].reduction);
                        CHECK(ladder[i - 1].reduction - s.reduction <= 3);
                    }
                }
                const size_t start = fern::ladder_start(ladder);
                CHECK(start > 0 && start + 1 < ladder.size());
            }
        }
    }
}

TEST(gain_ladder_keeps_the_lna_gain_high_while_the_if_has_room) {
    const std::vector<int> lna = fern::lna_reductions(Model::rsp1a, Antenna::a, 145e6, false);
    const std::vector<fern::GainStep> ladder = fern::gain_ladder(lna);
    // A total of 40 dB or less needs no LNA reduction at all.
    for (const fern::GainStep& s : ladder)
        if (s.reduction <= 40)
            CHECK_EQ(s.lna_state, 0u);
    CHECK(std::abs(ladder[fern::ladder_nearest(ladder, 41)].reduction - 41) <= 1);
    CHECK_EQ(ladder[fern::ladder_nearest(ladder, 1000)].reduction, ladder.front().reduction);
}

TEST(antennas_resolve_per_model_and_refuse_what_a_model_lacks) {
    Antenna out;
    CHECK(!fern::resolve_antenna(Model::rsp1a, Antenna::automatic, 7e6, out));
    CHECK(out == Antenna::a);
    CHECK(fern::resolve_antenna(Model::rsp1a, Antenna::b, 7e6, out));
    CHECK(!fern::resolve_antenna(Model::rspduo, Antenna::automatic, 7e6, out));
    CHECK(out == Antenna::tuner1);
    CHECK(!fern::resolve_antenna(Model::rspduo, Antenna::tuner2, 7e6, out));
    CHECK(fern::resolve_antenna(Model::rspduo, Antenna::c, 7e6, out));
    CHECK(fern::resolve_antenna(Model::rsp2, Antenna::hiz, 145e6, out));  // Hi-Z up to 60 MHz
    CHECK(!fern::resolve_antenna(Model::rsp2, Antenna::hiz, 14e6, out));
    CHECK(!fern::resolve_antenna(Model::rspdx_r2, Antenna::c, 145e6, out));
    CHECK(fern::resolve_antenna(Model::rspdx, Antenna::c, 433e6, out));  // C up to 200 MHz
    CHECK(fern::resolve_antenna(Model::rspdx, Antenna::tuner1, 7e6, out));
    CHECK(fern::resolve_antenna(Model::rsp1, Antenna::a, 5e3, out));    // the RSP1 from 10 kHz
    CHECK(!fern::resolve_antenna(Model::rsp1a, Antenna::a, 5e3, out));
    CHECK(fern::resolve_antenna(Model::rsp1a, Antenna::a, 2100e6, out));
}

TEST(switches_exist_where_the_hardware_has_them) {
    CHECK(!fern::has_bias_tee(Model::rsp1, Antenna::a));
    CHECK(fern::has_bias_tee(Model::rsp1b, Antenna::a));
    CHECK(fern::has_bias_tee(Model::rsp2, Antenna::b));
    CHECK(!fern::has_bias_tee(Model::rsp2, Antenna::hiz));
    CHECK(fern::has_bias_tee(Model::rspduo, Antenna::tuner2));
    CHECK(!fern::has_bias_tee(Model::rspduo, Antenna::tuner1));
    CHECK(fern::has_bias_tee(Model::rspdx_r2, Antenna::b));
    CHECK(!fern::has_bias_tee(Model::rspdx_r2, Antenna::a));
    CHECK(!fern::has_rf_notch(Model::rsp1));
    CHECK(fern::has_rf_notch(Model::rsp2));
    CHECK(!fern::has_dab_notch(Model::rsp2));
    CHECK(fern::has_dab_notch(Model::rspdx));
    CHECK(fern::has_am_notch(Model::rspduo, Antenna::hiz));
    CHECK(!fern::has_am_notch(Model::rspduo, Antenna::tuner2));
    CHECK(fern::has_hdr(Model::rspdx_r2));
    CHECK(!fern::has_hdr(Model::rsp1a));
    CHECK(fern::is_hdr_frequency(475000));
    CHECK(!fern::is_hdr_frequency(476000));
}

TEST(rate_plans_deliver_exactly_the_band_rate) {
    fern::RatePlan p;
    REQUIRE(!fern::plan_rate(2000000, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.fs_hz, 2000000.0);
    CHECK_EQ(p.decimation, 1u);
    CHECK_EQ(p.bandwidth_khz, 1536);
    CHECK_EQ(p.output_hz, 2000000.0);
    REQUIRE(!fern::plan_rate(8000000, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.bandwidth_khz, 8000);
    REQUIRE(!fern::plan_rate(6000000, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.bandwidth_khz, 6000);
    REQUIRE(!fern::plan_rate(10660000, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.fs_hz, 10660000.0);
    // Below 2 MHz the converter runs at a power of two times the rate.
    REQUIRE(!fern::plan_rate(192000, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.decimation, 16u);
    CHECK_EQ(p.fs_hz, 3072000.0);
    CHECK_EQ(p.fs_hz / p.decimation, 192000.0);
    CHECK_EQ(p.bandwidth_khz, 200);
    CHECK(p.wide_band);
    REQUIRE(!fern::plan_rate(1000000, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.decimation, 2u);
    CHECK_EQ(p.bandwidth_khz, 600);
    REQUIRE(!fern::plan_rate(62500, fern::IfMode::zero, 0, p));
    CHECK_EQ(p.decimation, 32u);
    CHECK(fern::plan_rate(62499, fern::IfMode::zero, 0, p));
    CHECK(fern::plan_rate(10660001, fern::IfMode::zero, 0, p));

    REQUIRE(!fern::plan_rate(2000000, fern::IfMode::low, 0, p));
    CHECK_EQ(p.fs_hz, 6000000.0);
    CHECK_EQ(p.if_khz, 1620);
    CHECK_EQ(p.decimation, 1u);
    CHECK_EQ(p.bandwidth_khz, 1536);
    REQUIRE(!fern::plan_rate(250000, fern::IfMode::low, 0, p));
    CHECK_EQ(p.decimation, 8u);
    CHECK_EQ(p.bandwidth_khz, 200);
    CHECK(!p.wide_band);
    CHECK(fern::plan_rate(3000000, fern::IfMode::low, 0, p));
    CHECK(fern::plan_rate(192000, fern::IfMode::low, 0, p));

    REQUIRE(!fern::plan_rate(8000000, fern::IfMode::zero, 5000, p));
    CHECK_EQ(p.bandwidth_khz, 5000);
    CHECK(fern::plan_rate(2000000, fern::IfMode::zero, 5000, p));  // wider than the band
    CHECK(fern::plan_rate(2000000, fern::IfMode::low, 5000, p));
}
