#pragma once

// Claim-once ledger for TikTok LIVE cleanup. Every successful
// POST /stream/start must eventually produce exactly one
// POST /stream/<id>/end when OBS does not keep streaming, no matter
// how many stop paths race (normal/forced stop, cancel, shutdown,
// target deletion). Claiming is the single serialization point, so
// double cleanup of the same id is impossible.
//
// Qt-free and OBS-free for unit testing.

#include <mutex>
#include <string>
#include <unordered_set>

class TikTokCleanupLedger {
public:
    // Claims streamId for cleanup. Returns true exactly once per id;
    // empty ids are never claimed.
    bool claim(const std::string &streamId);
    bool contains(const std::string &streamId) const;
    size_t size() const;

private:
    mutable std::mutex mutex_;
    std::unordered_set<std::string> claimed_;
};

// Plugin-global ledger.
TikTokCleanupLedger &GlobalCleanupLedger();
