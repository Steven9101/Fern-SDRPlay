// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// How many SDRplay devices are on the USB bus, as the kernel sees them.
// sdrplay_api_GetDevices leaves out an RSP that another process has
// selected, and the API has no other way to tell "unplugged" from "in use":
// more RSPs on the bus than the API lists means some are in use. RSPs carry
// no serial number in their USB descriptors, so the count is all there is.
#pragma once

#include <string>

namespace fern {

constexpr const char* default_usb_devices_dir = "/sys/bus/usb/devices";

// The number of USB devices with SDRplay's vendor id 1df7, or -1 when the
// directory cannot be read.
int count_rsps_on_usb(const std::string& usb_devices_dir);

}  // namespace fern
