// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include <mutex>

#include "fake_control.h"
#include "listing.h"
#include "test.h"

namespace r = fern::rsp;
using Clock = std::chrono::steady_clock;

namespace {

fern::json::Value list(const std::string& usb_dir, fern::Api& api = fake::api()) {
    fern::Watchdog watchdog([](const std::string&) {});
    return fern::list_devices(api, watchdog, Clock::now() + std::chrono::seconds(5), usb_dir);
}

}  // namespace

TEST(listing_shows_what_the_api_offers_without_selecting_anything) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "AAA"), fake::device(r::hw::rspduo, "BBB"),
                     fake::device(r::hw::rspdx_r2, "CCC"), fake::device(r::hw::rsp2, "DDD")};
    s.cfg.devices[1].duo_modes = r::duo_mode::slave;
    s.cfg.devices[3].busy = true;
    fake::UsbDir usb(4);
    const fern::json::Value report = list(usb.path());
    CHECK_EQ(fern::json::serialize(*report.find("devices")),
             std::string("[{\"index\":0,\"name\":\"RSP1A\",\"serial\":\"AAA\",\"hw_version\":255,\"usable\":true},"
                         "{\"index\":1,\"name\":\"RSPduo\",\"serial\":\"BBB\",\"hw_version\":3,\"usable\":false,"
                         "\"error\":\"in use by another program as an RSPduo master; this module does not use slave "
                         "mode\"},"
                         "{\"index\":2,\"name\":\"RSPdx-R2\",\"serial\":\"CCC\",\"hw_version\":7,\"usable\":true}]"));
    CHECK_HAS(report.find("note")->as_string(), "not listed");
    CHECK_EQ(report.find("on_usb")->as_number(), 4.0);
    CHECK_EQ(report.find("in_use_elsewhere")->as_number(), 1.0);
    CHECK_EQ(report.find("api_version")->as_string(), std::string("3.15"));
    std::lock_guard<std::mutex> lock(s.mutex);
    std::string calls;
    for (const std::string& c : s.calls)
        calls += c + " ";
    CHECK_EQ(calls, std::string("Open ApiVersion LockDeviceApi GetDevices UnlockDeviceApi Close "));
    CHECK(!s.locked);
}

TEST(listing_says_what_is_wrong_with_the_api) {
    fake::State& s = fake::fresh();
    s.cfg.open_error = r::err::fail;
    fern::json::Value report = list("/nonexistent");
    CHECK_HAS(report.find("error")->as_string(), "service is not running");
    CHECK(report.find("devices")->items().empty());
    CHECK(report.find("on_usb") == nullptr);

    fake::fresh().cfg.version = 3.07f;
    report = list("/nonexistent");
    CHECK_HAS(report.find("error")->as_string(), "3.07");
    CHECK(!s.opened);

    fern::Api missing({"/nonexistent/libsdrplay_api.so.3"});
    report = list("/nonexistent", missing);
    CHECK_HAS(report.find("error")->as_string(), "not installed");
    CHECK_EQ(fern::report_text(report).back(), '\n');
}
