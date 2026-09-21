#include "tiktok/streamlabs-client.h"

StreamlabsTikTokClient::StreamlabsTikTokClient(std::string token, uint64_t authEpoch)
    : token_(std::move(token))
    , authEpoch_(authEpoch)
{
}

TikTokAccountResult StreamlabsTikTokClient::GetAccountInfo()
{
    return StreamlabsTikTokAPI::GetAccountInfo(token_);
}

TikTokCategoryListResult StreamlabsTikTokClient::SearchCategories(const std::string &query)
{
    return StreamlabsTikTokAPI::ListCategories(token_, query);
}

TikTokCategoryResult StreamlabsTikTokClient::ResolveCategory(const std::string &category)
{
    return StreamlabsTikTokAPI::ResolveCategory(token_, category);
}

TikTokStartResult StreamlabsTikTokClient::StartStream(const std::string &title,
                                                     const std::string &categoryId,
                                                     TikTokAudience audience)
{
    return StreamlabsTikTokAPI::StartStream(token_, title, categoryId, audience);
}

bool StreamlabsTikTokClient::EndStream(const std::string &streamId, std::string *error)
{
    return StreamlabsTikTokAPI::EndStream(token_, streamId, error);
}
