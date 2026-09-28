// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// Loads SDRplay's API library at run time. The module has no link-time
// dependency on it: SDRplay's licence does not allow passing the library
// on, so the operator installs it from SDRplay, and a module that the
// dynamic loader refused to start for want of it could not even say so.
// Loading it here also keeps --describe working on a machine without it.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "failure.h"
#include "rsp_api.h"

namespace fern {

// Where the operator gets the API.
constexpr const char* api_download_url = "https://www.sdrplay.com/api/";

// The library names tried in order: the soname SDRplay's installer
// registers with ldconfig, the development link, and the installer's own
// path for a system whose loader cache does not list /usr/local/lib.
std::vector<std::string> default_library_candidates();

// The API versions whose structure layout src/rsp_api.h matches.
bool api_version_supported(float version);

class Api {
public:
    explicit Api(std::vector<std::string> candidates = default_library_candidates())
        : candidates_(std::move(candidates)) {}
    ~Api();
    Api(const Api&) = delete;
    Api& operator=(const Api&) = delete;

    // Loads the library and every function the module uses. A failure says
    // what to install. Loading again after success does nothing.
    std::optional<Failure> load();
    bool loaded() const { return handle_ != nullptr; }
    const std::string& path() const { return path_; }

    // A readable text for an API error code, with the library's own name for
    // it when it is loaded.
    std::string error_text(rsp::ErrT err) const;
    // The library's extended message for the last error, or empty.
    std::string last_error(rsp::DeviceT* device) const;

    rsp::OpenFn Open = nullptr;
    rsp::CloseFn Close = nullptr;
    rsp::ApiVersionFn ApiVersion = nullptr;
    rsp::LockDeviceApiFn LockDeviceApi = nullptr;
    rsp::UnlockDeviceApiFn UnlockDeviceApi = nullptr;
    rsp::GetDevicesFn GetDevices = nullptr;
    rsp::SelectDeviceFn SelectDevice = nullptr;
    rsp::ReleaseDeviceFn ReleaseDevice = nullptr;
    rsp::GetErrorStringFn GetErrorString = nullptr;
    rsp::GetLastErrorFn GetLastError = nullptr;
    rsp::GetDeviceParamsFn GetDeviceParams = nullptr;
    rsp::InitFn Init = nullptr;
    rsp::UninitFn Uninit = nullptr;
    rsp::UpdateFn Update = nullptr;

private:
    std::vector<std::string> candidates_;
    void* handle_ = nullptr;
    std::string path_;
};

}  // namespace fern
