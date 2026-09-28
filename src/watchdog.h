// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// A deadline for calls into SDRplay's API, which can block without limit:
// every call waits while another process holds the API's lock, and the
// service may stop answering. The module cannot interrupt such a call, so
// when one outlasts its deadline the handler runs on the watchdog's own
// thread; in the real program it reports the hang and ends the process,
// which also releases whatever the process held of the API.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace fern {

class Watchdog {
public:
    using Clock = std::chrono::steady_clock;
    // Called at most once per armed call, with the call's name.
    using Handler = std::function<void(const std::string& what)>;

    explicit Watchdog(Handler handler);
    ~Watchdog();
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

    // Watches one call while it lives.
    class Scope {
    public:
        Scope(Watchdog& w, const char* what, Clock::time_point deadline);
        ~Scope();
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;
        // Whether the call outlasted its deadline.
        bool expired() const;

    private:
        Watchdog& w_;
    };

private:
    void main();

    Handler handler_;
    std::mutex mutex_;
    std::condition_variable wake_;
    bool quit_ = false;
    bool armed_ = false;
    bool expired_ = false;
    std::string what_;
    Clock::time_point deadline_;
    uint64_t generation_ = 0;
    std::thread thread_;
};

}  // namespace fern
