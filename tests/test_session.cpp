// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// Runs the session loop in a thread with real pipes in place of fds 0 to 3,
// against the fake API, and plays FernSDR's part.
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "fake_control.h"
#include "json.h"
#include "session.h"
#include "test.h"

namespace r = fern::rsp;
using fern::json::Value;
using Clock = std::chrono::steady_clock;

namespace {

struct Pipe {
    int r = -1;
    int w = -1;
    Pipe() {
        int fds[2];
        if (::pipe2(fds, O_CLOEXEC) == 0) {
            r = fds[0];
            w = fds[1];
        }
    }
    ~Pipe() {
        close_r();
        close_w();
    }
    void close_r() {
        if (r >= 0)
            ::close(r);
        r = -1;
    }
    void close_w() {
        if (w >= 0)
            ::close(w);
        w = -1;
    }
};

fern::SessionOptions quick_options() {
    fern::SessionOptions o;
    o.stats_interval = std::chrono::milliseconds(100);
    o.stall_timeout = std::chrono::milliseconds(800);
    o.shutdown_timeout = std::chrono::milliseconds(1500);
    o.rate_window = std::chrono::milliseconds(1000);
    o.ring_bytes = 1 << 20;
    o.usb_devices_dir = "/nonexistent";
    o.exit_on_hang = false;
    return o;
}

const char* const open_line =
    "{\"type\":\"open\",\"sample_rate\":2000000,\"center\":7100000,\"signal\":\"iq\",\"settings\":{}}";

class Harness {
public:
    explicit Harness(fern::SessionOptions options = quick_options(), fern::Api& api = fake::api()) {
        REQUIRE(commands_.r >= 0 && samples_.r >= 0 && events_.r >= 0 && stop_.r >= 0);
        fern::SessionIo io;
        io.commands = commands_.r;
        io.samples = samples_.w;
        io.events = events_.w;
        io.stop = stop_.r;
        thread_ = std::thread([this, &api, io, options] {
            result_ = fern::run_session(api, io, options);
            done_.store(true);
        });
    }

    ~Harness() {
        // Whatever happened, make the session end so that the thread can be
        // joined: this is what FernSDR's leaving looks like.
        commands_.close_w();
        stop_draining();
        samples_.close_r();
        events_.close_r();
        if (thread_.joinable())
            thread_.join();
    }

    void send(const std::string& line) {
        const std::string text = line + "\n";
        REQUIRE(::write(commands_.w, text.data(), text.size()) == static_cast<ssize_t>(text.size()));
    }
    void close_commands() { commands_.close_w(); }
    void close_samples() {
        stop_draining();
        samples_.close_r();
    }
    void signal_stop() { REQUIRE(::write(stop_.w, "x", 1) == 1); }

    // The next event on fd 3, or null after the timeout.
    Value event(int timeout_ms = 3000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const size_t nl = events_buf_.find('\n');
            if (nl != std::string::npos) {
                const std::string line = events_buf_.substr(0, nl);
                events_buf_.erase(0, nl + 1);
                Value v;
                std::string error;
                if (!fern::json::parse(line, v, error))
                    test::report(__FILE__, __LINE__, "event is not valid JSON: " + line + ": " + error);
                lines_.push_back(line);
                return v;
            }
            const int left = static_cast<int>(
                std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count());
            if (left <= 0)
                return Value();
            struct pollfd pfd = {events_.r, POLLIN, 0};
            if (::poll(&pfd, 1, left) <= 0)
                continue;
            char buf[4096];
            const ssize_t n = ::read(events_.r, buf, sizeof buf);
            if (n <= 0)
                return Value();
            events_buf_.append(buf, static_cast<size_t>(n));
        }
    }

    // The next event of the given type, skipping stats.
    Value expect(const std::string& type, int timeout_ms = 3000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            Value v = left > 0 ? event(static_cast<int>(left)) : Value();
            if (v.is_null()) {
                test::report(__FILE__, __LINE__, "no " + type + " event");
                throw test::Stop{};
            }
            const std::string t = v.find("type") ? v.find("type")->as_string() : "";
            if (t == type)
                return v;
            if (t != "stats") {
                test::report(__FILE__, __LINE__, "expected " + type + ", got " + lines_.back());
                throw test::Stop{};
            }
        }
    }

    // The first stats for which pred holds, within the timeout.
    template <typename Pred>
    Value stats_where(Pred pred, int timeout_ms = 5000) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (Clock::now() < deadline) {
            const Value v = expect("stats", timeout_ms);
            if (pred(v))
                return v;
        }
        test::report(__FILE__, __LINE__, "no stats as expected");
        throw test::Stop{};
    }

    // Reads exactly n sample bytes.
    std::vector<uint8_t> samples(size_t n, int timeout_ms = 3000) {
        std::vector<uint8_t> out;
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (out.size() < n && Clock::now() < deadline) {
            struct pollfd pfd = {samples_.r, POLLIN, 0};
            if (::poll(&pfd, 1, 50) <= 0)
                continue;
            std::vector<uint8_t> buf(n - out.size());
            const ssize_t got = ::read(samples_.r, buf.data(), buf.size());
            if (got <= 0)
                break;
            out.insert(out.end(), buf.begin(), buf.begin() + got);
        }
        return out;
    }

    // Reads and discards samples in a thread of its own, as FernSDR would.
    void start_draining() {
        drainer_ = std::thread([this] {
            std::vector<uint8_t> buf(1 << 16);
            while (!drain_stop_.load()) {
                struct pollfd pfd = {samples_.r, POLLIN, 0};
                if (::poll(&pfd, 1, 20) <= 0)
                    continue;
                if (::read(samples_.r, buf.data(), buf.size()) <= 0)
                    break;
            }
        });
    }

    bool samples_pending() {
        struct pollfd pfd = {samples_.r, POLLIN, 0};
        return ::poll(&pfd, 1, 0) > 0;
    }

    int exit_status(int timeout_ms = 5000, bool clean = true) {
        const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        while (!done_.load() && Clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        REQUIRE(done_.load());
        thread_.join();
        CHECK_EQ(result_.clean, clean);
        return result_.status;
    }

    const std::vector<std::string>& lines() const { return lines_; }

private:
    void stop_draining() {
        drain_stop_.store(true);
        if (drainer_.joinable())
            drainer_.join();
    }

    Pipe commands_;
    Pipe samples_;
    Pipe events_;
    Pipe stop_;
    std::thread thread_;
    std::thread drainer_;
    std::atomic<bool> drain_stop_{false};
    std::atomic<bool> done_{false};
    fern::SessionResult result_;
    std::string events_buf_;
    std::vector<std::string> lines_;
};

void check_hello(Harness& h) {
    const Value hello = h.event();
    REQUIRE(hello.is_object());
    CHECK_EQ(fern::json::serialize(hello),
             std::string("{\"type\":\"hello\",\"api\":1,\"id\":\"sdrplay\",\"version\":\"") + FERN_SDRPLAY_VERSION +
                 "\",\"kind\":\"input\"}");
}

std::string text_of(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f && f->is_string() ? f->as_string() : "";
}

double number_of(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f && f->is_number() ? f->as_number() : NAN;
}

// The pairs on fd 1 are the fake's pattern, in order and unbroken, from
// wherever the stream was when ready went out.
bool pairs_follow_the_pattern(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 4)
        return false;
    std::vector<int16_t> v(bytes.size() / 2);
    std::memcpy(v.data(), bytes.data(), v.size() * 2);
    const uint64_t start = static_cast<uint64_t>(v[0] + 8192);
    for (size_t k = 0; 2 * k + 1 < v.size(); ++k)
        if (v[2 * k] != fake::pattern_i(start + k) || v[2 * k + 1] != fake::pattern_q(start + k))
            return false;
    return true;
}

std::string calls(fake::State& s) {
    std::lock_guard<std::mutex> lock(s.mutex);
    std::string out;
    for (const std::string& c : s.calls)
        out += (out.empty() ? "" : " ") + c;
    return out;
}


}  // namespace

TEST(session_full_exchange) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "1A0001")};
    Harness h;
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":2000000,\"center\":7100000,\"signal\":\"iq\","
           "\"settings\":{\"gain\":\"auto\",\"rf_notch\":true}}");
    const Value ready = h.expect("ready", 5000);
    CHECK_EQ(text_of(ready, "format"), std::string("s16"));
    CHECK_EQ(text_of(ready, "signal"), std::string("iq"));
    CHECK_EQ(number_of(ready, "sample_rate"), 2000000.0);
    CHECK_EQ(number_of(ready, "center"), 7100000.0);
    CHECK_EQ(text_of(*ready.find("device"), "name"), std::string("RSP1A"));
    CHECK_EQ(text_of(*ready.find("device"), "serial"), std::string("1A0001"));
    CHECK_EQ(text_of(*ready.find("settings"), "gain"), std::string("auto"));
    CHECK(ready.find("settings")->find("rf_notch")->as_bool());

    // Every pair on fd 1 is the fake's, interleaved and in order.
    const std::vector<uint8_t> bytes = h.samples(400000);
    REQUIRE(bytes.size() == 400000);
    CHECK(pairs_follow_the_pattern(bytes));
    h.start_draining();

    const Value stats = h.expect("stats");
    CHECK(number_of(stats, "samples") > 0);
    CHECK_EQ(number_of(stats, "clipping"), 0.0);
    CHECK(number_of(stats, "gain") < 0);

    h.send("{\"type\":\"set\",\"id\":7,\"settings\":{\"gain\":\"manual\",\"lna_state\":2,\"if_gain_reduction\":45}}");
    CHECK_EQ(fern::json::serialize(h.expect("applied")),
             std::string("{\"type\":\"applied\",\"id\":7,\"settings\":{\"gain\":\"manual\",\"lna_state\":2,"
                         "\"if_gain_reduction\":45,\"gain_reduction\":57}}"));
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        CHECK_EQ(s.updates.back().lna_state, 2u);
        CHECK_EQ(s.updates.back().if_reduction, 45);
    }
    // A manual gain is not the module's to report.
    CHECK(h.expect("stats").find("gain") == nullptr);

    h.send("{\"type\":\"set\",\"id\":8,\"settings\":{\"antenna\":\"b\"}}");
    const Value refused = h.expect("error");
    CHECK_EQ(number_of(refused, "id"), 8.0);
    CHECK_HAS(text_of(refused, "message"), "restart the band");
    CHECK(!refused.find("fatal")->as_bool());
    h.send("{\"type\":\"set\",\"id\":9,\"settings\":{\"bias_tee\":true,\"am_notch\":true}}");
    const Value refused2 = h.expect("error");
    CHECK_HAS(text_of(refused2, "message"), "MW notch");
    CHECK(!refused2.find("fatal")->as_bool());

    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
    CHECK_HAS(calls(s), "Uninit ReleaseDevice Close");
    CHECK(!s.opened);
}

TEST(session_acknowledges_every_overload_and_counts_it_as_clipping) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rspdx, "DX1")};
    s.cfg.overload_after = 50;     // 25 ms of 2 MHz after Init
    s.cfg.corrected_after = 1200;  // 0.6 s
    Harness h;
    check_hello(h);
    h.send("{\"type\":\"open\",\"sample_rate\":2000000,\"center\":7100000,\"signal\":\"iq\","
           "\"settings\":{\"gain\":\"manual\"}}");
    h.expect("ready", 5000);
    h.start_draining();
    h.stats_where([](const Value& v) { return number_of(v, "clipping") > 0.5; });
    h.stats_where([](const Value& v) { return number_of(v, "clipping") == 0.0; });
    int acks = 0;
    for (int i = 0; i < 100 && acks < 2; ++i) {
        {
            std::lock_guard<std::mutex> lock(s.mutex);
            acks = s.acks;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK_EQ(acks, 2);
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_counts_samples_the_api_skipped_as_dropped) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    s.cfg.gap_after = 400;
    s.cfg.gap_samples = 5000;
    Harness h;
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    h.start_draining();
    const Value v = h.stats_where([](const Value& st) { return number_of(st, "dropped") > 0; });
    CHECK_EQ(number_of(v, "dropped"), 5000.0);
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_auto_gain_backs_off_when_the_converter_clips_and_climbs_when_it_stops) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    s.loud.store(true);
    fern::SessionOptions o = quick_options();
    o.gain_timing.settle = std::chrono::milliseconds(150);
    o.gain_timing.hold_start = std::chrono::milliseconds(400);
    Harness h(o);
    check_hello(h);
    h.send(open_line);
    const Value ready = h.expect("ready", 5000);
    const double start = -number_of(*ready.find("settings"), "gain_reduction");
    h.start_draining();
    // The loud pattern clips about half its samples: 6 dB down at a time.
    const Value lower = h.stats_where([&](const Value& v) { return number_of(v, "gain") <= start - 6; });
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        REQUIRE(!s.updates.empty());
        CHECK((s.updates.back().reason & r::update::tuner_gr) != 0);
    }
    s.loud.store(false);
    const double bottom = number_of(h.stats_where([&](const Value& v) { return number_of(v, "clipping") == 0.0; }),
                                    "gain");
    // Quiet again, with the peaks 12 dB under full scale: it climbs.
    h.stats_where([&](const Value& v) { return number_of(v, "gain") > bottom; }, 6000);
    (void)lower;
    h.send("{\"type\":\"stop\"}");
    CHECK_EQ(h.exit_status(), 0);
}

TEST(session_reports_a_removed_rsp_as_lost) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    s.cfg.remove_after = 100;
    Harness h;
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    h.start_draining();
    const Value e = h.expect("error");
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK(e.find("fatal")->as_bool());
    CHECK_HAS(text_of(e, "message"), "unplugged");
    CHECK_EQ(h.exit_status(), 5);
    CHECK_HAS(calls(s), "Uninit ReleaseDevice Close");
}

TEST(session_keeps_the_stream_when_uninit_fails) {
    // A failed Uninit does not say that the API's stream callbacks have
    // ended, so the stream they call into must outlive the session.
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    s.cfg.uninit_error = r::err::fail;
    {
        Harness h;
        check_hello(h);
        h.send(open_line);
        h.expect("ready", 5000);
        h.start_draining();
        h.send("{\"type\":\"stop\"}");
        CHECK_EQ(h.exit_status(5000, false), 0);
        CHECK_HAS(calls(s), "Uninit ReleaseDevice Close");
        // The callbacks go on for a while after the session has ended.
        const uint64_t before = s.callbacks.load();
        const auto until = Clock::now() + std::chrono::seconds(2);
        while (s.callbacks.load() < before + 20 && Clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        CHECK(s.callbacks.load() >= before + 20);
    }
    fake::fresh();
}

TEST(session_reports_a_stall) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    s.cfg.stall_after = 200;
    Harness h;
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    h.start_draining();
    const Value e = h.expect("error", 4000);
    CHECK_EQ(text_of(e, "code"), std::string("lost"));
    CHECK_HAS(text_of(e, "message"), "no samples for 0.8 seconds");
    CHECK_EQ(h.exit_status(), 5);
    CHECK_HAS(calls(s), "Uninit ReleaseDevice Close");
}

TEST(session_refuses_to_go_on_when_the_delivered_rate_is_not_the_announced_one) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    s.cfg.rate_factor = 0.5;
    Harness h;
    check_hello(h);
    h.send(open_line);
    h.expect("ready", 5000);
    h.start_draining();
    const Value e = h.expect("error", 5000);
    CHECK_EQ(text_of(e, "code"), std::string("internal"));
    CHECK_HAS(text_of(e, "message"), "not the 2000000 this module announced");
    CHECK_EQ(h.exit_status(), 1);
}

TEST(session_reports_what_is_missing_before_any_sample) {
    {
        fake::fresh();
        Harness h;
        check_hello(h);
        h.send(open_line);
        const Value e = h.expect("error");
        CHECK_EQ(text_of(e, "code"), std::string("no-device"));
        CHECK_EQ(h.exit_status(), 3);
        CHECK(!h.samples_pending());
    }
    {
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
        s.cfg.version = 3.16f;
        Harness h;
        check_hello(h);
        h.send(open_line);
        CHECK_HAS(text_of(h.expect("error"), "message"), "3.16");
        CHECK_EQ(h.exit_status(), 1);
    }
    {
        fern::Api missing({"/nonexistent/libsdrplay_api.so.3"});
        Harness h(quick_options(), missing);
        check_hello(h);
        h.send(open_line);
        const Value e = h.expect("error");
        CHECK_HAS(text_of(e, "message"), "https://www.sdrplay.com/api/");
        CHECK_EQ(h.exit_status(), 1);
    }
    {
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
        s.cfg.lock_ms = 1200;
        fern::SessionOptions o = quick_options();
        o.open_timeout = std::chrono::milliseconds(300);
        Harness h(o);
        check_hello(h);
        h.send(open_line);
        const Value e = h.expect("error", 4000);
        CHECK_EQ(text_of(e, "code"), std::string("busy"));
        CHECK_HAS(text_of(e, "message"), "did not return in time");
        CHECK_EQ(h.exit_status(), 4);
        CHECK(!s.locked);
    }
}

TEST(session_refuses_bad_settings_before_touching_the_api) {
    fake::State& s = fake::fresh();
    s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
    {
        Harness h;
        check_hello(h);
        h.send("{\"type\":\"open\",\"sample_rate\":2000000,\"center\":7100000,\"signal\":\"real\"}");
        CHECK_HAS(text_of(h.expect("error"), "message"), "set signal = iq");
        CHECK_EQ(h.exit_status(), 6);
    }
    {
        Harness h;
        check_hello(h);
        h.send("{\"type\":\"set\",\"id\":1,\"settings\":{\"bias_tee\":true}}");
        const Value e = h.expect("error");
        CHECK(!e.find("fatal")->as_bool());
        CHECK_HAS(text_of(e, "message"), "before the RSP was opened");
        h.send("{\"type\":\"open\",\"sample_rate\":2000000,\"center\":7100000,\"signal\":\"iq\","
               "\"settings\":{\"squelch\":3}}");
        CHECK_HAS(text_of(h.expect("error"), "message"), "module.squelch");
        CHECK_EQ(h.exit_status(), 6);
    }
    CHECK(calls(s).empty());
}

TEST(session_exits_on_eof_on_a_closed_sample_pipe_and_on_a_signal) {
    {
        fake::fresh();
        Harness h;
        check_hello(h);
        h.close_commands();
        CHECK_EQ(h.exit_status(), 0);
    }
    {
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
        Harness h;
        check_hello(h);
        h.send(open_line);
        h.expect("ready", 5000);
        h.close_samples();
        CHECK_EQ(h.exit_status(), 0);
        CHECK_HAS(calls(s), "Uninit ReleaseDevice Close");
    }
    {
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
        Harness h;
        check_hello(h);
        h.send(open_line);
        h.expect("ready", 5000);
        h.signal_stop();
        CHECK_EQ(h.exit_status(), 0);
        CHECK_HAS(calls(s), "Uninit ReleaseDevice Close");
    }
}

TEST(session_auto_gain_retries_a_refused_step_and_says_when_it_gives_up) {
    {
        // The API refuses two gain changes, then takes them again: the
        // control keeps trying and gets the gain down.
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
        s.cfg.update_error = r::err::gain_update_error;
        s.cfg.update_error_on = r::update::tuner_gr;
        s.cfg.update_error_count = 2;
        s.loud.store(true);
        fern::SessionOptions o = quick_options();
        o.gain_timing.settle = std::chrono::milliseconds(100);
        Harness h(o);
        check_hello(h);
        h.send(open_line);
        const Value ready = h.expect("ready", 5000);
        const double start = -number_of(*ready.find("settings"), "gain_reduction");
        h.start_draining();
        h.stats_where([&](const Value& v) { return number_of(v, "gain") < start; }, 6000);
        CHECK_EQ(s.failed_updates, 2);
        h.send("{\"type\":\"stop\"}");
        CHECK_EQ(h.exit_status(), 0);
    }
    {
        // It refuses every one: after a few tries the module says, in an
        // error FernSDR logs, that auto gain has stopped, and streams on.
        fake::State& s = fake::fresh();
        s.cfg.devices = {fake::device(r::hw::rsp1a, "A")};
        s.cfg.update_error = r::err::gain_update_error;
        s.cfg.update_error_on = r::update::tuner_gr;
        s.loud.store(true);
        fern::SessionOptions o = quick_options();
        o.gain_timing.settle = std::chrono::milliseconds(100);
        Harness h(o);
        check_hello(h);
        h.send(open_line);
        h.expect("ready", 5000);
        h.start_draining();
        const Value e = h.expect("error", 8000);
        CHECK(!e.find("fatal")->as_bool());
        CHECK(e.find("id") == nullptr);
        CHECK_HAS(text_of(e, "message"), "gain = auto has stopped");
        CHECK_EQ(s.failed_updates, 5);
        // No more tries, and no gain in the stats, which only a module that
        // sets the gain reports.
        const Value later = h.expect("stats");
        CHECK(later.find("gain") == nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        CHECK_EQ(s.failed_updates, 5);
        h.send("{\"type\":\"stop\"}");
        CHECK_EQ(h.exit_status(), 0);
    }
}

TEST(session_gives_the_api_most_of_the_time_before_fernsdr_kills_the_module) {
    // FernSDR sends SIGKILL 4 s after stop; the SIGTERM at 2 s only reaches
    // the signalfd. Closing the API may take nearly all of that.
    const fern::SessionOptions defaults;
    CHECK(defaults.shutdown_timeout >= std::chrono::milliseconds(3000));
    CHECK(defaults.shutdown_timeout <= std::chrono::milliseconds(3600));
}
