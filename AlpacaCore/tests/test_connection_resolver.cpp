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

// util::connect_resolved() policy (issue #659): fixed endpoints connect once,
// a resolver runs on the first connect, the resolved endpoint is retried
// before a second scan, and a dead endpoint falls through to a fresh scan.
// Exceptions from the resolver or from the connect that follows it
// propagate; only the retry of a stale endpoint is swallowed.

#include <alpacacore/util/connection_resolver.h>
#include <alpacacore/util/error_handling.h>

#include <stdexcept>
#include <string>
#include <vector>

#include "catch2_compat.h"

namespace {

struct Endpoint {
    std::string where;
};

}  // namespace

TEST_CASE("connect_resolved - no resolver connects the fixed endpoint once", "[util][unit]") {
    Endpoint info{"/dev/fixed"};
    bool resolved = false;
    std::vector<std::string> tried;
    alpacacore::util::connect_resolved(info, resolved, alpacacore::util::ConnectionResolver<Endpoint>{},
                                       [&tried](const Endpoint& e) { tried.push_back(e.where); });
    CHECK(tried == std::vector<std::string>{"/dev/fixed"});
    CHECK_FALSE(resolved);
    CHECK(info.where == "/dev/fixed");
}

TEST_CASE("connect_resolved - no resolver lets a failed connect propagate", "[util][unit]") {
    Endpoint info{"/dev/fixed"};
    bool resolved = false;
    CHECK_THROWS_AS(
        alpacacore::util::connect_resolved(info, resolved, alpacacore::util::ConnectionResolver<Endpoint>{},
                                           [](const Endpoint&) { throw alpacacore::AlpacaException("down"); }),
        alpacacore::AlpacaException);
    CHECK_FALSE(resolved);
}

TEST_CASE("connect_resolved - first connect scans, later connects reuse the endpoint", "[util][unit]") {
    Endpoint info;
    bool resolved = false;
    int scans = 0;
    alpacacore::util::ConnectionResolver<Endpoint> resolver = [&scans] {
        ++scans;
        return Endpoint{"/dev/found" + std::to_string(scans)};
    };
    std::vector<std::string> tried;
    auto connect = [&tried](const Endpoint& e) { tried.push_back(e.where); };

    alpacacore::util::connect_resolved(info, resolved, resolver, connect);
    CHECK(scans == 1);
    CHECK(resolved);
    CHECK(info.where == "/dev/found1");

    alpacacore::util::connect_resolved(info, resolved, resolver, connect);
    CHECK(scans == 1);
    CHECK(info.where == "/dev/found1");
    CHECK(tried == std::vector<std::string>{"/dev/found1", "/dev/found1"});
}

TEST_CASE("connect_resolved - a dead resolved endpoint triggers one fresh scan", "[util][unit]") {
    Endpoint info{"/dev/found1"};
    bool resolved = true;
    int scans = 0;
    alpacacore::util::ConnectionResolver<Endpoint> resolver = [&scans] {
        ++scans;
        return Endpoint{"/dev/found2"};
    };
    std::vector<std::string> tried;
    alpacacore::util::connect_resolved(info, resolved, resolver, [&tried](const Endpoint& e) {
        tried.push_back(e.where);
        if (e.where == "/dev/found1") throw std::runtime_error("gone");
    });
    CHECK(scans == 1);
    CHECK(tried == std::vector<std::string>{"/dev/found1", "/dev/found2"});
    CHECK(info.where == "/dev/found2");
    CHECK(resolved);
}

TEST_CASE("connect_resolved - a failed scan propagates and leaves the endpoint alone", "[util][unit]") {
    Endpoint info{"/dev/found1"};
    bool resolved = true;
    int connects = 0;
    alpacacore::util::ConnectionResolver<Endpoint> resolver = []() -> Endpoint {
        throw alpacacore::AlpacaException("No mount found on the local network");
    };
    try {
        alpacacore::util::connect_resolved(info, resolved, resolver, [&connects](const Endpoint&) {
            ++connects;
            throw std::runtime_error("gone");
        });
        FAIL("the scan's exception must propagate");
    } catch (const alpacacore::AlpacaException& ex) {
        CHECK(std::string(ex.what()).find("No mount found") != std::string::npos);
    }
    CHECK(connects == 1);  // the stale endpoint was retried before the scan
    CHECK(info.where == "/dev/found1");
    CHECK(resolved);
}

TEST_CASE("connect_resolved - a failed connect after a fresh scan propagates with the new endpoint kept",
          "[util][unit]") {
    Endpoint info;
    bool resolved = false;
    alpacacore::util::ConnectionResolver<Endpoint> resolver = [] { return Endpoint{"/dev/found1"}; };
    CHECK_THROWS_AS(
        alpacacore::util::connect_resolved(info, resolved, resolver,
                                           [](const Endpoint&) { throw alpacacore::AlpacaException("refused"); }),
        alpacacore::AlpacaException);
    CHECK(resolved);
    CHECK(info.where == "/dev/found1");
}
