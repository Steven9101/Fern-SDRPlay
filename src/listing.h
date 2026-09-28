// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#pragma once

#include <chrono>
#include <string>

#include "api.h"
#include "json.h"
#include "watchdog.h"

namespace fern {

// FernSDR reads at most this much from --describe and --list-devices.
constexpr size_t max_report_bytes = 64 * 1024;

// Lists what sdrplay_api_GetDevices returns, without selecting anything.
// The API leaves out an RSP that another process has selected, so the
// report says so, and how many more RSPs the USB bus has than the API
// lists. Problems with the API go into the report's "error".
json::Value list_devices(Api& api, Watchdog& watchdog, std::chrono::steady_clock::time_point deadline,
                         const std::string& usb_devices_dir);

// Serializes a report, dropping devices until it fits into max_report_bytes
// including the newline.
std::string report_text(const json::Value& report);

}  // namespace fern
