// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <barrier>
#include <iostream>
#include <stdexcept>
#include <thread>

#include "video_core/screenshot_request.h"

static void Require(bool value) {
    if (!value) throw std::runtime_error("screenshot request assertion failed");
}

int main() {
    VideoCore::ScreenshotRequest request;
    int payload = 0;
    Require(!request.Pending());
    Require(request.TryRequest([&] { payload = 17; }));
    Require(request.Pending() && payload == 17);
    Require(!request.TryRequest([&] { payload = 99; }));
    Require(payload == 17); // A busy producer cannot overwrite a renderer-owned buffer.
    request.Complete();
    Require(request.TryRequest([&] { payload = 23; }));
    Require(payload == 23);
    request.Complete();

    bool threw = false;
    try {
        request.TryRequest([] { throw std::runtime_error("publication failed"); });
    } catch (const std::runtime_error&) {
        threw = true;
    }
    Require(threw && !request.Pending());
    Require(request.TryRequest([&] { payload = 31; }));
    request.Complete();

    // Simultaneous Qt/GIF and service capture producers: exactly one owns each frame.
    for (int round = 0; round < 256; ++round) {
        std::barrier start{3};
        std::atomic<int> accepted{}, published{};
        auto producer = [&](int value) {
            start.arrive_and_wait();
            if (request.TryRequest([&] { payload = value; ++published; })) ++accepted;
        };
        std::thread a{producer, 41}, b{producer, 43};
        start.arrive_and_wait();
        a.join();
        b.join();
        Require(accepted == 1 && published == 1 && request.Pending());
        Require(payload == 41 || payload == 43);
        request.Complete();
    }

    // Real producer/consumer handoff repeatedly publishes data before the pending flag.
    std::thread consumer{[&] {
        for (int expected = 1; expected <= 10000; ++expected) {
            while (!request.Pending()) std::this_thread::yield();
            Require(payload == expected);
            request.Complete();
        }
    }};
    for (int value = 1; value <= 10000; ++value) {
        while (!request.TryRequest([&] { payload = value; })) std::this_thread::yield();
    }
    consumer.join();
    Require(!request.Pending());
    std::cout << "PASS: busy/retry, publication failure, 256 concurrent producers, 10000 handoffs\n";
}
