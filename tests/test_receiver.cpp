// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// The receiver against the fake API: the order of the calls, what lands in
// the parameter structs per model, device selection, live changes and the
// clean-up on every failure.
#include <memory>
#include <mutex>
#include <unistd.h>

#include "fake_control.h"
#include "receiver.h"
#include "test.h"

namespace r = fern::rsp;
using Clock = std::chrono::steady_clock;

namespace {

struct Rig {
    std::mutex mutex;
    std::vector<std::string> hangs;
    fern::Watchdog watchdog{[this](const std::string& what) {
        std::lock_guard<std::mutex> lock(mutex);
        hangs.push_back(what);
    }};
    int fds[2] = {-1, -1};
    // Before the receiver: the API calls into it until Uninit.
    std::unique_ptr<fern::Stream> stream;
    fern::Receiver receiver;

    explicit Rig(const std::string& usb_dir = "/nonexistent")
        : receiver(fake::api(), watchdog, usb_dir) {
        if (::pipe(fds) != 0)
            throw std::runtime_error("pipe");
        stream = std::make_unique<fern::Stream>(fds[1], -1, 1 << 20, 1);
    }
    ~Rig() {
        receiver.close_by(Clock::now() + std::chrono::seconds(3));
        ::close(fds[0]);
        ::close(fds[1]);
    }

    std::optional<fern::Failure> open(const std::string& settings, double center = 7100000,
                                      uint32_t rate = 2000000, int deadline_ms = 5000) {
        fern::json::Value v;
        std::string error;
        const std::string text = "{\"type\":\"open\",\"signal\":\"iq\",\"sample_rate\":" + std::to_string(rate) +
                                 ",\"center\":" + std::to_string(static_cast<long long>(center)) +
                                 ",\"settings\":" + settings + "}";
        if (!fern::json::parse(text, v, error))
            return fern::Failure{fern::ErrorCode::internal, "bad test JSON " + error};
        fern::OpenRequest request;
        if (auto f = fern::parse_open(v, request))
            return f;
        return receiver.open(request, *stream, Clock::now() + std::chrono::milliseconds(deadline_ms));
    }
};

std::string calls(fake::State& s) {
    std::lock_guard<std::mutex> lock(s.mutex);
    std::string out;
    for (const std::string& c : s.calls)
        out += (out.empty() ? "" : " ") + c;
    return out;
}

bool called(fake::State& s, const std::string& name) { return calls(s).find(name) != std::string::npos; }

// Every failure leaves the API as it found it.
void check_cleaned_up(fake::State& s) {
    const std::string c = calls(s);
    CHECK(!s.locked);
    CHECK(!s.initialised);
    CHECK_EQ(s.selected, -1);
    CHECK(c.size() >= 5 && c.compare(c.size() - 5, 5, "Close") == 0);
    // The lock, where it was taken, is given back before the API closes.
    const size_t unlock = c.find("UnlockDeviceApi");
    if (unlock != std::string::npos)
        CHECK(unlock < c.rfind("Close"));
}

}  // namespace

TEST(receiver_opens_in_the_order_the_specification_gives_and_closes_the_same_way) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "1A0001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{}"));
        CHECK_EQ(calls(s),
                 std::string("Open ApiVersion LockDeviceApi GetDevices SelectDevice UnlockDeviceApi GetDeviceParams Init"));
        CHECK(s.listed_while_locked);
        CHECK(s.selected_while_locked);
        CHECK(!s.locked);
        CHECK_EQ(s.init_dev.fsFreq.fsHz, 2000000.0);
        CHECK_EQ(s.init_channel.tunerParams.rfFreq.rfHz, 7100000.0);
        CHECK_EQ(s.init_channel.tunerParams.bwType, 1536);
        CHECK_EQ(s.init_channel.tunerParams.ifType, 0);
        CHECK_EQ(s.init_channel.tunerParams.gain.minGr, r::min_gr::normal);
        CHECK_EQ(s.init_channel.ctrlParams.decimation.enable, 0);
        // The API's AGC, on by default, is off unless asked for.
        CHECK_EQ(s.init_channel.ctrlParams.agc.enable, r::agc::disable);
        const fern::GainStep& start = rig.receiver.ladder()[rig.receiver.ladder_step()];
        CHECK_EQ(static_cast<unsigned>(s.init_channel.tunerParams.gain.LNAstate), start.lna_state);
        CHECK_EQ(s.init_channel.tunerParams.gain.gRdB, start.if_reduction);
        CHECK_EQ(rig.receiver.identity().serial, std::string("1A0001"));
        CHECK(rig.receiver.identity().model == fern::Model::rsp1a);
        CHECK_EQ(rig.receiver.lna_table().size(), size_t(7));
        CHECK(rig.receiver.is_open());
        CHECK(rig.receiver.close_by(Clock::now() + std::chrono::seconds(2)));
    }
    const std::string c = calls(s);
    CHECK_HAS(c, "Init Uninit ReleaseDevice Close");
    CHECK(!s.opened);
}

TEST(receiver_writes_each_models_inputs_and_switches) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rspdx_r2, "DX0001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"antenna\":\"b\",\"bias_tee\":true,\"rf_notch\":true,\"dab_notch\":true,\"hdr\":true}",
                          475000));
        CHECK_EQ(s.init_dev.rspDxParams.antennaSel, r::rspdx_antenna::b);
        CHECK_EQ(s.init_dev.rspDxParams.biasTEnable, 1);
        CHECK_EQ(s.init_dev.rspDxParams.rfNotchEnable, 1);
        CHECK_EQ(s.init_dev.rspDxParams.rfDabNotchEnable, 1);
        CHECK_EQ(s.init_dev.rspDxParams.hdrEnable, 1);
        CHECK_EQ(rig.receiver.lna_table().size(), size_t(22));
    }
    fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp2, "R20001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"antenna\":\"hiz\"}"));
        CHECK_EQ(s.init_channel.rsp2TunerParams.amPortSel, r::am_port::port1);
        CHECK_EQ(rig.receiver.lna_table().size(), size_t(5));
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp2, "R20001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"antenna\":\"b\",\"bias_tee\":true,\"rf_notch\":true}", 145000000));
        CHECK_EQ(s.init_channel.rsp2TunerParams.antennaSel, r::rsp2_antenna::b);
        CHECK_EQ(s.init_channel.rsp2TunerParams.amPortSel, r::am_port::port2);
        CHECK_EQ(s.init_channel.rsp2TunerParams.biasTEnable, 1);
        CHECK_EQ(s.init_channel.rsp2TunerParams.rfNotchEnable, 1);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rspduo, "DU0001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"antenna\":\"tuner2\",\"bias_tee\":true,\"dab_notch\":true}"));
        CHECK_EQ(s.selected_tuner, r::tuner::b);
        CHECK_EQ(s.selected_mode, r::duo_mode::single_tuner);
        CHECK_EQ(s.b.rspDuoTunerParams.biasTEnable, 1);
        CHECK_EQ(s.b.rspDuoTunerParams.rfDabNotchEnable, 1);
        CHECK_EQ(s.a.rspDuoTunerParams.biasTEnable, 0);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rspduo, "DU0001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"antenna\":\"hiz\",\"am_notch\":true}"));
        CHECK_EQ(s.selected_tuner, r::tuner::a);
        CHECK_EQ(s.init_channel.rspDuoTunerParams.tuner1AmPortSel, r::am_port::port1);
        CHECK_EQ(s.init_channel.rspDuoTunerParams.tuner1AmNotchEnable, 1);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1b, "1B0001")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"bias_tee\":true,\"rf_notch\":true,\"gain\":\"agc\",\"lna_state\":2,\"ppm\":1.25,"
                          "\"if_mode\":\"low\"}",
                          7100000, 500000));
        CHECK_EQ(s.init_channel.rsp1aTunerParams.biasTEnable, 1);
        CHECK_EQ(s.init_dev.rsp1aParams.rfNotchEnable, 1);
        CHECK_EQ(s.init_channel.ctrlParams.agc.enable, r::agc::ctrl_en);
        CHECK_EQ(s.init_channel.ctrlParams.agc.setPoint_dBfs, -30);
        CHECK_EQ(static_cast<unsigned>(s.init_channel.tunerParams.gain.LNAstate), 2u);
        CHECK_EQ(s.init_dev.ppm, 1.25);
        CHECK_EQ(s.init_dev.fsFreq.fsHz, 6000000.0);
        CHECK_EQ(s.init_channel.tunerParams.ifType, 1620);
        CHECK_EQ(s.init_channel.ctrlParams.decimation.enable, 1);
        CHECK_EQ(s.init_channel.ctrlParams.decimation.decimationFactor, 4);
        CHECK_EQ(s.output_rate, 500000.0);
    }
}

TEST(receiver_refuses_a_switch_the_model_lacks_before_taking_the_rsp) {
    struct Case {
        unsigned char hw;
        const char* settings;
        const char* says;
    };
    const Case cases[] = {
        {r::hw::rsp1, "{\"bias_tee\":true}", "has no bias tee"},
        {r::hw::rsp1, "{\"rf_notch\":true}", "no broadcast notch"},
        {r::hw::rsp2, "{\"dab_notch\":true}", "no DAB notch"},
        {r::hw::rspdx, "{\"bias_tee\":true}", "on input b"},
        {r::hw::rspduo, "{\"bias_tee\":true}", "on tuner2"},
        {r::hw::rsp1a, "{\"am_notch\":true}", "tuner 1 MW notch"},
        {r::hw::rsp1a, "{\"hdr\":true}", "RSPdx and RSPdx-R2"},
        {r::hw::rsp1a, "{\"antenna\":\"b\"}", "one antenna input"},
        {r::hw::rsp1a, "{\"gain\":\"manual\",\"lna_state\":7}", "7 LNA states"},
    };
    for (const Case& c : cases) {
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(c.hw, "S1")};
        Rig rig;
        const auto f = rig.open(c.settings, std::string(c.settings) == "{\"hdr\":true}" ? 475000 : 7100000);
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::invalid);
        CHECK_HAS(f->message, c.says);
        CHECK(!called(s, "SelectDevice"));
        check_cleaned_up(s);
    }
}

TEST(receiver_selects_by_serial_and_index_and_says_what_is_wrong_otherwise) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA"), fake::device(r::hw::rspdx, "BBB")};
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::no_device);
        CHECK_HAS(f->message, "2 RSPs are plugged in");
        CHECK_HAS(f->message, "RSP1A AAA, RSPdx BBB");
        check_cleaned_up(s);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA"), fake::device(r::hw::rspdx, "BBB")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"device\":\"serial:BBB\"}"));
        CHECK(rig.receiver.identity().model == fern::Model::rspdx);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA"), fake::device(r::hw::rspdx, "BBB")};
    {
        Rig rig;
        REQUIRE(!rig.open("{\"device\":\"index:0\"}"));
        CHECK_EQ(rig.receiver.identity().serial, std::string("AAA"));
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    {
        Rig rig;
        const auto f = rig.open("{\"device\":\"serial:CCC\"}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::no_device);
        CHECK_HAS(f->message, "no RSP with serial CCC");
        check_cleaned_up(s);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    {
        Rig rig;
        const auto f = rig.open("{\"device\":\"index:1\"}");
        REQUIRE(f);
        CHECK_HAS(f->message, "Indexes change");
    }
    fake::fresh();
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::no_device);
        CHECK_HAS(f->message, "no RSP is plugged in");
        check_cleaned_up(s);
    }
}

TEST(receiver_tells_an_rsp_in_use_elsewhere_from_a_missing_one) {
    // Two on the bus, one selected by another process: the API lists one.
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA"), fake::device(r::hw::rsp1a, "BBB")};
    s.cfg.devices[1].busy = true;
    fake::UsbDir usb(2);
    {
        Rig rig(usb.path());
        const auto f = rig.open("{\"device\":\"serial:BBB\"}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::busy);
        CHECK_HAS(f->message, "in use by another program");
        check_cleaned_up(s);
    }
    {
        // Without a serial the free one must not be taken silently.
        Rig rig(usb.path());
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::no_device);
        CHECK_HAS(f->message, "2 RSPs are plugged in");
        CHECK_HAS(f->message, "1 more RSP is plugged in but in use");
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    s.cfg.devices[0].busy = true;
    fake::UsbDir one(1);
    {
        Rig rig(one.path());
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::busy);
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rspduo, "DU1")};
    s.cfg.devices[0].duo_modes = r::duo_mode::slave;
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::busy);
        CHECK_HAS(f->message, "slave mode");
        CHECK(!called(s, "SelectDevice"));
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    s.cfg.devices[0].valid = false;
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::usb);
        CHECK_HAS(f->message, "not ready");
    }
    fake::fresh().cfg.devices = {fake::device(9, "NEW")};
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK_HAS(f->message, "hardware version 9");
    }
}

TEST(receiver_refuses_an_api_version_whose_layout_it_does_not_know) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    s.cfg.version = 3.07f;
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK(f->code == fern::ErrorCode::internal);
        CHECK_HAS(f->message, "SDRplay API 3.07 is installed");
        CHECK_EQ(calls(s), std::string("Open ApiVersion Close"));
    }
    fake::fresh().cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    s.cfg.version = 3.14f;
    {
        Rig rig;
        CHECK(!rig.open("{}"));
    }
    fake::fresh().cfg.version_error = r::err::invalid_service_version;
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK_HAS(f->message, "different versions");
        check_cleaned_up(s);
    }
    fake::fresh().cfg.open_error = r::err::fail;
    {
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK_HAS(f->message, "service is not running");
        CHECK_HAS(f->message, "systemctl start sdrplay");
        CHECK_EQ(calls(s), std::string("Open"));
    }
}

TEST(receiver_cleans_up_after_an_error_from_any_call) {
    struct Case {
        r::ErrT fake::Config::*field;
        r::ErrT err;
        const char* call;
        fern::ErrorCode code;
    };
    const Case cases[] = {
        {&fake::Config::lock_error, r::err::fail, "sdrplay_api_LockDeviceApi", fern::ErrorCode::usb},
        {&fake::Config::get_devices_error, r::err::service_not_responding, "sdrplay_api_GetDevices",
         fern::ErrorCode::internal},
        {&fake::Config::select_error, r::err::hw_error, "sdrplay_api_SelectDevice", fern::ErrorCode::usb},
        {&fake::Config::params_error, r::err::fail, "sdrplay_api_GetDeviceParams", fern::ErrorCode::usb},
        {&fake::Config::init_error, r::err::hw_error, "sdrplay_api_Init", fern::ErrorCode::usb},
        {&fake::Config::init_error, r::err::out_of_range, "refused the settings", fern::ErrorCode::invalid},
    };
    for (const Case& c : cases) {
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
        s.cfg.*(c.field) = c.err;
        Rig rig;
        const auto f = rig.open("{}");
        REQUIRE(f);
        CHECK_HAS(f->message, c.call);
        CHECK(f->code == c.code);
        check_cleaned_up(s);
        if (c.field == &fake::Config::init_error || c.field == &fake::Config::params_error)
            CHECK(called(s, "ReleaseDevice"));
    }
}

TEST(receiver_reports_a_hung_call_and_still_releases_the_lock) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    s.cfg.lock_ms = 600;
    Rig rig;
    const auto f = rig.open("{}", 7100000, 2000000, 150);
    REQUIRE(f);
    CHECK(f->code == fern::ErrorCode::busy);
    CHECK_HAS(f->message, "sdrplay_api_LockDeviceApi did not return in time");
    {
        std::lock_guard<std::mutex> lock(rig.mutex);
        REQUIRE(rig.hangs.size() == 1);
        CHECK_EQ(rig.hangs[0], std::string("sdrplay_api_LockDeviceApi"));
    }
    // The lock it got late is given back, and the API closed.
    check_cleaned_up(s);
}

TEST(receiver_applies_live_changes_with_the_matching_update_reasons) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA")};
    Rig rig;
    REQUIRE(!rig.open("{\"gain\":\"manual\",\"lna_state\":1,\"if_gain_reduction\":40}"));
    const auto last = [&] {
        std::lock_guard<std::mutex> lock(s.mutex);
        return s.updates.empty() ? fake::UpdateRecord{} : s.updates.back();
    };

    fern::LiveChange c;
    c.lna_state = 3;
    c.if_gain_reduction = 30;
    REQUIRE(!rig.receiver.apply(c));
    CHECK_EQ(last().reason, r::update::tuner_gr);
    CHECK_EQ(last().lna_state, 3u);
    CHECK_EQ(last().if_reduction, 30);

    fern::LiveChange bias;
    bias.bias_tee = true;
    bias.rf_notch = true;
    bias.ppm = -0.5;
    REQUIRE(!rig.receiver.apply(bias));
    CHECK_EQ(last().reason, r::update::rsp1a_bias_t | r::update::rsp1a_rf_notch | r::update::dev_ppm);
    CHECK_EQ(s.a.rsp1aTunerParams.biasTEnable, 1);
    CHECK_EQ(s.dev.ppm, -0.5);

    fern::LiveChange agc;
    agc.gain = fern::GainMode::agc;
    REQUIRE(!rig.receiver.apply(agc));
    CHECK_EQ(last().reason, r::update::ctrl_agc);
    CHECK_EQ(last().agc, r::agc::ctrl_en);

    fern::LiveChange wrong;
    wrong.if_gain_reduction = 25;
    CHECK_HAS(rig.receiver.apply(wrong)->message, "AGC");

    fern::LiveChange automatic;
    automatic.gain = fern::GainMode::automatic;
    const size_t updates_before = s.updates.size();
    REQUIRE(!rig.receiver.apply(automatic));
    CHECK(s.updates.size() == updates_before + 1);
    CHECK((last().reason & r::update::ctrl_agc) != 0);
    CHECK_EQ(last().agc, r::agc::disable);
    // From the step nearest the 18 + 30 dB in use.
    const fern::GainStep& step = rig.receiver.ladder()[rig.receiver.ladder_step()];
    CHECK(std::abs(step.reduction - 48) <= 1);

    fern::LiveChange by_hand;
    by_hand.lna_state = 2;
    CHECK_HAS(rig.receiver.apply(by_hand)->message, "module.gain is auto");

    fern::LiveChange dab;
    dab.dab_notch = true;
    REQUIRE(!rig.receiver.apply(dab));
    CHECK_EQ(last().reason, r::update::rsp1a_rf_dab_notch);
    fern::LiveChange am;
    am.am_notch = true;
    CHECK_HAS(rig.receiver.apply(am)->message, "tuner 1 MW notch");

    // An update the API refuses puts the fields back.
    s.cfg.update_error = r::err::gain_update_error;
    s.cfg.update_error_on = r::update::tuner_gr;
    const int before = s.a.tunerParams.gain.gRdB;
    const size_t other = rig.receiver.ladder_step() == 0 ? 1 : 0;
    const auto f = rig.receiver.set_ladder_step(other);
    REQUIRE(f);
    CHECK(f->code == fern::ErrorCode::usb);
    CHECK_EQ(s.a.tunerParams.gain.gRdB, before);
    CHECK(rig.receiver.ladder_step() != other);
}

TEST(receiver_moves_along_the_ladder_and_retunes_with_the_new_band_table) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rspdx, "DX1")};
    Rig rig;
    REQUIRE(!rig.open("{}"));
    const size_t step = rig.receiver.ladder_step();
    REQUIRE(!rig.receiver.set_ladder_step(step + 1));
    const fern::GainStep& g = rig.receiver.ladder()[step + 1];
    CHECK_EQ(static_cast<unsigned>(s.a.tunerParams.gain.LNAstate), g.lna_state);
    CHECK_EQ(s.a.tunerParams.gain.gRdB, g.if_reduction);
    CHECK_EQ(s.updates.back().reason, r::update::tuner_gr);

    REQUIRE(rig.receiver.lna_table().size() == 19);
    REQUIRE(!rig.receiver.retune(145000000));
    CHECK_EQ(rig.receiver.lna_table().size(), size_t(27));
    CHECK_EQ(s.a.tunerParams.rfFreq.rfHz, 145000000.0);
    CHECK((s.updates.back().reason & r::update::tuner_frf) != 0);
    CHECK_EQ(rig.receiver.effective().center, 145000000.0);
    CHECK(rig.receiver.retune(3e9));
    CHECK_EQ(rig.receiver.effective().center, 145000000.0);

    fern::json::Value applied = rig.receiver.settings_json();
    CHECK_EQ(applied.find("gain")->as_string(), std::string("auto"));
    CHECK_EQ(applied.find("lna_states")->as_number(), 27.0);
    CHECK_EQ(rig.receiver.device_json().find("name")->as_string(), std::string("RSPdx"));
    CHECK_EQ(rig.receiver.device_json().find("api_version")->as_string(), std::string("3.15"));
}
