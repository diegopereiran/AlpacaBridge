// AlpacaCore
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaCore.
//
// AlpacaCore is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Self-test for StressCallGuard (issue #322) — the per-call guard every
// [stress] registration is meant to share, replacing the per-file `call()`
// copies whose catch type has flip-flopped across review rounds. These
// assertions ARE the harness-level decision: swallow an expected
// AlpacaError code silently, count everything else, and never intercept a
// non-std::exception throw.

#include <stdexcept>
#include <thread>
#include <vector>

#include "catch2_compat.h"
#include "concurrency_stress.h"

using alpacacore::AlpacaException;
using alpacacore::test::StressCallGuard;
namespace AlpacaError = alpacacore::AlpacaError;

TEST_CASE("StressCallGuard - a call that does not throw is not counted", "[stress][unit]") {
    StressCallGuard guard;
    guard([] {});
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.report().empty());
}

TEST_CASE("StressCallGuard - the default expected code (NotConnected) is swallowed silently", "[stress][unit]") {
    StressCallGuard guard;
    guard([] { throw AlpacaException("racing a disconnect", AlpacaError::NotConnected); });
    CHECK(guard.unexpected_count() == 0);
    CHECK(guard.report().empty());
}

TEST_CASE("StressCallGuard - an AlpacaException with an unexpected code is counted", "[stress][unit]") {
    StressCallGuard guard;
    guard([] { throw AlpacaException("bug", AlpacaError::DriverException); });
    CHECK(guard.unexpected_count() == 1);
    CHECK(guard.report().find("AlpacaException") != std::string::npos);
    CHECK(guard.report().find("bug") != std::string::npos);
    CHECK(guard.report().find(std::to_string(AlpacaError::DriverException)) != std::string::npos);
}

TEST_CASE("StressCallGuard - any other std::exception is counted, not silently dropped", "[stress][unit]") {
    // The exact failure mode #322 exists to fix: a non-Alpaca throw escaping
    // teardown (e.g. std::system_error from serial/curl) must not be
    // indistinguishable from an expected NotConnected.
    StressCallGuard guard;
    guard([] { throw std::out_of_range("index out of range"); });
    CHECK(guard.unexpected_count() == 1);
    CHECK(guard.report().find("index out of range") != std::string::npos);
}

TEST_CASE("StressCallGuard - a caller-specified expected code is swallowed", "[stress][unit]") {
    // e.g. WeeWX's PropertyNotImplemented getters that answer without a
    // connection — a widened set is an explicit per-file opt-in, not a
    // silent default.
    StressCallGuard guard({AlpacaError::NotConnected, AlpacaError::PropertyNotImplemented});
    guard([] { throw AlpacaException("no such property", AlpacaError::PropertyNotImplemented); });
    CHECK(guard.unexpected_count() == 0);
}

TEST_CASE("StressCallGuard - a call after an unexpected throw still keeps isolation", "[stress][unit]") {
    // The whole point: the FIRST throw must not skip the calls after it, the
    // way run_lifecycle_stress's outer catch would.
    StressCallGuard guard;
    int reached = 0;
    guard([] { throw std::runtime_error("first call fails"); });
    guard([&] { ++reached; });
    guard([] { throw AlpacaException("third call fails", AlpacaError::DriverException); });
    CHECK(reached == 1);
    CHECK(guard.unexpected_count() == 2);
}

TEST_CASE("StressCallGuard - a non-std::exception throw is not intercepted", "[stress][unit]") {
    // Must reach the caller (and from there, run_lifecycle_stress's outer
    // catch, or std::terminate on a raw thread) exactly as it does today.
    StressCallGuard guard;
    CHECK_THROWS_AS(guard([] { throw 42; }), int);
    CHECK(guard.unexpected_count() == 0);
}

TEST_CASE("StressCallGuard - report() caps the number of stored samples", "[stress][unit]") {
    StressCallGuard guard;
    for (int i = 0; i < 20; ++i) {
        guard([i] { throw std::runtime_error("failure " + std::to_string(i)); });
    }
    CHECK(guard.unexpected_count() == 20);
    const std::string report = guard.report();
    CHECK(report.find("failure 0") != std::string::npos);
    CHECK(report.find("more") != std::string::npos);
}

TEST_CASE("StressCallGuard - concurrent hits from many threads count exactly", "[stress][unit]") {
    // op_threads hits one guard concurrently in real registrations.
    StressCallGuard guard;
    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&guard] {
            for (int i = 0; i < kPerThread; ++i) {
                guard([i] {
                    if (i % 2 == 0) {
                        throw AlpacaException("expected", AlpacaError::NotConnected);
                    }
                    throw AlpacaException("unexpected", AlpacaError::DriverException);
                });
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    CHECK(guard.unexpected_count() == kThreads * kPerThread / 2);
}
