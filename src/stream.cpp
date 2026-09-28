// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include "stream.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <system_error>
#include <unistd.h>

#include "log.h"

namespace fern {

namespace {

size_t round_up_pow2(size_t n) {
    size_t p = 1;
    while (p < n)
        p <<= 1;
    return p;
}

int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

extern "C" void wake_writer(int) {}

// SIGUSR1 interrupts a writer blocked in write(); without SA_RESTART the call
// returns EINTR, and the writer then sees that it should stop.
void install_wake_handler() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa;
        std::memset(&sa, 0, sizeof sa);
        sa.sa_handler = wake_writer;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;
        sigaction(SIGUSR1, &sa, nullptr);
    });
}

}  // namespace

RingBuffer::RingBuffer(size_t capacity)
    : buf_(new uint8_t[round_up_pow2(std::max<size_t>(capacity, 2))]),
      mask_(round_up_pow2(std::max<size_t>(capacity, 2)) - 1) {}

size_t RingBuffer::size() const {
    return head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire);
}

bool RingBuffer::push(const uint8_t* data, size_t len) {
    const size_t head = head_.load(std::memory_order_relaxed);
    const size_t tail = tail_.load(std::memory_order_acquire);
    if (len > capacity() - (head - tail))
        return false;
    const size_t pos = head & mask_;
    const size_t first = std::min(len, capacity() - pos);
    std::memcpy(buf_.get() + pos, data, first);
    std::memcpy(buf_.get(), data + first, len - first);
    head_.store(head + len, std::memory_order_release);
    return true;
}

size_t RingBuffer::peek(const uint8_t** data) const {
    const size_t tail = tail_.load(std::memory_order_relaxed);
    const size_t available = head_.load(std::memory_order_acquire) - tail;
    if (available == 0)
        return 0;
    const size_t pos = tail & mask_;
    *data = buf_.get() + pos;
    return std::min(available, capacity() - pos);
}

void RingBuffer::consume(size_t len) {
    tail_.store(tail_.load(std::memory_order_relaxed) + len, std::memory_order_release);
}

Stream::Stream(int samples_fd, int notify_fd, size_t ring_bytes, unsigned decimation)
    : samples_fd_(samples_fd),
      notify_fd_(notify_fd),
      decimation_(decimation == 0 ? 1 : decimation),
      ring_(ring_bytes),
      data_event_(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)),
      scratch_(new int16_t[chunk_samples * 2]) {
    last_data_ns_.store(now_ns());
}

Stream::~Stream() {
    if (writer_.joinable()) {
        // The owner waits for the writer first; one that is still running
        // here would crash the process a moment later anyway.
        request_stop();
        if (!wait(std::chrono::steady_clock::now() + std::chrono::seconds(2))) {
            log_line("internal error: the writer thread did not stop");
            std::abort();
        }
    }
    if (data_event_ >= 0)
        ::close(data_event_);
}

void Stream::notify(int fd) {
    if (fd < 0)
        return;
    // Both fds are non-blocking eventfds: this never waits, in either
    // callback thread.
    const uint64_t one = 1;
    const ssize_t ignored = ::write(fd, &one, sizeof one);
    (void)ignored;
}

std::chrono::steady_clock::time_point Stream::last_data() const {
    return std::chrono::steady_clock::time_point(std::chrono::nanoseconds(last_data_ns_.load()));
}

bool Stream::start() {
    install_wake_handler();
    last_data_ns_.store(now_ns());
    try {
        writer_ = std::thread(&Stream::writer_main, this);
    } catch (const std::system_error& e) {
        log_line("could not start the writer thread: %s", e.what());
        return false;
    }
    accepting_.store(true);
    return true;
}

bool Stream::take_pending_ack() {
    uint64_t n = pending_acks_.load();
    while (n > 0)
        if (pending_acks_.compare_exchange_weak(n, n - 1))
            return true;
    return false;
}

void Stream::on_stream(short* xi, short* xq, rsp::StreamCbParamsT* params, unsigned int count, unsigned int reset,
                       void* ctx) {
    static_cast<Stream*>(ctx)->handle_samples(xi, xq, params, count, reset != 0);
}

void Stream::account_step(uint32_t step, unsigned count) {
    const uint32_t expected = static_cast<uint32_t>(static_cast<uint64_t>(count) * stride_);
    if (step == expected)
        return;
    const uint32_t gap = step - expected;
    if (gap < 0x80000000u)
        skipped_.fetch_add(gap / stride_, std::memory_order_relaxed);
    else
        discontinuities_.fetch_add(1, std::memory_order_relaxed);
}

void Stream::track_numbers(uint32_t first, unsigned count, bool reset) {
    // The API numbers its samples, 32 bits and wrapping (StreamCbParamsT.
    // firstSampleNum), but the specification does not say in what: delivered
    // samples, converter samples before decimation, or, in low IF, samples
    // at 6 MHz, three for each one delivered at 2 MHz. So the step per
    // delivered sample is learnt: 1, 3, the decimation or three times it,
    // once two pairs of callbacks in a row agree on it, and learnt again
    // after a reset, which restarts the numbering on purpose. A step beyond
    // the expected one is samples the API lost; the pairs seen while
    // learning are counted once the step is known, a gap in the very first
    // among them too.
    if (reset || !have_previous_) {
        if (reset && have_previous_)
            resets_.fetch_add(1, std::memory_order_relaxed);
        if (reset) {
            stride_ = 0;
            candidate_ = 0;
            agreed_ = 0;
            pending_ = 0;
        }
        have_previous_ = true;
        previous_first_ = first;
        previous_count_ = count;
        return;
    }
    const uint32_t step = first - previous_first_;
    const unsigned before = previous_count_;
    previous_first_ = first;
    previous_count_ = count;
    if (stride_ != 0) {
        account_step(step, before);
        return;
    }
    unsigned k = 0;
    if (before > 0 && step % before == 0) {
        const uint32_t ratio = step / before;
        if (ratio == 1 || ratio == 3 || ratio == decimation_ || ratio == 3 * decimation_)
            k = ratio;
    }
    if (k != 0 && k == candidate_) {
        ++agreed_;
    } else if (k != 0) {
        candidate_ = k;
        agreed_ = 1;
    }
    if (pending_ == max_pending) {
        std::memmove(pending_step_, pending_step_ + 1, sizeof pending_step_ - sizeof pending_step_[0]);
        std::memmove(pending_count_, pending_count_ + 1, sizeof pending_count_ - sizeof pending_count_[0]);
        --pending_;
    }
    pending_step_[pending_] = step;
    pending_count_[pending_] = before;
    ++pending_;
    if (agreed_ < 2)
        return;
    stride_ = candidate_;
    for (size_t i = 0; i < pending_; ++i)
        account_step(pending_step_[i], pending_count_[i]);
    pending_ = 0;
}

void Stream::handle_samples(const short* xi, const short* xq, const rsp::StreamCbParamsT* params, unsigned count,
                            bool reset) {
    last_data_ns_.store(now_ns(), std::memory_order_relaxed);
    if (!accepting_.load(std::memory_order_relaxed) || count == 0 || !xi || !xq)
        return;
    received_.fetch_add(count, std::memory_order_relaxed);
    if (params)
        track_numbers(params->firstSampleNum, count, reset);
    // A converter overload the API reported counts every sample of the
    // callbacks it covers as clipped, a short one that came and went
    // between two callbacks the next callback's.
    const bool overload = overloaded_.load(std::memory_order_relaxed) |
                          overload_pulse_.exchange(false, std::memory_order_relaxed);
    unsigned peak = 0;
    uint64_t clipped = 0;
    for (unsigned done = 0; done < count;) {
        const unsigned n = static_cast<unsigned>(std::min<size_t>(count - done, chunk_samples));
        int16_t* out = scratch_.get();
        // Branch-free, so that it keeps up with 10 MHz in this thread.
        for (unsigned k = 0; k < n; ++k) {
            const int i = xi[done + k];
            const int q = xq[done + k];
            out[2 * k] = static_cast<int16_t>(i);
            out[2 * k + 1] = static_cast<int16_t>(q);
            const unsigned a = static_cast<unsigned>(i < 0 ? -i : i);
            const unsigned b = static_cast<unsigned>(q < 0 ? -q : q);
            const unsigned m = a > b ? a : b;
            peak = m > peak ? m : peak;
            clipped += m >= static_cast<unsigned>(clip_level);
        }
        const size_t bytes = static_cast<size_t>(n) * 4;
        if (!ring_.push(reinterpret_cast<const uint8_t*>(out), bytes))
            bytes_dropped_.fetch_add(bytes, std::memory_order_relaxed);
        done += n;
    }
    clipped_.fetch_add(overload ? count : clipped, std::memory_order_relaxed);
    unsigned previous = peak_.load(std::memory_order_relaxed);
    while (peak > previous && !peak_.compare_exchange_weak(previous, peak, std::memory_order_relaxed)) {
    }
    notify(data_event_);
}

void Stream::on_event(rsp::EventId id, rsp::TunerSelect, rsp::EventParamsT* params, void* ctx) {
    Stream* s = static_cast<Stream*>(ctx);
    switch (id) {
    case rsp::event::power_overload_change:
        if (params && params->powerOverloadParams.powerOverloadChangeType == rsp::overload::detected) {
            s->overloaded_.store(true);
            s->overload_pulse_.store(true);
            s->overload_events_.fetch_add(1);
        } else {
            s->overloaded_.store(false);
        }
        // Every overload message, detected and corrected alike, is
        // acknowledged (the specification's example does so); the session
        // does it, since this thread must not wait on the API.
        s->pending_acks_.fetch_add(1);
        s->notify(s->notify_fd_);
        break;
    case rsp::event::gain_change:
        if (params)
            s->reported_gr_.store(static_cast<int>(params->gainParams.gRdB));
        s->gain_events_.fetch_add(1);
        break;
    case rsp::event::device_removed:
        s->removed_.store(true);
        s->notify(s->notify_fd_);
        break;
    case rsp::event::device_failure:
        s->failed_.store(true);
        s->notify(s->notify_fd_);
        break;
    default:
        // RspDuoModeChange concerns master and slave use, which this module
        // does not do.
        break;
    }
}

void Stream::writer_main() {
    // Large enough to keep the syscall rate low, small enough that a stop is
    // noticed between writes.
    constexpr size_t max_write = 256 * 1024;
    WriterEnd end = WriterEnd::stopped;
    while (!writer_stop_.load()) {
        const uint8_t* data = nullptr;
        size_t n = ring_.peek(&data);
        if (n == 0) {
            // The timeout only guards against a lost wakeup; the producer
            // signals data_event_ after every push.
            struct pollfd pfd = {data_event_, POLLIN, 0};
            ::poll(&pfd, 1, 50);
            uint64_t count;
            if (data_event_ >= 0) {
                const ssize_t ignored = ::read(data_event_, &count, sizeof count);
                (void)ignored;
            }
            continue;
        }
        n = std::min(n, max_write);
        const ssize_t w = ::write(samples_fd_, data, n);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // fd 1 was handed over non-blocking and the pipe is full.
                struct pollfd pfd = {samples_fd_, POLLOUT, 0};
                ::poll(&pfd, 1, 50);
                continue;
            }
            if (errno == EPIPE) {
                end = WriterEnd::host_gone;
            } else {
                end = WriterEnd::failed;
                writer_errno_.store(errno);
            }
            break;
        }
        ring_.consume(static_cast<size_t>(w));
        bytes_written_.fetch_add(static_cast<uint64_t>(w), std::memory_order_relaxed);
    }
    writer_end_.store(end);
    notify(notify_fd_);
}

void Stream::request_stop() {
    accepting_.store(false);
    writer_stop_.store(true);
    notify(data_event_);
}

bool Stream::wait(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const bool done = !writer_.joinable() || writer_end_.load() != WriterEnd::running;
        if (done) {
            if (writer_.joinable())
                writer_.join();
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        pthread_kill(writer_.native_handle(), SIGUSR1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

}  // namespace fern
