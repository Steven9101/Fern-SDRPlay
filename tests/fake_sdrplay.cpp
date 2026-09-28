// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "fake_sdrplay.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <sstream>

using namespace fake;

namespace {

State& state() {
    static State* s = [] {
        State* st = new State;
        // A program run by the command line tests is configured through the
        // environment; the unit tests set cfg directly.
        if (const char* spec = std::getenv("FAKE_SDRPLAY")) {
            std::stringstream all(spec);
            std::string item;
            while (std::getline(all, item, ';')) {
                const size_t eq = item.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key = item.substr(0, eq);
                const std::string value = item.substr(eq + 1);
                if (key == "devices") {
                    std::stringstream list(value);
                    std::string d;
                    while (std::getline(list, d, ',')) {
                        const size_t colon = d.find(':');
                        DeviceSpec spec_d;
                        spec_d.hw = static_cast<unsigned char>(std::atoi(d.substr(0, colon).c_str()));
                        spec_d.serial = d.substr(colon + 1);
                        st->cfg.devices.push_back(spec_d);
                    }
                } else if (key == "busy") {
                    for (DeviceSpec& d : st->cfg.devices)
                        if (d.serial == value)
                            d.busy = true;
                } else if (key == "version") {
                    st->cfg.version = static_cast<float>(std::atof(value.c_str()));
                } else if (key == "open_error") {
                    st->cfg.open_error = std::atoi(value.c_str());
                } else if (key == "lock_ms") {
                    st->cfg.lock_ms = std::atoi(value.c_str());
                } else if (key == "loud") {
                    st->loud.store(value == "1");
                }
            }
        }
        return st;
    }();
    return *s;
}

void call(const char* name) {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().calls.push_back(name);
}

void sleep_ms(int ms) {
    if (ms > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// The defaults the specification gives in section 2.
void default_params(State& s) {
    s.dev = r::DevParamsT{};
    s.dev.fsFreq.fsHz = 2000000.0;
    s.dev.rspDxParams.antennaSel = r::rspdx_antenna::a;
    for (r::RxChannelParamsT* ch : {&s.a, &s.b}) {
        *ch = r::RxChannelParamsT{};
        ch->tunerParams.bwType = 200;
        ch->tunerParams.ifType = r::if_khz::zero;
        ch->tunerParams.loMode = 1;
        ch->tunerParams.gain.gRdB = 50;
        ch->tunerParams.gain.minGr = r::min_gr::normal;
        ch->tunerParams.rfFreq.rfHz = 200000000.0;
        ch->tunerParams.dcOffsetTuner.dcCal = 3;
        ch->tunerParams.dcOffsetTuner.trackTime = 1;
        ch->tunerParams.dcOffsetTuner.refreshRateTime = 2048;
        ch->ctrlParams.dcOffset.DCenable = 1;
        ch->ctrlParams.dcOffset.IQenable = 1;
        ch->ctrlParams.decimation.decimationFactor = 1;
        ch->ctrlParams.agc.enable = r::agc::hz50;
        ch->ctrlParams.agc.setPoint_dBfs = -60;
        ch->rsp2TunerParams.antennaSel = r::rsp2_antenna::a;
        ch->rsp2TunerParams.amPortSel = r::am_port::port2;
        ch->rspDxTunerParams.hdrBw = 3;
    }
    s.params.devParams = &s.dev;
    s.params.rxChannelA = &s.a;
    const bool duo = s.selected >= 0 && s.cfg.devices[static_cast<size_t>(s.selected)].hw == r::hw::rspduo;
    s.params.rxChannelB = duo ? &s.b : nullptr;
}

r::RxChannelParamsT& channel(State& s) { return s.selected_tuner == r::tuner::b ? s.b : s.a; }

void streamer_main(State* s, double rate, unsigned decimation) {
    const auto start = std::chrono::steady_clock::now();
    uint64_t sample = 0;
    uint32_t number = 0;
    uint64_t n = 0;
    const unsigned chunk = s->cfg.chunk;
    std::vector<short> xi(chunk), xq(chunk);
    bool stalled = false;
    while (!s->stop.load()) {
        const auto due = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(static_cast<double>(n) * chunk / rate));
        std::this_thread::sleep_until(due);
        if (s->stop.load())
            break;
        ++n;
        if (s->cfg.stall_after != 0 && n > s->cfg.stall_after)
            stalled = true;
        if (stalled) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }
        const bool loud = s->loud.load();
        for (unsigned k = 0; k < chunk; ++k) {
            int i = pattern_i(sample + k);
            int q = pattern_q(sample + k);
            if (loud) {
                i = std::max(-32768, std::min(32767, i * 4));
                q = std::max(-32768, std::min(32767, q * 4));
            }
            xi[k] = static_cast<short>(i);
            xq[k] = static_cast<short>(q);
        }
        unsigned reset = 0;
        if (s->cfg.gap_after != 0 && n == s->cfg.gap_after)
            number += s->cfg.gap_samples * (s->cfg.counts_before_decimation ? decimation : 1);
        if (s->cfg.reset_after != 0 && n == s->cfg.reset_after) {
            reset = 1;
            number = 12345;
        }
        r::StreamCbParamsT p{};
        p.firstSampleNum = number;
        p.numSamples = chunk;
        s->cb.StreamACbFn(xi.data(), xq.data(), &p, chunk, reset, s->ctx);
        s->callbacks.fetch_add(1);
        sample += chunk;
        number += chunk * (s->cfg.counts_before_decimation ? decimation : 1);

        r::EventParamsT e{};
        const r::TunerSelect t = s->selected_tuner;
        if (s->cfg.overload_after != 0 && n == s->cfg.overload_after) {
            e.powerOverloadParams.powerOverloadChangeType = r::overload::detected;
            s->cb.EventCbFn(r::event::power_overload_change, t, &e, s->ctx);
        }
        if (s->cfg.corrected_after != 0 && n == s->cfg.corrected_after) {
            e.powerOverloadParams.powerOverloadChangeType = r::overload::corrected;
            s->cb.EventCbFn(r::event::power_overload_change, t, &e, s->ctx);
        }
        if (s->cfg.remove_after != 0 && n == s->cfg.remove_after)
            s->cb.EventCbFn(r::event::device_removed, t, &e, s->ctx);
        if (s->cfg.fail_after != 0 && n == s->cfg.fail_after)
            s->cb.EventCbFn(r::event::device_failure, t, &e, s->ctx);
    }
}

void stop_streamer(State& s) {
    s.stop.store(true);
    if (s.streamer.joinable())
        s.streamer.join();
}

}  // namespace

extern "C" {

__attribute__((visibility("default"))) State* fake_sdrplay_state() { return &state(); }

__attribute__((visibility("default"))) void fake_sdrplay_reset() {
    State& s = state();
    stop_streamer(s);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.cfg = Config{};
    s.calls.clear();
    s.opened = s.locked = s.listed_while_locked = s.selected_while_locked = s.initialised = false;
    s.selected = -1;
    s.selected_tuner = 0;
    s.selected_mode = 0;
    s.updates.clear();
    s.acks = 0;
    s.failed_updates = 0;
    s.output_rate = 0;
    s.loud.store(false);
    s.callbacks.store(0);
    s.stop.store(false);
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_Open() {
    call("Open");
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.open_error)
        return s.cfg.open_error;
    s.opened = true;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_Close() {
    call("Close");
    std::lock_guard<std::mutex> lock(state().mutex);
    state().opened = false;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_ApiVersion(float* version) {
    call("ApiVersion");
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.version_error)
        return s.cfg.version_error;
    *version = s.cfg.version;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_LockDeviceApi() {
    call("LockDeviceApi");
    State& s = state();
    sleep_ms(s.cfg.lock_ms);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.lock_error)
        return s.cfg.lock_error;
    s.locked = true;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_UnlockDeviceApi() {
    call("UnlockDeviceApi");
    std::lock_guard<std::mutex> lock(state().mutex);
    state().locked = false;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_GetDevices(r::DeviceT* devices, unsigned int* count,
                                                                       unsigned int max) {
    call("GetDevices");
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.get_devices_error)
        return s.cfg.get_devices_error;
    s.listed_while_locked = s.locked;
    unsigned n = 0;
    for (const DeviceSpec& d : s.cfg.devices) {
        if (d.busy || n >= max)
            continue;
        r::DeviceT& out = devices[n++];
        std::memset(&out, 0, sizeof out);
        std::strncpy(out.SerNo, d.serial.c_str(), sizeof out.SerNo - 1);
        out.hwVer = d.hw;
        out.tuner = d.hw == r::hw::rspduo ? r::tuner::both : r::tuner::a;
        out.rspDuoMode = d.hw == r::hw::rspduo ? d.duo_modes : 0;
        out.valid = d.valid;
    }
    *count = n;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_SelectDevice(r::DeviceT* device) {
    call("SelectDevice");
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.select_error)
        return s.cfg.select_error;
    s.selected_while_locked = s.locked;
    for (size_t i = 0; i < s.cfg.devices.size(); ++i) {
        if (s.cfg.devices[i].serial == device->SerNo && !s.cfg.devices[i].busy) {
            s.selected = static_cast<int>(i);
            s.selected_tuner = device->tuner;
            s.selected_mode = device->rspDuoMode;
            device->dev = &s;
            default_params(s);
            return r::err::success;
        }
    }
    return r::err::fail;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_ReleaseDevice(r::DeviceT*) {
    call("ReleaseDevice");
    std::lock_guard<std::mutex> lock(state().mutex);
    state().selected = -1;
    return r::err::success;
}

__attribute__((visibility("default"))) const char* sdrplay_api_GetErrorString(r::ErrT err) {
    switch (err) {
    case r::err::success: return "sdrplay_api_Success";
    case r::err::fail: return "sdrplay_api_Fail";
    case r::err::out_of_range: return "sdrplay_api_OutOfRange";
    case r::err::hw_error: return "sdrplay_api_HwError";
    case r::err::service_not_responding: return "sdrplay_api_ServiceNotResponding";
    case r::err::invalid_service_version: return "sdrplay_api_InvalidServiceVersion";
    default: return "sdrplay_api_Error";
    }
}

__attribute__((visibility("default"))) r::ErrorInfoT* sdrplay_api_GetLastError(r::DeviceT*) {
    static r::ErrorInfoT info;
    std::strcpy(info.message, "the fake API's last error");
    return &info;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_GetDeviceParams(r::Handle dev, r::DeviceParamsT** out) {
    call("GetDeviceParams");
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.params_error)
        return s.cfg.params_error;
    if (dev != &s || s.selected < 0)
        return r::err::not_initialised;
    *out = &s.params;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_Init(r::Handle dev, r::CallbackFnsT* cb, void* ctx) {
    call("Init");
    State& s = state();
    sleep_ms(s.cfg.init_ms);
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.cfg.init_error)
        return s.cfg.init_error;
    if (dev != &s || s.selected < 0)
        return r::err::not_initialised;
    if (s.initialised)
        return r::err::already_initialised;
    s.init_dev = s.dev;
    s.init_channel = channel(s);
    const r::RxChannelParamsT& ch = channel(s);
    double base = s.dev.fsFreq.fsHz;
    // The low-IF cases the module uses: the API mixes down to 2 MHz.
    if ((base == 6000000.0 && ch.tunerParams.ifType == r::if_khz::if_1_620) ||
        (base == 8000000.0 && ch.tunerParams.ifType == r::if_khz::if_2_048))
        base = 2000000.0;
    const unsigned decimation = ch.ctrlParams.decimation.enable ? ch.ctrlParams.decimation.decimationFactor : 1;
    s.output_rate = base / decimation;
    s.cb = *cb;
    s.ctx = ctx;
    s.stop.store(false);
    s.initialised = true;
    s.streamer = std::thread(streamer_main, &s, s.output_rate * s.cfg.rate_factor, decimation);
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_Uninit(r::Handle) {
    call("Uninit");
    State& s = state();
    sleep_ms(s.cfg.uninit_ms);
    stop_streamer(s);
    std::lock_guard<std::mutex> lock(s.mutex);
    s.initialised = false;
    return r::err::success;
}

__attribute__((visibility("default"))) r::ErrT sdrplay_api_Update(r::Handle, r::TunerSelect,
                                                                   r::ReasonForUpdate reason,
                                                                   r::ReasonForUpdateExt1 ext1) {
    call("Update");
    State& s = state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if (reason & r::update::ctrl_overload_msg_ack)
        ++s.acks;
    if (s.cfg.update_error && (reason & s.cfg.update_error_on) &&
        (s.cfg.update_error_count == 0 || s.failed_updates < s.cfg.update_error_count)) {
        ++s.failed_updates;
        return s.cfg.update_error;
    }
    const r::RxChannelParamsT& ch = channel(s);
    s.updates.push_back(UpdateRecord{reason, ext1, ch.tunerParams.gain.LNAstate, ch.tunerParams.gain.gRdB,
                                     ch.ctrlParams.agc.enable});
    return r::err::success;
}

}  // extern "C"
