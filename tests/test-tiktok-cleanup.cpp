// Claim-once ledger tests: every started TikTok LIVE must eventually be
// ended exactly once, no matter how many stop paths race (normal/forced
// stop, cancel, shutdown, target deletion).
#include <atomic>
#include <iostream>
#include <thread>
#include <vector>

#include "tiktok/cleanup.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestClaimOnce()
{
    TikTokCleanupLedger ledger;
    CHECK(ledger.claim("live-1"));
    CHECK(!ledger.claim("live-1"));
    CHECK(ledger.contains("live-1"));
    CHECK(ledger.size() == 1);
    CHECK(ledger.claim("live-2"));
    CHECK(ledger.contains("live-2"));
    CHECK(ledger.size() == 2);
    CHECK(!ledger.contains("live-3"));
    return 0;
}

static int TestEmptyNeverClaimed()
{
    TikTokCleanupLedger ledger;
    CHECK(!ledger.claim(""));
    CHECK(!ledger.contains(""));
    CHECK(ledger.size() == 0);
    return 0;
}

static int TestConcurrentClaimHasOneWinner()
{
    TikTokCleanupLedger ledger;
    std::atomic<int> wins{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&]() {
            if (ledger.claim("live-1"))
                wins.fetch_add(1);
        });
    }
    for (auto &t : threads)
        t.join();
    CHECK(wins.load() == 1);
    CHECK(ledger.size() == 1);
    return 0;
}

int main()
{
    if (TestClaimOnce())
        return 1;
    if (TestEmptyNeverClaimed())
        return 1;
    if (TestConcurrentClaimHasOneWinner())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
