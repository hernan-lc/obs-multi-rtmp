#include "tiktok/cleanup.h"

bool TikTokCleanupLedger::claim(const std::string &streamId)
{
    if (streamId.empty())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return claimed_.insert(streamId).second;
}

bool TikTokCleanupLedger::contains(const std::string &streamId) const
{
    if (streamId.empty())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return claimed_.find(streamId) != claimed_.end();
}

size_t TikTokCleanupLedger::size() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return claimed_.size();
}

TikTokCleanupLedger &GlobalCleanupLedger()
{
    static TikTokCleanupLedger ledger;
    return ledger;
}
