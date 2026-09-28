// Fern-SDRPlay, an SDRplay RSP input module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later WITH AdditionRef-Fern-SDRPlay-API-exception
#include <atomic>
#include <cstring>
#include <thread>
#include <vector>
#include <unistd.h>
#include <chrono>
#include <stdexcept>

#include "fake_sdrplay.h"
#include "stream.h"
#include "test.h"

TEST(ring_rounds_capacity_to_a_power_of_two) {
    CHECK_EQ(fern::RingBuffer(1000).capacity(), size_t(1024));
    CHECK_EQ(fern::RingBuffer(4096).capacity(), size_t(4096));
    CHECK_EQ(fern::RingBuffer(0).capacity(), size_t(2));
}

TEST(ring_wraps_around) {
    fern::RingBuffer ring(16);
    std::vector<uint8_t> data(12);
    for (size_t i = 0; i < data.size(); ++i)
        data[i] = static_cast<uint8_t>(i + 1);
    REQUIRE(ring.push(data.data(), 12));
    CHECK_EQ(ring.size(), size_t(12));
    const uint8_t* p = nullptr;
    REQUIRE(ring.peek(&p) == 12);
    CHECK_EQ(p[0], uint8_t(1));
    ring.consume(10);
    CHECK_EQ(ring.size(), size_t(2));

    // 12 more bytes: 4 fit before the end, 8 wrap to the start.
    REQUIRE(ring.push(data.data(), 12));
    CHECK_EQ(ring.size(), size_t(14));
    std::vector<uint8_t> out;
    while (ring.size() > 0) {
        const size_t n = ring.peek(&p);
        REQUIRE(n > 0);
        out.insert(out.end(), p, p + n);
        ring.consume(n);
    }
    REQUIRE(out.size() == 14);
    CHECK_EQ(out[0], uint8_t(11));
    CHECK_EQ(out[1], uint8_t(12));
    for (size_t i = 0; i < 12; ++i)
        CHECK_EQ(out[2 + i], uint8_t(i + 1));
    CHECK_EQ(ring.peek(&p), size_t(0));
}

TEST(ring_drops_whole_blocks_when_full) {
    fern::RingBuffer ring(16);
    const uint8_t block[6] = {1, 2, 3, 4, 5, 6};
    CHECK(ring.push(block, 6));
    CHECK(ring.push(block, 6));
    CHECK(!ring.push(block, 6));  // 12 used, 4 free: nothing is stored
    CHECK_EQ(ring.size(), size_t(12));
    CHECK(ring.push(block, 4));
    CHECK_EQ(ring.size(), size_t(16));
    CHECK(!ring.push(block, 1));
    ring.consume(6);
    CHECK(ring.push(block, 6));
    CHECK(!ring.push(block, 17));
}

// One producer pushes numbered blocks as fast as it can while one consumer
// reads slowly. Every byte read must belong to a block that was pushed
// whole, in order, and pushed plus dropped must account for every block.
TEST(ring_keeps_order_under_concurrency) {
    fern::RingBuffer ring(1 << 12);
    constexpr uint32_t blocks = 20000;
    constexpr size_t block_size = 256;
    std::atomic<bool> done{false};
    uint32_t pushed = 0;
    uint32_t dropped = 0;

    std::thread producer([&] {
        std::vector<uint8_t> block(block_size);
        for (uint32_t n = 0; n < blocks; ++n) {
            std::memcpy(block.data(), &n, sizeof n);
            for (size_t i = sizeof n; i < block_size; ++i)
                block[i] = static_cast<uint8_t>(n + i);
            if (ring.push(block.data(), block.size()))
                ++pushed;
            else
                ++dropped;
        }
        done.store(true);
    });

    std::vector<uint8_t> partial;
    uint32_t received = 0;
    int64_t last = -1;
    bool in_order = true;
    bool intact = true;
    for (;;) {
        const uint8_t* p = nullptr;
        const size_t n = ring.peek(&p);
        if (n == 0) {
            if (done.load() && ring.size() == 0)
                break;
            std::this_thread::yield();
            continue;
        }
        const size_t take = n < 100 ? n : 100;
        partial.insert(partial.end(), p, p + take);
        ring.consume(take);
        while (partial.size() >= block_size) {
            uint32_t id;
            std::memcpy(&id, partial.data(), sizeof id);
            for (size_t i = sizeof id; i < block_size; ++i)
                if (partial[i] != static_cast<uint8_t>(id + i))
                    intact = false;
            if (static_cast<int64_t>(id) <= last)
                in_order = false;
            last = id;
            ++received;
            partial.erase(partial.begin(), partial.begin() + block_size);
        }
    }
    producer.join();
    CHECK(intact);
    CHECK(in_order);
    CHECK(partial.empty());
    CHECK_EQ(received, pushed);
    CHECK_EQ(pushed + dropped, blocks);
    CHECK(dropped > 0);  // the consumer is slower on purpose
}

namespace {

int pipe_write_end(int* fds) {
    if (::pipe(fds) != 0)
        throw std::runtime_error("pipe failed");
    return fds[1];
}

// Feeds the stream callbacks of `count` samples of the fake's pattern,
// numbered by number_step per callback, and reads what the writer puts on a pipe.
struct Feeder {
    int fds[2] = {-1, -1};
    fern::Stream stream;
    uint64_t sample = 0;
    uint32_t number = 0;

    explicit Feeder(unsigned decimation = 1) : stream(pipe_write_end(fds), -1, 1 << 20, decimation) {}
    ~Feeder() {
        stream.request_stop();
        stream.wait(std::chrono::steady_clock::now() + std::chrono::seconds(2));
        ::close(fds[0]);
        ::close(fds[1]);
    }
    void feed(unsigned count, uint32_t number_step, bool reset = false, int16_t force = 0) {
        std::vector<short> xi(count), xq(count);
        for (unsigned k = 0; k < count; ++k) {
            xi[k] = force ? force : fake::pattern_i(sample + k);
            xq[k] = force ? static_cast<short>(-force) : fake::pattern_q(sample + k);
        }
        fern::rsp::StreamCbParamsT p{};
        p.firstSampleNum = number;
        p.numSamples = count;
        fern::Stream::on_stream(xi.data(), xq.data(), &p, count, reset ? 1 : 0, &stream);
        sample += count;
        number += number_step;
    }
    std::vector<int16_t> read_pairs(size_t samples) {
        std::vector<int16_t> out(samples * 2);
        size_t got = 0;
        const size_t want = out.size() * 2;
        while (got < want) {
            const ssize_t n = ::read(fds[0], reinterpret_cast<char*>(out.data()) + got, want - got);
            if (n <= 0)
                break;
            got += static_cast<size_t>(n);
        }
        out.resize(got / 2);
        return out;
    }
};

}  // namespace

TEST(stream_discards_samples_until_started_then_interleaves_i_and_q_in_order) {
    Feeder f;
    f.feed(1000, 1000);
    CHECK_EQ(f.stream.samples_received(), uint64_t(0));
    REQUIRE(f.stream.start());
    const uint64_t first = f.sample;
    for (int i = 0; i < 20; ++i)
        f.feed(3000, 3000);  // more than one scratch chunk of 8192 over the run
    f.feed(9000, 9000);
    const std::vector<int16_t> pairs = f.read_pairs(69000);
    REQUIRE(pairs.size() == 138000);
    bool exact = true;
    for (size_t k = 0; k < 69000; ++k)
        exact = exact && pairs[2 * k] == fake::pattern_i(first + k) && pairs[2 * k + 1] == fake::pattern_q(first + k);
    CHECK(exact);
    CHECK_EQ(f.stream.samples_received(), uint64_t(69000));
    CHECK_EQ(f.stream.samples_dropped(), uint64_t(0));
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(0));
    CHECK_EQ(f.stream.take_peak(), 8192u);
    CHECK_EQ(f.stream.take_peak(), 0u);
}

TEST(stream_counts_gaps_in_the_sample_numbers_as_dropped) {
    Feeder f;
    REQUIRE(f.stream.start());
    f.feed(500, 500);
    f.feed(500, 500 + 300);  // the next callback starts 300 samples later
    f.feed(500, 500);
    f.feed(500, 500);
    CHECK_EQ(f.stream.samples_skipped(), uint64_t(300));
    // A reset restarts the numbering without counting anything.
    f.number = 7;
    f.feed(500, 500, true);
    f.feed(500, 500);
    CHECK_EQ(f.stream.samples_skipped(), uint64_t(300));
    CHECK_EQ(f.stream.resets(), uint64_t(1));
    // Numbers that go backwards are a discontinuity, not a drop.
    f.number -= 5000;
    f.feed(500, 500);
    CHECK_EQ(f.stream.samples_skipped(), uint64_t(300));
    CHECK_EQ(f.stream.discontinuities(), uint64_t(1));
    CHECK_EQ(f.stream.samples_dropped(), uint64_t(300));
}

TEST(stream_numbers_wrap_at_32_bits) {
    Feeder f;
    REQUIRE(f.stream.start());
    f.number = 0xFFFFFE00u;
    f.feed(256, 256);
    f.feed(256, 256);
    f.feed(256, 256 + 64);  // across the wrap
    f.feed(256, 256);
    CHECK_EQ(f.stream.samples_skipped(), uint64_t(64));
    CHECK_EQ(f.stream.discontinuities(), uint64_t(0));
}

TEST(stream_learns_numbers_counted_before_decimation) {
    Feeder f(4);
    REQUIRE(f.stream.start());
    f.feed(500, 2000);
    f.feed(500, 2000);
    f.feed(500, 2000 + 400);  // 100 output samples missing
    f.feed(500, 2000);
    CHECK_EQ(f.stream.samples_skipped(), uint64_t(100));
    CHECK_EQ(f.stream.discontinuities(), uint64_t(0));

    Feeder g(4);  // the same decimation, numbers counted after it
    REQUIRE(g.stream.start());
    g.feed(500, 500);
    g.feed(500, 500);
    g.feed(500, 500);
    CHECK_EQ(g.stream.samples_skipped(), uint64_t(0));
}

TEST(stream_counts_clipping_and_every_sample_during_an_overload) {
    Feeder f;
    REQUIRE(f.stream.start());
    f.feed(100, 100, false, 32767);
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(100));
    CHECK_EQ(f.stream.take_peak(), 32767u);
    f.feed(100, 100, false, fern::clip_level - 1);
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(100));

    fern::rsp::EventParamsT e{};
    e.powerOverloadParams.powerOverloadChangeType = fern::rsp::overload::detected;
    fern::Stream::on_event(fern::rsp::event::power_overload_change, fern::rsp::tuner::a, &e, &f.stream);
    CHECK(f.stream.overloaded());
    f.feed(200, 200);
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(300));
    e.powerOverloadParams.powerOverloadChangeType = fern::rsp::overload::corrected;
    fern::Stream::on_event(fern::rsp::event::power_overload_change, fern::rsp::tuner::a, &e, &f.stream);
    f.feed(200, 200);
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(300));
    // An overload that came and went between callbacks still marks the next.
    e.powerOverloadParams.powerOverloadChangeType = fern::rsp::overload::detected;
    fern::Stream::on_event(fern::rsp::event::power_overload_change, fern::rsp::tuner::a, &e, &f.stream);
    e.powerOverloadParams.powerOverloadChangeType = fern::rsp::overload::corrected;
    fern::Stream::on_event(fern::rsp::event::power_overload_change, fern::rsp::tuner::a, &e, &f.stream);
    f.feed(50, 50);
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(350));
    f.feed(50, 50);
    CHECK_EQ(f.stream.samples_clipped(), uint64_t(350));
    // Each message, detected or corrected, waits for one acknowledgement.
    CHECK_EQ(f.stream.overload_events(), uint64_t(2));
    int acks = 0;
    while (f.stream.take_pending_ack())
        ++acks;
    CHECK_EQ(acks, 4);
}

TEST(stream_notes_removal_failure_and_gain_events) {
    Feeder f;
    fern::rsp::EventParamsT e{};
    e.gainParams.gRdB = 44;
    fern::Stream::on_event(fern::rsp::event::gain_change, fern::rsp::tuner::a, &e, &f.stream);
    CHECK_EQ(f.stream.reported_if_reduction(), 44);
    CHECK(!f.stream.device_removed());
    fern::Stream::on_event(fern::rsp::event::device_removed, fern::rsp::tuner::a, &e, &f.stream);
    CHECK(f.stream.device_removed());
    fern::Stream::on_event(fern::rsp::event::device_failure, fern::rsp::tuner::a, &e, &f.stream);
    CHECK(f.stream.device_failed());
}

TEST(stream_drops_and_counts_what_the_ring_cannot_hold) {
    int fds[2];
    REQUIRE(::pipe(fds) == 0);
    {
        fern::Stream stream(fds[1], -1, 64 * 1024, 1);
        REQUIRE(stream.start());
        // Nobody reads the pipe: once it and the ring are full, callbacks drop.
        std::vector<short> xi(4096), xq(4096);
        fern::rsp::StreamCbParamsT p{};
        for (int i = 0; i < 200; ++i) {
            p.firstSampleNum = static_cast<unsigned>(i) * 4096;
            fern::Stream::on_stream(xi.data(), xq.data(), &p, 4096, 0, &stream);
        }
        CHECK(stream.samples_dropped_here() > 0);
        CHECK_EQ(stream.samples_skipped(), uint64_t(0));
        CHECK_EQ(stream.samples_received(), uint64_t(200 * 4096));
        stream.request_stop();
        CHECK(stream.wait(std::chrono::steady_clock::now() + std::chrono::seconds(2)));
    }
    ::close(fds[0]);
    ::close(fds[1]);
}
