// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
//
// Moves samples from the API's stream callback to fd 1. The callback, on a
// thread of the API's, interleaves the separate I and Q arrays into s16
// pairs, notes the peak and clipping, follows the sample numbers for gaps
// and copies into a ring, and never blocks: a writer thread empties the
// ring into fd 1 with blocking writes. When the ring is full the newest
// samples are dropped and counted.
//
// The event callback, on another API thread, only records: overloads to be
// acknowledged, the device's removal or failure. The session acts on them,
// so that every call into the API comes from one thread.
#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "rsp_api.h"

namespace fern {

// Single producer, single consumer. The capacity is a power of two, so that
// the free-running positions can wrap around.
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity);

    size_t capacity() const { return mask_ + 1; }
    size_t size() const;

    // Producer: stores all of data, or nothing when it does not fit.
    bool push(const uint8_t* data, size_t len);

    // Consumer: the longest contiguous readable span, then release part of it.
    size_t peek(const uint8_t** data) const;
    void consume(size_t len);

private:
    std::unique_ptr<uint8_t[]> buf_;
    size_t mask_;
    std::atomic<size_t> head_{0};  // written by the producer
    std::atomic<size_t> tail_{0};  // written by the consumer
};

// A sample of I or Q at or beyond this magnitude counts as clipped. The
// API delivers 16-bit values whatever the converter's resolution (14 bits
// up to 6.048 MHz, 8 at the top rates), and its corrections move a sample
// off the exact limit, so the threshold allows for that: 32512 is the top of
// the 8-bit range, 0.07 dB below full scale.
constexpr int clip_level = 32512;

class Stream {
public:
    enum class WriterEnd { running, stopped, host_gone, failed };

    // notify_fd, an eventfd, is signalled when the writer ends and when an
    // event needs the session. decimation is the API's, for the sample
    // numbers (see track_numbers).
    Stream(int samples_fd, int notify_fd, size_t ring_bytes, unsigned decimation);
    ~Stream();
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;

    // The callbacks for sdrplay_api_Init, with this stream as the context.
    static void on_stream(short* xi, short* xq, rsp::StreamCbParamsT* params, unsigned int count,
                          unsigned int reset, void* ctx);
    static void on_event(rsp::EventId id, rsp::TunerSelect tuner, rsp::EventParamsT* params, void* ctx);

    // From ready on: samples go to the ring and the writer runs. Samples
    // that arrive before are discarded, since nothing may reach fd 1 before
    // ready. False when the writer thread could not be created.
    bool start();
    // Stops taking samples and stops the writer; does not wait. The API may
    // still call on_stream until sdrplay_api_Uninit returns.
    void request_stop();
    // Keeps nudging the writer until it has ended or the deadline passes.
    bool wait(std::chrono::steady_clock::time_point deadline);

    WriterEnd writer_end() const { return writer_end_.load(); }
    int writer_errno() const { return writer_errno_.load(); }

    // Samples are I/Q pairs, four bytes each on fd 1.
    uint64_t samples_received() const { return received_.load(); }
    uint64_t samples_delivered() const { return bytes_written_.load() / 4; }
    uint64_t samples_dropped_here() const { return bytes_dropped_.load() / 4; }
    // Samples the API skipped, from the gaps between the sample numbers.
    uint64_t samples_skipped() const { return skipped_.load(); }
    uint64_t samples_dropped() const { return samples_dropped_here() + samples_skipped(); }
    // Sample numbers that went backwards or restarted without a reset.
    uint64_t discontinuities() const { return discontinuities_.load(); }
    uint64_t resets() const { return resets_.load(); }
    // Samples with I or Q at clip_level or beyond, and all samples that
    // arrived while the API reported an overload, since start().
    uint64_t samples_clipped() const { return clipped_.load(); }
    // The largest magnitude of I or Q since the last call, 0 to 32768.
    unsigned take_peak() { return peak_.exchange(0); }
    // steady_clock time of the last stream callback, or of start() before.
    std::chrono::steady_clock::time_point last_data() const;

    uint64_t overload_events() const { return overload_events_.load(); }
    bool overloaded() const { return overloaded_.load(); }
    // Overload events not yet acknowledged; each call takes one.
    bool take_pending_ack();
    bool device_removed() const { return removed_.load(); }
    bool device_failed() const { return failed_.load(); }
    uint64_t gain_events() const { return gain_events_.load(); }
    // The last GainChange event's IF gain reduction, or -1 before one.
    int reported_if_reduction() const { return reported_gr_.load(); }

private:
    void handle_samples(const short* xi, const short* xq, const rsp::StreamCbParamsT* params, unsigned count,
                        bool reset);
    void track_numbers(uint32_t first, unsigned count, bool reset);
    void account_step(uint32_t step, unsigned count);
    void writer_main();
    void notify(int fd);

    const int samples_fd_;
    const int notify_fd_;
    const unsigned decimation_;
    RingBuffer ring_;
    int data_event_ = -1;
    // The interleaved pairs of one chunk, touched by the stream callback only.
    static constexpr size_t chunk_samples = 8192;
    std::unique_ptr<int16_t[]> scratch_;

    std::thread writer_;
    std::atomic<bool> accepting_{false};
    std::atomic<bool> writer_stop_{false};
    std::atomic<WriterEnd> writer_end_{WriterEnd::running};
    std::atomic<int> writer_errno_{0};

    // Sample numbering, touched by the stream callback only.
    bool have_previous_ = false;
    uint32_t previous_first_ = 0;
    unsigned previous_count_ = 0;
    unsigned stride_ = 0;     // numbers per delivered sample; 0 until learnt
    unsigned candidate_ = 0;  // the step the last pairs of callbacks suggest
    unsigned agreed_ = 0;     // how many pairs in a row suggested it
    // The pairs seen while learning, accounted for once the step is known.
    static constexpr size_t max_pending = 8;
    uint32_t pending_step_[max_pending] = {};
    unsigned pending_count_[max_pending] = {};
    size_t pending_ = 0;

    std::atomic<uint64_t> received_{0};
    std::atomic<uint64_t> skipped_{0};
    std::atomic<uint64_t> discontinuities_{0};
    std::atomic<uint64_t> resets_{0};
    std::atomic<uint64_t> clipped_{0};
    std::atomic<unsigned> peak_{0};
    std::atomic<uint64_t> bytes_dropped_{0};
    std::atomic<uint64_t> bytes_written_{0};
    std::atomic<int64_t> last_data_ns_{0};

    std::atomic<bool> overloaded_{false};
    std::atomic<bool> overload_pulse_{false};
    std::atomic<uint64_t> overload_events_{0};
    std::atomic<uint64_t> pending_acks_{0};
    std::atomic<bool> removed_{false};
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> gain_events_{0};
    std::atomic<int> reported_gr_{-1};
};

}  // namespace fern
