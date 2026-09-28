// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "listing.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.h"
#include "models.h"
#include "presence.h"

namespace fern {

namespace {

constexpr const char* hidden_note =
    "RSPs in use by another program are not listed: the SDRplay API leaves out every RSP another process has "
    "selected, such as another FernSDR band";

json::Value with_error(json::Value report, const std::string& error) {
    report.set("error", error);
    return report;
}

}  // namespace

json::Value list_devices(Api& api, Watchdog& watchdog, std::chrono::steady_clock::time_point deadline,
                         const std::string& usb_devices_dir) {
    json::Value report = json::Value::object();
    report.set("devices", json::Value::array());
    report.set("note", hidden_note);
    if (auto f = api.load())
        return with_error(report, f->message);
    {
        Watchdog::Scope s(watchdog, "sdrplay_api_Open", deadline);
        const rsp::ErrT err = api.Open();
        if (s.expired() || err != rsp::err::success)
            return with_error(report, "the SDRplay API service is not running (sdrplay_api_Open: " +
                                          api.error_text(err) +
                                          "). Start it: sudo systemctl start sdrplay. If it runs, this process "
                                          "cannot see its shared memory in /dev/shm");
    }
    std::string error;
    rsp::DeviceT devices[rsp::max_devices];
    std::memset(devices, 0, sizeof devices);
    unsigned count = 0;
    float version = 0;
    {
        Watchdog::Scope s(watchdog, "sdrplay_api_ApiVersion", deadline);
        const rsp::ErrT err = api.ApiVersion(&version);
        if (s.expired())
            error = "sdrplay_api_ApiVersion did not return in time; another program may hold the API's lock";
        else if (err != rsp::err::success)
            error = "sdrplay_api_ApiVersion failed: " + api.error_text(err) +
                    (err == rsp::err::invalid_service_version
                         ? ". The library and the sdrplay service are different versions; reinstall SDRplay's API"
                         : "");
    }
    if (error.empty() && !api_version_supported(version)) {
        char text[200];
        std::snprintf(text, sizeof text,
                      "SDRplay API %.2f is installed; this module knows versions 3.14 and 3.15 only", version);
        error = text;
    }
    if (error.empty()) {
        char text[16];
        std::snprintf(text, sizeof text, "%.2f", static_cast<double>(version));
        report.set("api_version", text);
        rsp::ErrT err;
        {
            Watchdog::Scope s(watchdog, "sdrplay_api_LockDeviceApi", deadline);
            err = api.LockDeviceApi();
            if (s.expired())
                error = "sdrplay_api_LockDeviceApi did not return in time; another program may hold the API's lock";
        }
        if (error.empty() && err != rsp::err::success)
            error = "sdrplay_api_LockDeviceApi failed: " + api.error_text(err);
        if (error.empty()) {
            {
                Watchdog::Scope s(watchdog, "sdrplay_api_GetDevices", deadline);
                err = api.GetDevices(devices, &count, rsp::max_devices);
                if (err != rsp::err::success)
                    error = "sdrplay_api_GetDevices failed: " + api.error_text(err);
            }
            // Released on every path, with a second of its own.
            Watchdog::Scope s(watchdog, "sdrplay_api_UnlockDeviceApi",
                              std::max(deadline, std::chrono::steady_clock::now() + std::chrono::seconds(1)));
            (void)api.UnlockDeviceApi();
        }
    }
    {
        Watchdog::Scope s(watchdog, "sdrplay_api_Close", deadline + std::chrono::seconds(1));
        (void)api.Close();
    }
    if (!error.empty())
        return with_error(report, error);
    if (count > rsp::max_devices)
        count = rsp::max_devices;

    json::Value list = json::Value::array();
    for (unsigned i = 0; i < count; ++i) {
        const rsp::DeviceT& d = devices[i];
        const auto model = model_from_hw(d.hwVer);
        json::Value e = json::Value::object();
        e.set("index", i);
        e.set("name", model ? model_name(*model) : "unknown RSP");
        e.set("serial", std::string(d.SerNo, strnlen(d.SerNo, sizeof d.SerNo)));
        e.set("hw_version", d.hwVer);
        std::string why;
        if (!model)
            why = "hardware version " + std::to_string(d.hwVer) + " is not known to this module";
        else if (!d.valid)
            why = "the API reports it as not ready";
        else if (*model == Model::rspduo && !(d.rspDuoMode & (rsp::duo_mode::master | rsp::duo_mode::single_tuner)))
            why = "in use by another program as an RSPduo master; this module does not use slave mode";
        e.set("usable", why.empty());
        if (!why.empty())
            e.set("error", why);
        list.push(std::move(e));
    }
    report.set("devices", std::move(list));
    const int on_usb = count_rsps_on_usb(usb_devices_dir);
    if (on_usb >= 0) {
        report.set("on_usb", on_usb);
        if (static_cast<unsigned>(on_usb) > count)
            report.set("in_use_elsewhere", static_cast<unsigned>(on_usb) - count);
    }
    return report;
}

std::string report_text(const json::Value& report) {
    std::string text = json::serialize(report) + "\n";
    if (text.size() <= max_report_bytes)
        return text;
    std::vector<json::Value> kept;
    if (const json::Value* list = report.find("devices"))
        for (const json::Value& d : list->items())
            kept.push_back(d);
    for (;;) {
        json::Value out = json::Value::object();
        json::Value list = json::Value::array();
        for (const json::Value& d : kept)
            list.push(d);
        out.set("devices", std::move(list));
        for (const json::Member& m : report.members())
            if (m.key != "devices")
                out.set(m.key, m.value);
        text = json::serialize(out) + "\n";
        if (text.size() <= max_report_bytes || kept.empty())
            break;
        kept.pop_back();
    }
    log_line("the device list was shortened to fit into %zu bytes", max_report_bytes);
    return text;
}

}  // namespace fern
