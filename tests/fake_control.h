// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// The tests' handle on the fake API (fake_sdrplay.h), which the module
// loads by path like the real one, and a fake of the kernel's list of USB
// devices.
#pragma once

#include <string>

#include "api.h"
#include "fake_sdrplay.h"

namespace fake {

// The fake library, loaded, reset and configured afresh for one test.
State& fresh();
// An Api that loads the fake.
fern::Api& api();
const char* library_path();

// A directory laid out like /sys/bus/usb/devices with `rsps` SDRplay
// devices and one other device; removed when it goes.
class UsbDir {
public:
    explicit UsbDir(int rsps);
    ~UsbDir();
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

DeviceSpec device(unsigned char hw, const std::string& serial);

}  // namespace fake
