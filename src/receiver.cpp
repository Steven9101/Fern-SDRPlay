// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "receiver.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "log.h"
#include "presence.h"

namespace fern {

namespace {

using Clock = std::chrono::steady_clock;

// How long a live change may wait for the API.
constexpr std::chrono::milliseconds update_timeout{3000};

std::string serial_of(const rsp::DeviceT& d) { return std::string(d.SerNo, strnlen(d.SerNo, sizeof d.SerNo)); }

std::string mhz_text(double hz) {
    char text[32];
    std::snprintf(text, sizeof text, "%.6g MHz", hz / 1e6);
    return text;
}

Failure invalid(std::string message) { return Failure{ErrorCode::invalid, std::move(message)}; }

std::string listed(const rsp::DeviceT* devices, unsigned count) {
    std::string s;
    for (unsigned i = 0; i < count; ++i) {
        const auto model = model_from_hw(devices[i].hwVer);
        s += (i == 0 ? "" : ", ") + std::string(model ? model_name(*model) : "unknown RSP") + " " +
             serial_of(devices[i]);
    }
    return s;
}

// Holds the API lock for its lifetime, so that no path leaves it held.
class ApiLock {
public:
    ApiLock(Api& api, Watchdog& watchdog, Clock::time_point deadline)
        : api_(api), watchdog_(watchdog), deadline_(deadline) {}
    ~ApiLock() { release(); }
    ApiLock(const ApiLock&) = delete;
    ApiLock& operator=(const ApiLock&) = delete;

    void mark_held() { held_ = true; }
    void release() {
        if (!held_)
            return;
        held_ = false;
        // An unlock gets a second of its own even when the open's deadline
        // has passed: leaving the lock held would stall every other band.
        Watchdog::Scope s(watchdog_, "sdrplay_api_UnlockDeviceApi",
                          std::max(deadline_, Clock::now() + std::chrono::seconds(1)));
        const rsp::ErrT err = api_.UnlockDeviceApi();
        if (err != rsp::err::success)
            log_line("sdrplay_api_UnlockDeviceApi failed: %s", api_.error_text(err).c_str());
    }

private:
    Api& api_;
    Watchdog& watchdog_;
    Clock::time_point deadline_;
    bool held_ = false;
};

}  // namespace

Receiver::Receiver(Api& api, Watchdog& watchdog, std::string usb_devices_dir)
    : api_(api), watchdog_(watchdog), usb_devices_dir_(std::move(usb_devices_dir)) {}

Receiver::~Receiver() {
    if (api_open_)
        close_by(Clock::now() + std::chrono::seconds(1));
}

std::optional<Failure> Receiver::hung(const char* call) const {
    return Failure{ErrorCode::busy,
                   std::string(call) +
                       " did not return in time. Another program may hold the SDRplay API's lock (another FernSDR "
                       "band that is starting or stuck, SDRuno, SDRconnect), or the sdrplay service hangs: sudo "
                       "systemctl restart sdrplay"};
}

std::optional<Failure> Receiver::api_failure(const char* call, rsp::ErrT err, rsp::DeviceT* device) const {
    std::string detail = api_.error_text(err);
    const std::string last = api_.last_error(device);
    if (!last.empty())
        detail += ": " + last;
    if (err == rsp::err::service_not_responding)
        return Failure{ErrorCode::internal, std::string(call) + " failed: the SDRplay API service stopped answering (" +
                                                detail + "). Restart it: sudo systemctl restart sdrplay"};
    return Failure{ErrorCode::usb, std::string(call) + " failed: " + detail +
                                       ". If this repeats, unplug the RSP and plug it in again, or restart the "
                                       "sdrplay service"};
}

std::optional<Failure> Receiver::choose(rsp::DeviceT* devices, unsigned count, const DeviceSelector& selector,
                                        rsp::DeviceT& chosen) {
    const int on_usb = count_rsps_on_usb(usb_devices_dir_);
    const unsigned hidden = on_usb > static_cast<int>(count) ? static_cast<unsigned>(on_usb) - count : 0;
    const std::string seen = count == 0 ? std::string("none") : listed(devices, count);
    const std::string held = hidden == 0 ? std::string()
                                         : std::to_string(hidden) + " more RSP" + (hidden == 1 ? " is" : "s are") +
                                               " plugged in but in use by another program, which the API does not "
                                               "list";
    const rsp::DeviceT* pick = nullptr;
    switch (selector.kind) {
    case DeviceSelector::Kind::only:
        if (count == 0 && hidden > 0)
            return Failure{ErrorCode::busy, "every RSP plugged in is in use by another program (" + held +
                                                "). Stop the other program, or check that no other FernSDR band uses "
                                                "the same RSP"};
        if (count == 0)
            return Failure{ErrorCode::no_device,
                           "no RSP is plugged in, or the sdrplay service does not see it. lsusb lists an RSP as "
                           "1df7:xxxx; after plugging one in, sudo systemctl restart sdrplay may help"};
        if (count > 1 || hidden > 0)
            return Failure{ErrorCode::no_device,
                           std::to_string(count + hidden) +
                               " RSPs are plugged in; give each band module.device = serial:<serial>. The API "
                               "offers: " +
                               seen + (hidden > 0 ? "; " + held : std::string()) +
                               ". fern-sdrplay --list-devices lists them"};
        pick = &devices[0];
        break;
    case DeviceSelector::Kind::index:
        if (selector.index >= count)
            return Failure{hidden > 0 ? ErrorCode::busy : ErrorCode::no_device,
                           "module.device asks for index " + std::to_string(selector.index) + ", but the API offers " +
                               std::to_string(count) + " RSP" + (count == 1 ? "" : "s") + " (" + seen + ")" +
                               (hidden > 0 ? "; " + held : std::string()) +
                               ". Indexes change when another program holds an RSP; use serial:"};
        pick = &devices[selector.index];
        break;
    case DeviceSelector::Kind::serial:
        for (unsigned i = 0; i < count; ++i)
            if (serial_of(devices[i]) == selector.serial)
                pick = &devices[i];
        if (!pick && hidden > 0)
            return Failure{ErrorCode::busy, "the RSP with serial " + selector.serial +
                                                " is probably in use by another program: " + held +
                                                ". The API offers " + seen +
                                                ". Stop the other program, or check that no other FernSDR band "
                                                "names the same serial"};
        if (!pick)
            return Failure{ErrorCode::no_device, "no RSP with serial " + selector.serial +
                                                     " is plugged in; the API offers " + seen};
        break;
    }
    const auto model = model_from_hw(pick->hwVer);
    if (!model)
        return Failure{ErrorCode::no_device, "the RSP " + serial_of(*pick) + " reports hardware version " +
                                                 std::to_string(pick->hwVer) +
                                                 ", which this module does not know; a newer release may"};
    if (!pick->valid)
        return Failure{ErrorCode::usb, "the SDRplay API reports the " + std::string(model_name(*model)) + " " +
                                           serial_of(*pick) +
                                           " as not ready to use. Unplug it and plug it in again, or restart the "
                                           "sdrplay service"};
    if (*model == Model::rspduo && !(pick->rspDuoMode & (rsp::duo_mode::master | rsp::duo_mode::single_tuner)))
        return Failure{ErrorCode::busy, "the RSPduo " + serial_of(*pick) +
                                            " is in use by another program, which leaves only its slave mode; this "
                                            "module uses an RSPduo as one tuner on its own"};
    chosen = *pick;
    return std::nullopt;
}

std::optional<Failure> Receiver::check_switches(const Effective& e) const {
    const std::string name = model_name(identity_.model);
    const std::string input = antenna_name(e.antenna);
    if (e.bias_tee && !has_bias_tee(identity_.model, e.antenna)) {
        std::string where = "has no bias tee";
        switch (identity_.model) {
        case Model::rsp2: where = "has its bias tee on inputs a and b"; break;
        case Model::rspduo: where = "has its bias tee on tuner2"; break;
        case Model::rspdx:
        case Model::rspdx_r2: where = "has its bias tee on input b"; break;
        default: break;
        }
        return invalid("module.bias_tee: the " + name + " " + where + ", not on " + input);
    }
    if (e.rf_notch && !has_rf_notch(identity_.model))
        return invalid("module.rf_notch: the " + name + " has no broadcast notch filter");
    if (e.dab_notch && !has_dab_notch(identity_.model))
        return invalid("module.dab_notch: the " + name + " has no DAB notch filter");
    if (e.am_notch && !has_am_notch(identity_.model, e.antenna))
        return invalid("module.am_notch is the RSPduo's tuner 1 MW notch; the " + name + " on " + input +
                       " has none");
    return std::nullopt;
}

std::optional<Failure> Receiver::plan(const OpenRequest& request, Model model, Effective& out,
                                      std::vector<int>& table) const {
    const ModuleSettings& s = request.settings;
    Effective e;
    e.center = request.center;
    if (auto f = resolve_antenna(model, s.antenna, request.center, e.antenna))
        return f;
    if (auto f = plan_rate(request.sample_rate, s.if_mode, s.bandwidth_khz, e.plan))
        return f;
    if (s.hdr && !has_hdr(model))
        return invalid("module.hdr is a mode of the RSPdx and RSPdx-R2; this is an " + std::string(model_name(model)));
    table = lna_reductions(model, e.antenna, request.center, s.hdr);
    const std::vector<GainStep> ladder = gain_ladder(table);
    const GainStep& start = ladder[ladder_start(ladder)];
    e.gain = s.gain;
    e.lna_state = start.lna_state;
    e.if_reduction = start.if_reduction;
    if (s.lna_state) {
        if (*s.lna_state >= table.size())
            return invalid("module.lna_state " + std::to_string(*s.lna_state) + ": the " + model_name(model) + " has " +
                           std::to_string(table.size()) + " LNA states at " + mhz_text(request.center) +
                           " on input " + antenna_name(e.antenna) + ", 0 to " + std::to_string(table.size() - 1));
        e.lna_state = *s.lna_state;
    }
    if (s.if_gain_reduction)
        e.if_reduction = *s.if_gain_reduction;
    e.bias_tee = s.bias_tee;
    e.rf_notch = s.rf_notch;
    e.dab_notch = s.dab_notch;
    e.am_notch = s.am_notch;
    e.hdr = s.hdr;
    e.ppm = s.ppm;
    e.dc_correction = s.dc_correction;
    e.iq_correction = s.iq_correction;
    e.if_mode = s.if_mode;
    out = e;
    return std::nullopt;
}

rsp::RxChannelParamsT* Receiver::channel() const {
    if (!params_)
        return nullptr;
    return device_.tuner == rsp::tuner::b ? params_->rxChannelB : params_->rxChannelA;
}

void Receiver::write_fields(const Effective& e) {
    rsp::DevParamsT* d = params_->devParams;
    rsp::RxChannelParamsT* ch = channel();
    d->fsFreq.fsHz = e.plan.fs_hz;
    d->ppm = e.ppm;
    rsp::TunerParamsT& t = ch->tunerParams;
    t.rfFreq.rfHz = e.center;
    t.bwType = e.plan.bandwidth_khz;
    t.ifType = e.plan.if_khz;
    t.gain.gRdB = e.if_reduction;
    t.gain.LNAstate = static_cast<unsigned char>(e.lna_state);
    t.gain.minGr = rsp::min_gr::normal;
    rsp::ControlParamsT& c = ch->ctrlParams;
    c.decimation.enable = e.plan.decimation > 1;
    c.decimation.decimationFactor = static_cast<unsigned char>(e.plan.decimation);
    c.decimation.wideBandSignal = e.plan.wide_band;
    // The API's AGC is on by default; it stays off unless the operator
    // chose it, since it would fight the module's own control.
    c.agc.enable = e.gain == GainMode::agc ? rsp::agc::ctrl_en : rsp::agc::disable;
    c.agc.setPoint_dBfs = agc_set_point_dbfs;
    c.dcOffset.DCenable = e.dc_correction;
    c.dcOffset.IQenable = e.iq_correction;
    switch (identity_.model) {
    case Model::rsp1:
        break;
    case Model::rsp1a:
    case Model::rsp1b:
        d->rsp1aParams.rfNotchEnable = e.rf_notch;
        d->rsp1aParams.rfDabNotchEnable = e.dab_notch;
        ch->rsp1aTunerParams.biasTEnable = e.bias_tee;
        break;
    case Model::rsp2:
        ch->rsp2TunerParams.biasTEnable = e.bias_tee;
        ch->rsp2TunerParams.rfNotchEnable = e.rf_notch;
        ch->rsp2TunerParams.antennaSel = e.antenna == Antenna::b ? rsp::rsp2_antenna::b : rsp::rsp2_antenna::a;
        ch->rsp2TunerParams.amPortSel = e.antenna == Antenna::hiz ? rsp::am_port::port1 : rsp::am_port::port2;
        break;
    case Model::rspduo:
        ch->rspDuoTunerParams.biasTEnable = e.bias_tee;
        ch->rspDuoTunerParams.rfNotchEnable = e.rf_notch;
        ch->rspDuoTunerParams.rfDabNotchEnable = e.dab_notch;
        ch->rspDuoTunerParams.tuner1AmNotchEnable = e.am_notch;
        ch->rspDuoTunerParams.tuner1AmPortSel = e.antenna == Antenna::hiz ? rsp::am_port::port1 : rsp::am_port::port2;
        break;
    case Model::rspdx:
    case Model::rspdx_r2:
        d->rspDxParams.antennaSel = e.antenna == Antenna::c   ? rsp::rspdx_antenna::c
                                    : e.antenna == Antenna::b ? rsp::rspdx_antenna::b
                                                              : rsp::rspdx_antenna::a;
        d->rspDxParams.hdrEnable = e.hdr;
        d->rspDxParams.biasTEnable = e.bias_tee;
        d->rspDxParams.rfNotchEnable = e.rf_notch;
        d->rspDxParams.rfDabNotchEnable = e.dab_notch;
        break;
    }
}

void Receiver::reasons_between(const Effective& a, const Effective& b, rsp::ReasonForUpdate& reason,
                               rsp::ReasonForUpdateExt1& ext1) const {
    reason = rsp::update::none;
    ext1 = rsp::update_ext1::none;
    if (a.center != b.center)
        reason |= rsp::update::tuner_frf;
    if (a.lna_state != b.lna_state || a.if_reduction != b.if_reduction)
        reason |= rsp::update::tuner_gr;
    if ((a.gain == GainMode::agc) != (b.gain == GainMode::agc))
        reason |= rsp::update::ctrl_agc;
    if (a.ppm != b.ppm)
        reason |= rsp::update::dev_ppm;
    if (a.dc_correction != b.dc_correction || a.iq_correction != b.iq_correction)
        reason |= rsp::update::ctrl_dc_offset_iq_imbalance;
    const bool bias = a.bias_tee != b.bias_tee;
    const bool rf = a.rf_notch != b.rf_notch;
    const bool dab = a.dab_notch != b.dab_notch;
    switch (identity_.model) {
    case Model::rsp1:
        break;
    case Model::rsp1a:
    case Model::rsp1b:
        reason |= (bias ? rsp::update::rsp1a_bias_t : 0) | (rf ? rsp::update::rsp1a_rf_notch : 0) |
                  (dab ? rsp::update::rsp1a_rf_dab_notch : 0);
        break;
    case Model::rsp2:
        reason |= (bias ? rsp::update::rsp2_bias_t : 0) | (rf ? rsp::update::rsp2_rf_notch : 0);
        break;
    case Model::rspduo:
        reason |= (bias ? rsp::update::rspduo_bias_t : 0) | (rf ? rsp::update::rspduo_rf_notch : 0) |
                  (dab ? rsp::update::rspduo_rf_dab_notch : 0) |
                  (a.am_notch != b.am_notch ? rsp::update::rspduo_tuner1_am_notch : 0);
        break;
    case Model::rspdx:
    case Model::rspdx_r2:
        // The RSPdx's switches take the extension flags (section 3.17).
        ext1 |= (bias ? rsp::update_ext1::rspdx_bias_t : 0) | (rf ? rsp::update_ext1::rspdx_rf_notch : 0) |
                (dab ? rsp::update_ext1::rspdx_rf_dab_notch : 0);
        break;
    }
}

std::optional<Failure> Receiver::update(const char* what, rsp::ReasonForUpdate reason,
                                        rsp::ReasonForUpdateExt1 ext1) {
    if (reason == rsp::update::none && ext1 == rsp::update_ext1::none)
        return std::nullopt;
    Watchdog::Scope s(watchdog_, what, Clock::now() + update_timeout);
    const rsp::ErrT err = api_.Update(device_.dev, device_.tuner, reason, ext1);
    if (s.expired())
        return hung(what);
    if (err != rsp::err::success)
        return api_failure(what, err, &device_);
    return std::nullopt;
}

std::optional<Failure> Receiver::change_to(const Effective& next) {
    const Effective before = effective_;
    rsp::ReasonForUpdate reason;
    rsp::ReasonForUpdateExt1 ext1;
    reasons_between(before, next, reason, ext1);
    write_fields(next);
    if (auto f = update("sdrplay_api_Update", reason, ext1)) {
        write_fields(before);
        return f;
    }
    effective_ = next;
    return std::nullopt;
}

std::optional<Failure> Receiver::open(const OpenRequest& request, Stream& stream, Clock::time_point deadline) {
    if (auto f = api_.load())
        return f;
    // Set while the API lock is held: a failure gives the lock back before
    // it closes the API.
    ApiLock* held = nullptr;
    const auto fail = [&](std::optional<Failure> f) {
        if (held)
            held->release();
        release_and_close(Clock::now() + std::chrono::milliseconds(1500));
        return f;
    };

    {
        Watchdog::Scope s(watchdog_, "sdrplay_api_Open", deadline);
        const rsp::ErrT err = api_.Open();
        if (err == rsp::err::success)
            api_open_ = true;
        if (s.expired())
            return fail(hung("sdrplay_api_Open"));
        if (err != rsp::err::success) {
            std::string detail = api_.error_text(err);
            const std::string last = api_.last_error(nullptr);
            if (!last.empty())
                detail += ": " + last;
            return Failure{ErrorCode::internal,
                           "the SDRplay API service is not running (sdrplay_api_Open: " + detail +
                               "). Start it: sudo systemctl start sdrplay; SDRplay's installer sets it up to start "
                               "at boot. If it runs, this process cannot see its shared memory in /dev/shm: a "
                               "container needs the host's IPC namespace and /dev/shm"};
        }
    }

    {
        float version = 0;
        Watchdog::Scope s(watchdog_, "sdrplay_api_ApiVersion", deadline);
        const rsp::ErrT err = api_.ApiVersion(&version);
        if (s.expired())
            return fail(hung("sdrplay_api_ApiVersion"));
        if (err == rsp::err::invalid_service_version)
            return fail(Failure{ErrorCode::internal,
                                "the SDRplay API library (" + api_.path() +
                                    ") and the sdrplay service are different versions. Install SDRplay API 3.15 "
                                    "from " +
                                    api_download_url + " again, so that both match, and restart the service"});
        if (err != rsp::err::success)
            return fail(api_failure("sdrplay_api_ApiVersion", err, nullptr));
        if (!api_version_supported(version)) {
            char text[32];
            std::snprintf(text, sizeof text, "%.2f", version);
            return fail(Failure{ErrorCode::internal,
                                std::string("SDRplay API ") + text +
                                    " is installed; this module knows the data layout of versions 3.14 and 3.15 "
                                    "only, and a different one could be misread. Install SDRplay API 3.15 from " +
                                    api_download_url + ", or a release of this module that supports " + text});
        }
        identity_.api_version = version;
    }

    Effective planned;
    rsp::DeviceT chosen{};
    {
        ApiLock lock(api_, watchdog_, deadline);
        held = &lock;
        {
            Watchdog::Scope s(watchdog_, "sdrplay_api_LockDeviceApi", deadline);
            const rsp::ErrT err = api_.LockDeviceApi();
            // A late success still holds the lock, and must release it.
            if (err == rsp::err::success)
                lock.mark_held();
            if (s.expired())
                return fail(hung("sdrplay_api_LockDeviceApi"));
            if (err != rsp::err::success)
                return fail(api_failure("sdrplay_api_LockDeviceApi", err, nullptr));
        }

        rsp::DeviceT devices[rsp::max_devices];
        std::memset(devices, 0, sizeof devices);
        unsigned count = 0;
        {
            Watchdog::Scope s(watchdog_, "sdrplay_api_GetDevices", deadline);
            const rsp::ErrT err = api_.GetDevices(devices, &count, rsp::max_devices);
            if (s.expired())
                return fail(hung("sdrplay_api_GetDevices"));
            if (err != rsp::err::success)
                return fail(api_failure("sdrplay_api_GetDevices", err, nullptr));
        }
        if (count > rsp::max_devices)
            count = rsp::max_devices;
        if (auto f = choose(devices, count, request.settings.device, chosen))
            return fail(f);
        identity_.model = *model_from_hw(chosen.hwVer);
        identity_.hw_version = chosen.hwVer;
        identity_.serial = serial_of(chosen);
        // Everything that depends on the model is checked before the RSP
        // is taken, so that a wrong setting never holds it.
        if (auto f = plan(request, identity_.model, planned, lna_table_))
            return fail(f);
        if (auto f = check_switches(planned))
            return fail(f);
        if (identity_.model == Model::rspduo) {
            chosen.tuner = planned.antenna == Antenna::tuner2 ? rsp::tuner::b : rsp::tuner::a;
            chosen.rspDuoMode = rsp::duo_mode::single_tuner;
        }
        device_ = chosen;
        {
            Watchdog::Scope s(watchdog_, "sdrplay_api_SelectDevice", deadline);
            const rsp::ErrT err = api_.SelectDevice(&device_);
            if (err == rsp::err::success)
                selected_ = true;
            if (s.expired())
                return fail(hung("sdrplay_api_SelectDevice"));
            if (err != rsp::err::success)
                return fail(api_failure("sdrplay_api_SelectDevice", err, &device_));
        }
        lock.release();
        held = nullptr;
    }

    {
        Watchdog::Scope s(watchdog_, "sdrplay_api_GetDeviceParams", deadline);
        const rsp::ErrT err = api_.GetDeviceParams(device_.dev, &params_);
        if (s.expired())
            return fail(hung("sdrplay_api_GetDeviceParams"));
        if (err != rsp::err::success)
            return fail(api_failure("sdrplay_api_GetDeviceParams", err, &device_));
    }
    if (!params_ || !params_->devParams || !channel())
        return fail(Failure{ErrorCode::internal,
                            "sdrplay_api_GetDeviceParams gave no device or tuner parameters for the " +
                                std::string(model_name(identity_.model)) + " " + identity_.serial});

    ladder_ = gain_ladder(lna_table_);
    ladder_step_ = ladder_start(ladder_);
    effective_ = planned;
    write_fields(effective_);

    callbacks_.StreamACbFn = &Stream::on_stream;
    callbacks_.StreamBCbFn = &Stream::on_stream;
    callbacks_.EventCbFn = &Stream::on_event;
    {
        Watchdog::Scope s(watchdog_, "sdrplay_api_Init", deadline);
        const rsp::ErrT err = api_.Init(device_.dev, &callbacks_, &stream);
        if (s.expired()) {
            initialised_ = true;
            return fail(hung("sdrplay_api_Init"));
        }
        if (err != rsp::err::success) {
            if (err == rsp::err::out_of_range)
                return fail(Failure{ErrorCode::invalid,
                                    "the SDRplay API refused the settings for the " +
                                        std::string(model_name(identity_.model)) + " (" + api_.error_text(err) +
                                        ": " + api_.last_error(&device_) + ")"});
            return fail(api_failure("sdrplay_api_Init", err, &device_));
        }
    }
    initialised_ = true;
    log_line("%s %s, API %.2f, %.0f Hz from %.0f Hz / %u, IF %d kHz, filter %d kHz, centre %.0f Hz, input %s, "
             "gain %s, LNA state %u, IF gain reduction %d dB",
             model_name(identity_.model), identity_.serial.c_str(), static_cast<double>(identity_.api_version),
             effective_.plan.output_hz, effective_.plan.fs_hz, effective_.plan.decimation, effective_.plan.if_khz,
             effective_.plan.bandwidth_khz, effective_.center, antenna_name(effective_.antenna),
             gain_mode_name(effective_.gain), effective_.lna_state, effective_.if_reduction);
    return std::nullopt;
}

std::optional<Failure> Receiver::apply(const LiveChange& change) {
    if (!initialised_)
        return Failure{ErrorCode::internal, "the RSP is not open"};
    Effective next = effective_;
    if (change.gain)
        next.gain = *change.gain;
    if (next.gain == GainMode::automatic && (change.lna_state || change.if_gain_reduction))
        return invalid(std::string("module.") + (change.lna_state ? "lna_state" : "if_gain_reduction") +
                       " sets the gain by hand, but module.gain is auto; send module.gain = manual with it");
    if (next.gain == GainMode::agc && change.if_gain_reduction)
        return invalid("module.if_gain_reduction is the API AGC's to set while module.gain = agc");
    std::optional<size_t> step;
    if (change.gain && *change.gain == GainMode::automatic && effective_.gain != GainMode::automatic) {
        step = ladder_nearest(ladder_, static_cast<int>(lna_table_[effective_.lna_state]) + effective_.if_reduction);
        next.lna_state = ladder_[*step].lna_state;
        next.if_reduction = ladder_[*step].if_reduction;
    }
    if (change.lna_state) {
        if (*change.lna_state >= lna_table_.size())
            return invalid("module.lna_state " + std::to_string(*change.lna_state) + ": this " +
                           model_name(identity_.model) + " has " + std::to_string(lna_table_.size()) +
                           " LNA states here, 0 to " + std::to_string(lna_table_.size() - 1));
        next.lna_state = *change.lna_state;
    }
    if (change.if_gain_reduction)
        next.if_reduction = *change.if_gain_reduction;
    if (change.bias_tee)
        next.bias_tee = *change.bias_tee;
    if (change.rf_notch)
        next.rf_notch = *change.rf_notch;
    if (change.dab_notch)
        next.dab_notch = *change.dab_notch;
    if (change.am_notch)
        next.am_notch = *change.am_notch;
    if (change.ppm)
        next.ppm = *change.ppm;
    if (change.dc_correction)
        next.dc_correction = *change.dc_correction;
    if (change.iq_correction)
        next.iq_correction = *change.iq_correction;
    if (auto f = check_switches(next))
        return f;
    if (auto f = change_to(next))
        return f;
    if (step)
        ladder_step_ = *step;
    return std::nullopt;
}

std::optional<Failure> Receiver::retune(double center) {
    if (!initialised_)
        return Failure{ErrorCode::internal, "the RSP is not open"};
    if (effective_.hdr)
        return invalid("HDR mode works at fixed centres only; restart the band to move it");
    Effective next = effective_;
    if (auto f = resolve_antenna(identity_.model, effective_.antenna, center, next.antenna))
        return f;
    next.center = center;
    std::vector<int> table = lna_reductions(identity_.model, next.antenna, center, false);
    std::vector<GainStep> ladder = gain_ladder(table);
    size_t step = ladder_step_;
    if (next.gain == GainMode::automatic) {
        step = ladder_nearest(ladder, lna_table_[effective_.lna_state] + effective_.if_reduction);
        next.lna_state = ladder[step].lna_state;
        next.if_reduction = ladder[step].if_reduction;
    } else if (next.lna_state >= table.size()) {
        return invalid("LNA state " + std::to_string(next.lna_state) + " does not exist at " + mhz_text(center) +
                       "; the " + model_name(identity_.model) + " has " + std::to_string(table.size()) +
                       " there");
    }
    if (auto f = change_to(next))
        return f;
    lna_table_ = std::move(table);
    ladder_ = std::move(ladder);
    ladder_step_ = step;
    return std::nullopt;
}

std::optional<Failure> Receiver::set_ladder_step(size_t step) {
    if (step >= ladder_.size())
        return Failure{ErrorCode::internal, "gain step out of range"};
    Effective next = effective_;
    next.lna_state = ladder_[step].lna_state;
    next.if_reduction = ladder_[step].if_reduction;
    if (auto f = change_to(next))
        return f;
    ladder_step_ = step;
    return std::nullopt;
}

std::optional<Failure> Receiver::acknowledge_overload() {
    if (!initialised_)
        return std::nullopt;
    return update("sdrplay_api_Update(OverloadMsgAck)", rsp::update::ctrl_overload_msg_ack, rsp::update_ext1::none);
}

json::Value Receiver::device_json() const {
    json::Value d = json::Value::object();
    d.set("name", model_name(identity_.model));
    d.set("serial", identity_.serial);
    d.set("hw_version", identity_.hw_version);
    char version[16];
    std::snprintf(version, sizeof version, "%.2f", static_cast<double>(identity_.api_version));
    d.set("api_version", version);
    d.set("library", api_.path());
    return d;
}

namespace {

void set_gain_fields(json::Value& s, const Effective& e, const std::vector<int>& table) {
    s.set("gain", gain_mode_name(e.gain));
    s.set("lna_state", e.lna_state);
    s.set("if_gain_reduction", e.if_reduction);
    if (e.lna_state < table.size())
        s.set("gain_reduction", table[e.lna_state] + e.if_reduction);
}

}  // namespace

json::Value Receiver::settings_json() const {
    const Effective& e = effective_;
    json::Value s = json::Value::object();
    s.set("antenna", antenna_name(e.antenna));
    set_gain_fields(s, e, lna_table_);
    s.set("lna_states", lna_table_.size());
    s.set("bias_tee", e.bias_tee);
    s.set("rf_notch", e.rf_notch);
    s.set("dab_notch", e.dab_notch);
    s.set("am_notch", e.am_notch);
    s.set("hdr", e.hdr);
    s.set("ppm", e.ppm);
    s.set("dc_correction", e.dc_correction);
    s.set("iq_correction", e.iq_correction);
    s.set("if_mode", if_mode_name(e.if_mode));
    s.set("bandwidth", std::to_string(e.plan.bandwidth_khz));
    s.set("converter_rate", e.plan.fs_hz);
    s.set("decimation", e.plan.decimation);
    return s;
}

json::Value Receiver::settings_json(const LiveChange& change) const {
    const Effective& e = effective_;
    json::Value s = json::Value::object();
    if (change.gain || change.lna_state || change.if_gain_reduction)
        set_gain_fields(s, e, lna_table_);
    if (change.bias_tee)
        s.set("bias_tee", e.bias_tee);
    if (change.rf_notch)
        s.set("rf_notch", e.rf_notch);
    if (change.dab_notch)
        s.set("dab_notch", e.dab_notch);
    if (change.am_notch)
        s.set("am_notch", e.am_notch);
    if (change.ppm)
        s.set("ppm", e.ppm);
    if (change.dc_correction)
        s.set("dc_correction", e.dc_correction);
    if (change.iq_correction)
        s.set("iq_correction", e.iq_correction);
    return s;
}

void Receiver::release_and_close(Clock::time_point deadline) {
    if (!close_by(deadline))
        log_line("closing the SDRplay API did not finish in time");
}

bool Receiver::close_by(Clock::time_point deadline) {
    bool ok = true;
    const auto run = [&](const char* name, auto&& call) {
        if (Clock::now() >= deadline) {
            log_line("no time left for %s", name);
            ok = false;
            return;
        }
        Watchdog::Scope s(watchdog_, name, deadline);
        const rsp::ErrT err = call();
        if (s.expired())
            ok = false;
        else if (err != rsp::err::success)
            log_line("%s failed: %s", name, api_.error_text(err).c_str());
    };
    if (initialised_) {
        run("sdrplay_api_Uninit", [&] { return api_.Uninit(device_.dev); });
        initialised_ = false;
    }
    if (selected_) {
        run("sdrplay_api_ReleaseDevice", [&] { return api_.ReleaseDevice(&device_); });
        selected_ = false;
    }
    if (api_open_) {
        run("sdrplay_api_Close", [&] { return api_.Close(); });
        api_open_ = false;
    }
    params_ = nullptr;
    return ok;
}

}  // namespace fern
