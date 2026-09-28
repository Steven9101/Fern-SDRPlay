// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "fake_control.h"

#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#ifndef FAKE_LIBRARY
#error "FAKE_LIBRARY must be defined by the build"
#endif

namespace fake {

const char* library_path() { return FAKE_LIBRARY; }

namespace {

struct Library {
    State* (*state)() = nullptr;
    void (*reset)() = nullptr;
    Library() {
        void* h = ::dlopen(FAKE_LIBRARY, RTLD_NOW | RTLD_LOCAL);
        if (!h)
            throw std::runtime_error(std::string("cannot load the fake API: ") + ::dlerror());
        state = reinterpret_cast<State* (*)()>(::dlsym(h, "fake_sdrplay_state"));
        reset = reinterpret_cast<void (*)()>(::dlsym(h, "fake_sdrplay_reset"));
        if (!state || !reset)
            throw std::runtime_error("the fake API lacks its control functions");
    }
};

Library& library() {
    static Library l;
    return l;
}

}  // namespace

State& fresh() {
    library().reset();
    return *library().state();
}

fern::Api& api() {
    // The same handle every time, as the one process loads the one library.
    static fern::Api a({FAKE_LIBRARY});
    return a;
}

DeviceSpec device(unsigned char hw, const std::string& serial) {
    DeviceSpec d;
    d.hw = hw;
    d.serial = serial;
    return d;
}

namespace {

void write_file(const std::string& path, const char* text) {
    FILE* f = std::fopen(path.c_str(), "w");
    if (!f)
        throw std::runtime_error("cannot write " + path);
    std::fputs(text, f);
    std::fclose(f);
}

}  // namespace

UsbDir::UsbDir(int rsps) {
    char tmpl[] = "/tmp/fern-sdrplay-usb-XXXXXX";
    const char* dir = ::mkdtemp(tmpl);
    if (!dir)
        throw std::runtime_error("mkdtemp failed");
    path_ = dir;
    for (int i = 0; i < rsps; ++i) {
        const std::string d = path_ + "/1-" + std::to_string(i + 1);
        ::mkdir(d.c_str(), 0755);
        write_file(d + "/idVendor", "1df7\n");
    }
    const std::string hub = path_ + "/usb1";
    ::mkdir(hub.c_str(), 0755);
    write_file(hub + "/idVendor", "1d6b\n");
    ::mkdir((path_ + "/1-1:1.0").c_str(), 0755);
}

UsbDir::~UsbDir() {
    const std::string command = "rm -rf '" + path_ + "'";
    const int ignored = std::system(command.c_str());
    (void)ignored;
}

}  // namespace fake
