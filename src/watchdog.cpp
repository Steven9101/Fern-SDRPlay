// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "watchdog.h"

namespace fern {

Watchdog::Watchdog(Handler handler) : handler_(std::move(handler)), thread_(&Watchdog::main, this) {}

Watchdog::~Watchdog() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    thread_.join();
}

Watchdog::Scope::Scope(Watchdog& w, const char* what, Clock::time_point deadline) : w_(w) {
    {
        std::lock_guard<std::mutex> lock(w_.mutex_);
        w_.armed_ = true;
        w_.expired_ = false;
        w_.what_ = what;
        w_.deadline_ = deadline;
        ++w_.generation_;
    }
    w_.wake_.notify_all();
}

Watchdog::Scope::~Scope() {
    {
        std::lock_guard<std::mutex> lock(w_.mutex_);
        w_.armed_ = false;
    }
    w_.wake_.notify_all();
}

bool Watchdog::Scope::expired() const {
    std::lock_guard<std::mutex> lock(w_.mutex_);
    return w_.expired_;
}

void Watchdog::main() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
        if (!armed_) {
            wake_.wait(lock);
            continue;
        }
        const uint64_t generation = generation_;
        if (wake_.wait_until(lock, deadline_, [&] { return quit_ || !armed_ || generation_ != generation; }))
            continue;
        expired_ = true;
        armed_ = false;
        const std::string what = what_;
        lock.unlock();
        handler_(what);
        lock.lock();
    }
}

}  // namespace fern
