// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "api.h"

#include <cmath>
#include <cstring>
#include <dlfcn.h>

namespace fern {

std::vector<std::string> default_library_candidates() {
    return {"libsdrplay_api.so.3", "libsdrplay_api.so", "/usr/local/lib/libsdrplay_api.so.3"};
}

bool api_version_supported(float version) {
    // ApiVersion returns the float the headers define as (float)(3.15), so
    // a small tolerance is all the comparison needs.
    return std::fabs(version - 3.14f) < 0.0005f || std::fabs(version - 3.15f) < 0.0005f;
}

Api::~Api() {
    // Never dlclose()d: the library starts threads of its own that may
    // outlive sdrplay_api_Close(), and unmapping their code would crash
    // them. The process is about to end whenever an Api goes away.
}

namespace {

template <typename Fn>
bool resolve(void* handle, const char* name, Fn& out) {
    // dlsym returns an object pointer; POSIX guarantees that a function
    // pointer survives the round trip.
    void* p = ::dlsym(handle, name);
    out = reinterpret_cast<Fn>(p);
    return p != nullptr;
}

}  // namespace

std::optional<Failure> Api::load() {
    if (handle_)
        return std::nullopt;
    std::string reasons;
    for (const std::string& candidate : candidates_) {
        ::dlerror();
        // RTLD_NOW finds a missing symbol here rather than in the middle of
        // streaming; RTLD_LOCAL keeps the library's own symbols to itself.
        void* h = ::dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (h) {
            handle_ = h;
            path_ = candidate;
            break;
        }
        const char* why = ::dlerror();
        if (!reasons.empty())
            reasons += "; ";
        reasons += why ? why : candidate + ": unknown error";
    }
    if (!handle_)
        return Failure{ErrorCode::internal,
                       "SDRplay's API is not installed on this machine (" + reasons +
                           "). Install SDRplay API 3.15 for Linux from " + api_download_url +
                           " (its installer also starts the sdrplay service), then start the band again"};

    struct Symbol {
        const char* name;
        bool ok;
    };
    const Symbol symbols[] = {
        {"sdrplay_api_Open", resolve(handle_, "sdrplay_api_Open", Open)},
        {"sdrplay_api_Close", resolve(handle_, "sdrplay_api_Close", Close)},
        {"sdrplay_api_ApiVersion", resolve(handle_, "sdrplay_api_ApiVersion", ApiVersion)},
        {"sdrplay_api_LockDeviceApi", resolve(handle_, "sdrplay_api_LockDeviceApi", LockDeviceApi)},
        {"sdrplay_api_UnlockDeviceApi", resolve(handle_, "sdrplay_api_UnlockDeviceApi", UnlockDeviceApi)},
        {"sdrplay_api_GetDevices", resolve(handle_, "sdrplay_api_GetDevices", GetDevices)},
        {"sdrplay_api_SelectDevice", resolve(handle_, "sdrplay_api_SelectDevice", SelectDevice)},
        {"sdrplay_api_ReleaseDevice", resolve(handle_, "sdrplay_api_ReleaseDevice", ReleaseDevice)},
        {"sdrplay_api_GetErrorString", resolve(handle_, "sdrplay_api_GetErrorString", GetErrorString)},
        {"sdrplay_api_GetLastError", resolve(handle_, "sdrplay_api_GetLastError", GetLastError)},
        {"sdrplay_api_GetDeviceParams", resolve(handle_, "sdrplay_api_GetDeviceParams", GetDeviceParams)},
        {"sdrplay_api_Init", resolve(handle_, "sdrplay_api_Init", Init)},
        {"sdrplay_api_Uninit", resolve(handle_, "sdrplay_api_Uninit", Uninit)},
        {"sdrplay_api_Update", resolve(handle_, "sdrplay_api_Update", Update)},
    };
    std::string missing;
    for (const Symbol& s : symbols) {
        if (s.ok)
            continue;
        if (!missing.empty())
            missing += ", ";
        missing += s.name;
    }
    if (!missing.empty()) {
        // Not unloaded, for the reason in the destructor; nothing is called.
        const std::string where = path_;
        handle_ = nullptr;
        return Failure{ErrorCode::internal, where + " lacks " + missing +
                                                ": it is not SDRplay's API 3.x. Reinstall SDRplay API 3.15 from " +
                                                api_download_url};
    }
    return std::nullopt;
}

std::string Api::error_text(rsp::ErrT err) const {
    const char* name = GetErrorString ? GetErrorString(err) : nullptr;
    return std::string(name && *name ? name : "error") + " (" + std::to_string(err) + ")";
}

std::string Api::last_error(rsp::DeviceT* device) const {
    if (!GetLastError)
        return {};
    const rsp::ErrorInfoT* info = GetLastError(device);
    if (!info)
        return {};
    // The library fills a fixed array; never trust it to be terminated.
    return std::string(info->message, strnlen(info->message, sizeof info->message));
}

}  // namespace fern
