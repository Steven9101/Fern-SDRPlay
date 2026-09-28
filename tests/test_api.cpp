// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "api.h"
#include "fake_control.h"
#include "test.h"

TEST(api_names_the_download_page_when_the_library_is_missing) {
    fern::Api api({"/nonexistent/libsdrplay_api.so.3", "libsdrplay_api_missing.so"});
    const auto f = api.load();
    REQUIRE(f);
    CHECK(f->code == fern::ErrorCode::internal);
    CHECK_HAS(f->message, "not installed");
    CHECK_HAS(f->message, "https://www.sdrplay.com/api/");
    CHECK_HAS(f->message, "/nonexistent/libsdrplay_api.so.3");
    CHECK(!api.loaded());
}

TEST(api_refuses_a_library_that_is_not_the_sdrplay_api) {
    fern::Api api({"libm.so.6"});
    const auto f = api.load();
    REQUIRE(f);
    CHECK_HAS(f->message, "lacks sdrplay_api_Open");
    CHECK_HAS(f->message, "not SDRplay's API 3.x");
    CHECK(!api.loaded());
}

TEST(api_loads_every_function_from_the_first_library_that_opens) {
    fern::Api api({"/nonexistent/libsdrplay_api.so.3", fake::library_path()});
    REQUIRE(!api.load());
    CHECK(api.loaded());
    CHECK_EQ(api.path(), std::string(fake::library_path()));
    CHECK(api.Open && api.Close && api.ApiVersion && api.LockDeviceApi && api.UnlockDeviceApi && api.GetDevices &&
          api.SelectDevice && api.ReleaseDevice && api.GetErrorString && api.GetLastError && api.GetDeviceParams &&
          api.Init && api.Uninit && api.Update);
    CHECK_EQ(api.error_text(fern::rsp::err::hw_error), std::string("sdrplay_api_HwError (7)"));
    CHECK_EQ(api.last_error(nullptr), std::string("the fake API's last error"));
}

TEST(api_accepts_the_versions_whose_layout_it_knows) {
    CHECK(fern::api_version_supported(3.15f));
    CHECK(fern::api_version_supported(3.14f));
    CHECK(!fern::api_version_supported(3.07f));
    CHECK(!fern::api_version_supported(3.16f));
    CHECK(!fern::api_version_supported(4.0f));
    CHECK(!fern::api_version_supported(0.0f));
}
