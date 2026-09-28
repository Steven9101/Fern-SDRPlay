// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "session.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <optional>
#include <poll.h>
#include <string>
#include <sys/eventfd.h>
#include <unistd.h>

#include "failure.h"
#include "io.h"
#include "json.h"
#include "log.h"
#include "models.h"
#include "receiver.h"
#include "settings.h"
#include "stream.h"
#include "watchdog.h"

namespace fern {

namespace {

using Clock = std::chrono::steady_clock;

constexpr size_t max_line = 64 * 1024;

bool blank(const std::string& s) {
    for (char c : s)
        if (c != ' ' && c != '\t' && c != '\r')
            return false;
    return true;
}

// How long one event may wait for FernSDR to read it.
constexpr int event_timeout_ms = 2000;

class Session {
public:
    Session(Api& api, const SessionIo& io, const SessionOptions& options)
        : io_(io),
          options_(options),
          watchdog_([this](const std::string& what) { on_hang(what); }),
          receiver_(api, watchdog_, options.usb_devices_dir),
          lines_(max_line) {}

    ~Session() {
        if (notify_fd_ >= 0 && !leaked_)
            ::close(notify_fd_);
    }

    SessionResult run();

private:
    enum class State { waiting, streaming };

    SessionIo io_;
    SessionOptions options_;
    Watchdog watchdog_;
    // Declared before the receiver, so that it outlives the receiver's
    // closing of the API: the API calls into it until Uninit returns.
    std::unique_ptr<Stream> stream_;
    Receiver receiver_;
    std::optional<GainControl> gain_control_;
    LineReader lines_;
    std::mutex events_mutex_;
    int notify_fd_ = -1;
    State state_ = State::waiting;
    std::optional<int> exit_;
    std::atomic<bool> stopping_{false};
    bool commands_open_ = true;
    bool events_broken_ = false;
    bool leaked_ = false;
    Clock::time_point next_stats_;
    Clock::time_point next_drop_log_;
    Clock::time_point next_lowest_log_;
    Clock::time_point next_overload_log_;
    uint64_t stats_received_ = 0;
    uint64_t stats_clipped_ = 0;
    uint64_t logged_drops_ = 0;
    uint64_t logged_overloads_ = 0;
    uint64_t logged_discontinuities_ = 0;
    // The rate check: when the first samples came, and the start of the
    // measured window.
    std::optional<Clock::time_point> first_data_;
    std::optional<Clock::time_point> rate_from_;
    uint64_t rate_from_samples_ = 0;
    bool rate_checked_ = false;

    void finish(int status) {
        if (!exit_)
            exit_ = status;
    }
    bool send(const json::Value& event);
    void fail(const Failure& f);
    void on_hang(const std::string& what);
    void read_commands();
    void handle_line(const LineReader::Line& line);
    void handle_open(const json::Value& message);
    void handle_set(const json::Value& message);
    void check_stream();
    void start_gain_control();
    void check_rate(Clock::time_point now);
    void on_tick();
    SessionResult shut_down();
};

bool Session::send(const json::Value& event) {
    std::lock_guard<std::mutex> lock(events_mutex_);
    if (events_broken_)
        return false;
    std::string line = json::serialize(event);
    if (line.size() >= max_line) {
        // Nothing the module writes comes near this; keep the line limit even
        // if something does.
        log_line("an event of %zu bytes is longer than FernSDR accepts; sent an error instead", line.size());
        json::Value e = json::Value::object();
        e.set("type", "error");
        e.set("code", "internal");
        e.set("message", "an event was too long to send");
        const json::Value* fatal = event.find("fatal");
        e.set("fatal", fatal && fatal->is_bool() && fatal->as_bool());
        line = json::serialize(e);
    }
    line += '\n';
    // Bounded, and cut short by a stop signal: FernSDR alive but not reading
    // fd 3 must not hold the module out of reach of SIGTERM.
    const int err = write_all(io_.events, line.data(), line.size(), event_timeout_ms, io_.stop);
    if (err == 0)
        return true;
    events_broken_ = true;
    if (err == EPIPE) {
        log_line("FernSDR closed fd 3; stopping");
        finish(exit_status::stopped);
    } else if (err == ECANCELED) {
        log_line("stopping on a signal while FernSDR was not reading fd 3");
        finish(exit_status::stopped);
    } else if (err == ETIMEDOUT) {
        log_line("FernSDR has not read fd 3 for %d ms; stopping", event_timeout_ms);
        finish(exit_status::internal);
    } else {
        log_line("writing an event to fd 3 failed: %s", std::strerror(err));
        finish(exit_status::internal);
    }
    return false;
}

void Session::fail(const Failure& f) {
    log_line("%s", f.message.c_str());
    json::Value e = json::Value::object();
    e.set("type", "error");
    e.set("code", error_code_name(f.code));
    e.set("message", f.message);
    e.set("fatal", true);
    send(e);
    finish(exit_status_for(f.code));
}

// On the watchdog's thread, while the session's thread waits inside the
// API. There is no way to take a call back from the API, so the real
// program reports and leaves: the process's end releases the API lock and
// the RSP (the lock is a robust mutex; measured with API 3.15, a killed
// holder frees it at once).
void Session::on_hang(const std::string& what) {
    if (stopping_.load()) {
        log_line("%s did not return while stopping; leaving without waiting for it", what.c_str());
        if (options_.exit_on_hang)
            _exit(exit_.value_or(exit_status::stopped));
        return;
    }
    const std::string message =
        what +
        " has not returned in time. Another program may hold the SDRplay API's lock (another FernSDR band that is "
        "starting or stuck, SDRuno, SDRconnect), or the sdrplay service hangs: sudo systemctl restart sdrplay";
    log_line("%s", message.c_str());
    if (!options_.exit_on_hang)
        return;
    json::Value e = json::Value::object();
    e.set("type", "error");
    e.set("code", "busy");
    e.set("message", message);
    e.set("fatal", true);
    send(e);
    _exit(exit_status::busy);
}

void Session::read_commands() {
    char buf[16384];
    const ssize_t n = ::read(io_.commands, buf, sizeof buf);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        log_line("reading fd 0 failed: %s; stopping", std::strerror(errno));
        commands_open_ = false;
        finish(exit_status::stopped);
        return;
    }
    if (n == 0) {
        commands_open_ = false;
        lines_.finish();
    } else {
        lines_.feed(buf, static_cast<size_t>(n));
    }
    LineReader::Line line;
    while (!exit_ && lines_.next(line))
        handle_line(line);
    if (!commands_open_ && !exit_) {
        log_line("fd 0 was closed; stopping");
        finish(exit_status::stopped);
    }
}

void Session::handle_line(const LineReader::Line& line) {
    if (line.too_long) {
        log_line("ignored a command longer than %zu bytes", max_line);
        return;
    }
    if (blank(line.text))
        return;
    json::Value message;
    std::string error;
    if (!json::parse(line.text, message, error)) {
        log_line("ignored a command that is not valid JSON: %s", error.c_str());
        return;
    }
    const json::Value* type = message.find("type");
    if (!type || !type->is_string()) {
        log_line("ignored a command without a type");
        return;
    }
    const std::string& t = type->as_string();
    if (t == "open") {
        handle_open(message);
    } else if (t == "set") {
        handle_set(message);
    } else if (t == "stop") {
        log_line("stopping as FernSDR asked");
        finish(exit_status::stopped);
    }
    // Any other type is ignored, so that the protocol can grow.
}

void Session::handle_open(const json::Value& message) {
    if (state_ != State::waiting || stream_) {
        log_line("ignored a second open");
        return;
    }
    const Clock::time_point deadline = Clock::now() + options_.open_timeout;
    OpenRequest request;
    if (auto f = parse_open(message, request)) {
        fail(*f);
        return;
    }
    RatePlan plan;
    if (auto f = plan_rate(request.sample_rate, request.settings.if_mode, request.settings.bandwidth_khz, plan)) {
        fail(*f);
        return;
    }
    const size_t ring = options_.ring_bytes != 0
                            ? options_.ring_bytes
                            : std::max<size_t>(8u << 20, static_cast<size_t>(request.sample_rate) * 2);
    stream_ = std::make_unique<Stream>(io_.samples, notify_fd_, ring, plan.decimation);
    if (auto f = receiver_.open(request, *stream_, deadline)) {
        fail(*f);
        return;
    }

    // A bigger pipe rides out short pauses in FernSDR: 16 MiB, 0.4 s at
    // 10 MHz, where the system allows it, and 1 MiB otherwise.
    if (::fcntl(io_.samples, F_SETPIPE_SZ, 16 << 20) < 0)
        (void)::fcntl(io_.samples, F_SETPIPE_SZ, 1 << 20);

    const Effective& e = receiver_.effective();
    json::Value ready = json::Value::object();
    ready.set("type", "ready");
    ready.set("format", "s16");
    ready.set("signal", "iq");
    ready.set("sample_rate", e.plan.output_hz);
    // rfFreq.rfHz as set: the API reports no tuned frequency of its own.
    ready.set("center", e.center);
    ready.set("device", receiver_.device_json());
    ready.set("settings", receiver_.settings_json());
    if (!send(ready))
        return;
    if (!stream_->start()) {
        fail(Failure{ErrorCode::internal, "could not start the writer thread"});
        return;
    }
    state_ = State::streaming;
    next_stats_ = Clock::now() + options_.stats_interval;
    next_drop_log_ = Clock::now();
    start_gain_control();
}

// With gain = auto, from the ladder step in use on; otherwise none.
void Session::start_gain_control() {
    gain_control_.reset();
    const auto& ladder = receiver_.ladder();
    if (!stream_ || receiver_.effective().gain != GainMode::automatic || ladder.size() < 2)
        return;
    std::vector<int> steps;
    for (const GainStep& s : ladder)
        steps.push_back(-s.reduction * 10);
    gain_control_.emplace(std::move(steps), receiver_.ladder_step(), Clock::now(), stream_->samples_received(),
                          stream_->samples_clipped(), options_.gain_timing);
}

void Session::handle_set(const json::Value& message) {
    const json::Value* id = message.find("id");
    // FernSDR's ids are small numbers; an absurd one cannot be echoed within
    // the line limit, so the answer goes without it.
    if (id && json::serialize(*id).size() > 1024) {
        log_line("a set carried an id longer than 1024 bytes");
        id = nullptr;
    }
    auto refuse = [&](const Failure& f) {
        log_line("refused a set: %s", f.message.c_str());
        json::Value e = json::Value::object();
        e.set("type", "error");
        if (id)
            e.set("id", *id);
        e.set("code", error_code_name(f.code));
        e.set("message", f.message);
        e.set("fatal", false);
        send(e);
    };
    if (state_ != State::streaming) {
        refuse(Failure{ErrorCode::invalid, "set arrived before the RSP was opened"});
        return;
    }
    LiveChange change;
    if (const json::Value* settings = message.find("settings"))
        if (auto f = parse_set(*settings, change)) {
            refuse(*f);
            return;
        }
    const GainMode before = receiver_.effective().gain;
    const auto failure = receiver_.apply(change);
    if (receiver_.effective().gain != before || (change.gain && *change.gain == GainMode::automatic))
        start_gain_control();
    if (failure) {
        // An API that stopped answering fails the band; a refused value only
        // the set.
        if (failure->code == ErrorCode::internal) {
            fail(*failure);
            return;
        }
        refuse(*failure);
        return;
    }
    json::Value applied = json::Value::object();
    applied.set("type", "applied");
    if (id)
        applied.set("id", *id);
    applied.set("settings", receiver_.settings_json(change));
    send(applied);
}

void Session::check_stream() {
    if (!stream_)
        return;
    switch (stream_->writer_end()) {
    case Stream::WriterEnd::host_gone:
        log_line("FernSDR closed fd 1; stopping");
        finish(exit_status::stopped);
        return;
    case Stream::WriterEnd::failed:
        fail(Failure{ErrorCode::internal,
                     std::string("writing samples to fd 1 failed: ") + std::strerror(stream_->writer_errno())});
        return;
    case Stream::WriterEnd::running:
    case Stream::WriterEnd::stopped:
        break;
    }
}

void Session::check_rate(Clock::time_point now) {
    if (rate_checked_)
        return;
    const uint64_t received = stream_->samples_received();
    if (!first_data_) {
        if (received > 0)
            first_data_ = now;
        return;
    }
    if (!rate_from_) {
        if (now - *first_data_ >= std::chrono::milliseconds(500)) {
            rate_from_ = now;
            rate_from_samples_ = received;
        }
        return;
    }
    const double seconds = std::chrono::duration<double>(now - *rate_from_).count();
    if (now - *rate_from_ < options_.rate_window)
        return;
    rate_checked_ = true;
    const double measured = static_cast<double>(received - rate_from_samples_) / seconds;
    const double announced = receiver_.effective().plan.output_hz;
    log_line("the RSP delivers %.0f samples a second (announced %.0f)", measured, announced);
    if (std::fabs(measured / announced - 1) > 0.1) {
        char text[400];
        std::snprintf(text, sizeof text,
                      "the RSP delivers about %.0f samples a second, not the %.0f this module announced, so the "
                      "band's frequencies would be wrong. This module's idea of the API is off for these settings "
                      "(converter %.0f Hz, decimation %u, IF %d kHz); please report it with the module version and "
                      "the RSP's model",
                      measured, announced, receiver_.effective().plan.fs_hz, receiver_.effective().plan.decimation,
                      receiver_.effective().plan.if_khz);
        fail(Failure{ErrorCode::internal, text});
    }
}

void Session::on_tick() {
    const Clock::time_point now = Clock::now();
    while (!exit_ && stream_->take_pending_ack()) {
        if (auto f = receiver_.acknowledge_overload()) {
            if (f->code == ErrorCode::internal) {
                fail(*f);
                return;
            }
            log_line("acknowledging an overload failed: %s", f->message.c_str());
        }
    }
    if (stream_->device_removed()) {
        fail(Failure{ErrorCode::lost, "the RSP was unplugged (the SDRplay API reported it removed). Check the cable "
                                      "and the USB port"});
        return;
    }
    if (stream_->device_failed()) {
        fail(Failure{ErrorCode::lost, "the SDRplay API reported that the RSP failed. If this repeats, unplug it, "
                                      "check the cable and the power supply, and restart the sdrplay service"});
        return;
    }
    if (now - stream_->last_data() > options_.stall_timeout) {
        const double seconds = std::chrono::duration<double>(options_.stall_timeout).count();
        char text[240];
        std::snprintf(text, sizeof text,
                      "the RSP sent no samples for %.1f seconds; the SDRplay API stopped delivering them. If this "
                      "repeats, check the USB connection and restart the sdrplay service",
                      seconds);
        fail(Failure{ErrorCode::lost, text});
        return;
    }
    check_rate(now);
    if (exit_)
        return;
    if (gain_control_) {
        const size_t before = gain_control_->step();
        const size_t step = gain_control_->update(now, stream_->samples_received(), stream_->samples_clipped(),
                                                  (stream_->take_peak() + 255) / 256);
        if (step != before) {
            if (auto f = receiver_.set_ladder_step(step)) {
                if (f->code == ErrorCode::internal || f->code == ErrorCode::busy) {
                    fail(*f);
                    return;
                }
                log_line("the gain control stops: %s", f->message.c_str());
                gain_control_.reset();
            } else {
                const GainStep& s = receiver_.ladder()[step];
                log_line("gain reduction %d dB (LNA state %u, IF %d dB): %s", s.reduction, s.lna_state,
                         s.if_reduction, gain_control_->reason().c_str());
            }
        }
        if (gain_control_ && gain_control_->clipping_at_lowest() && now >= next_lowest_log_) {
            log_line("the converter overloads even at the most gain reduction, %d dB: switch on a notch filter "
                     "(module.rf_notch, module.dab_notch), or put a filter or attenuator in front of the RSP",
                     receiver_.ladder()[gain_control_->step()].reduction);
            next_lowest_log_ = now + std::chrono::minutes(10);
        }
    }
    if (now >= next_stats_) {
        json::Value stats = json::Value::object();
        stats.set("type", "stats");
        stats.set("samples", stream_->samples_delivered());
        stats.set("dropped", stream_->samples_dropped());
        // The share of the samples that clipped since the last stats, of all
        // that arrived: those still in the ring too.
        const uint64_t received = stream_->samples_received();
        const uint64_t clipped = stream_->samples_clipped();
        stats.set("clipping", received > stats_received_
                                  ? static_cast<double>(clipped - stats_clipped_) /
                                        static_cast<double>(received - stats_received_)
                                  : 0.0);
        stats_received_ = received;
        stats_clipped_ = clipped;
        // Minus the gain reduction: 0 would be the RSP's most gain.
        if (gain_control_)
            stats.set("gain", -receiver_.ladder()[gain_control_->step()].reduction);
        if (!send(stats))
            return;
        next_stats_ += options_.stats_interval;
        if (next_stats_ <= now)
            next_stats_ = now + options_.stats_interval;
    }
    const uint64_t dropped = stream_->samples_dropped();
    if (dropped > logged_drops_ && now >= next_drop_log_) {
        log_line("%llu samples dropped so far: %llu the SDRplay API skipped, %llu because FernSDR did not read them "
                 "fast enough",
                 static_cast<unsigned long long>(dropped), static_cast<unsigned long long>(stream_->samples_skipped()),
                 static_cast<unsigned long long>(stream_->samples_dropped_here()));
        logged_drops_ = dropped;
        next_drop_log_ = now + std::chrono::seconds(10);
    }
    const uint64_t overloads = stream_->overload_events();
    const uint64_t jumps = stream_->discontinuities();
    if ((overloads > logged_overloads_ || jumps > logged_discontinuities_) && now >= next_overload_log_) {
        log_line("%llu converter overloads reported by the SDRplay API so far, %llu jumps in its sample numbers",
                 static_cast<unsigned long long>(overloads), static_cast<unsigned long long>(jumps));
        logged_overloads_ = overloads;
        logged_discontinuities_ = jumps;
        next_overload_log_ = now + std::chrono::seconds(60);
    }
}

SessionResult Session::run() {
    std::signal(SIGPIPE, SIG_IGN);
    notify_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (notify_fd_ < 0) {
        log_line("could not create an eventfd: %s", std::strerror(errno));
        return SessionResult{exit_status::internal, true};
    }

    // The event writes wait with a limit and watch for a stop, which only a
    // non-blocking fd allows. This end is the module's own, so the flag
    // changes nothing for FernSDR.
    const int event_flags = ::fcntl(io_.events, F_GETFL);
    if (event_flags >= 0)
        (void)::fcntl(io_.events, F_SETFL, event_flags | O_NONBLOCK);

    json::Value hello = json::Value::object();
    hello.set("type", "hello");
    hello.set("api", module_api);
    hello.set("id", module_id);
    hello.set("version", module_version());
    hello.set("kind", "input");
    send(hello);

    while (!exit_) {
        struct pollfd fds[3];
        int count = 0;
        int commands = -1;
        int stop = -1;
        if (commands_open_) {
            fds[count] = {io_.commands, POLLIN, 0};
            commands = count++;
        }
        if (io_.stop >= 0) {
            fds[count] = {io_.stop, POLLIN, 0};
            stop = count++;
        }
        fds[count] = {notify_fd_, POLLIN, 0};
        const int notify = count++;

        // While streaming, wake up often enough for stats and the stall check.
        const int timeout = state_ == State::streaming ? 100 : -1;
        const int r = ::poll(fds, static_cast<nfds_t>(count), timeout);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            fail(Failure{ErrorCode::internal, std::string("poll failed: ") + std::strerror(errno)});
            break;
        }
        if (stop >= 0 && fds[stop].revents != 0) {
            char buf[128];
            const ssize_t ignored = ::read(io_.stop, buf, sizeof buf);
            (void)ignored;
            log_line("stopping on a signal");
            finish(exit_status::stopped);
            break;
        }
        if (commands >= 0 && fds[commands].revents != 0)
            read_commands();
        if (exit_)
            break;
        if (fds[notify].revents != 0) {
            uint64_t n;
            const ssize_t ignored = ::read(notify_fd_, &n, sizeof n);
            (void)ignored;
            check_stream();
        }
        if (!exit_ && state_ == State::streaming)
            on_tick();
    }
    return shut_down();
}

SessionResult Session::shut_down() {
    stopping_.store(true);
    SessionResult result{exit_.value_or(exit_status::stopped), true};
    // One budget for everything, so that the module is gone before FernSDR
    // resorts to SIGTERM.
    const Clock::time_point deadline = Clock::now() + options_.shutdown_timeout;
    bool writer_stopped = true;
    if (stream_) {
        stream_->request_stop();
        writer_stopped = stream_->wait(deadline);
        if (!writer_stopped)
            log_line("the writer did not stop within %lld ms", static_cast<long long>(options_.shutdown_timeout.count()));
    }
    // Uninit ends the API's calls into the stream; ReleaseDevice and Close
    // leave the service as the specification wants it, for the next band.
    if (!receiver_.close_by(std::max(deadline, Clock::now() + std::chrono::milliseconds(250)))) {
        log_line("closing the SDRplay API did not finish in time; exiting without waiting for it");
        result.clean = false;
    }
    if (!writer_stopped || !result.clean) {
        // A thread may still run inside the stream; it must never be freed.
        (void)stream_.release();
        leaked_ = true;
        result.clean = false;
    }
    return result;
}

}  // namespace

SessionResult run_session(Api& api, const SessionIo& io, const SessionOptions& options) {
    Session session(api, io, options);
    return session.run();
}

}  // namespace fern
