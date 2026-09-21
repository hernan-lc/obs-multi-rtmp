// Focused tests for the Streamlabs TikTok client.
// Pure response parsing only: no test here touches the network.
#include <cassert>
#include <iostream>

#include "streamlabs-tiktok.h"

static int g_checks = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        ++g_checks;                                                                                \
        if (!(cond)) {                                                                             \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";                                    \
            return 1;                                                                              \
        }                                                                                          \
    } while (0)

static int TestCategoryExactMatch()
{
    const std::string body = R"({"categories":[
        {"full_name":"Just Chatting","game_mask_id":"111"},
        {"full_name":"Minecraft","game_mask_id":"222"},
        {"full_name":"Other","game_mask_id":""}
    ]})";
    auto exact = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "Minecraft");
    CHECK(exact.success);
    CHECK(exact.categoryId == "222");
    CHECK(exact.matchedExact);

    auto lower = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "minecraft");
    CHECK(lower.success);
    CHECK(lower.categoryId == "222");
    CHECK(lower.matchedExact);

    auto upper = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "MINECRAFT");
    CHECK(upper.success);
    CHECK(upper.categoryId == "222");
    return 0;
}

static int TestCategoryNoFuzzyMatch()
{
    const std::string body = R"({"categories":[
        {"full_name":"Minecraft","game_mask_id":"222"},
        {"full_name":"Mine","game_mask_id":"333"}
    ]})";
    // Close but not equal: must NOT resolve to an unrelated game.
    auto r = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "Minecraf");
    CHECK(r.success);
    CHECK(r.categoryId.empty());
    CHECK(!r.matchedExact);

    auto unknown = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "Some Unknown Game");
    CHECK(unknown.success);
    CHECK(unknown.categoryId.empty());
    return 0;
}

static int TestCategoryOtherFallback()
{
    const std::string body = R"({"categories":[
        {"full_name":"Minecraft","game_mask_id":"222"},
        {"full_name":"Other","game_mask_id":"999"}
    ]})";
    auto r = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "Unknown Game");
    CHECK(r.success);
    CHECK(r.categoryId == "999");
    CHECK(!r.matchedExact);
    return 0;
}

static int TestCategoryDegradesGracefully()
{
    auto empty = StreamlabsTikTokAPI::ParseCategoryResponse(200, "{}", "");
    CHECK(empty.success);
    CHECK(empty.categoryId.empty());

    auto blank = StreamlabsTikTokAPI::ParseCategoryResponse(200, "{}", "   ");
    CHECK(blank.success);
    CHECK(blank.categoryId.empty());

    // 2xx with an unusable shape degrades to "Other", never blocks startup.
    auto badJson = StreamlabsTikTokAPI::ParseCategoryResponse(200, "not json", "Minecraft");
    CHECK(badJson.success);
    CHECK(badJson.categoryId.empty());

    auto missing = StreamlabsTikTokAPI::ParseCategoryResponse(200, R"({"ok":true})", "Minecraft");
    CHECK(missing.success);
    CHECK(missing.categoryId.empty());

    auto notArray = StreamlabsTikTokAPI::ParseCategoryResponse(200, R"({"categories":{}})", "Minecraft");
    CHECK(notArray.success);
    CHECK(notArray.categoryId.empty());
    return 0;
}

static int TestCategoryHttpErrors()
{
    auto auth = StreamlabsTikTokAPI::ParseCategoryResponse(401, "{}", "Minecraft");
    CHECK(!auth.success);
    CHECK(auth.failure == TikTokFailure::Auth);
    CHECK(!auth.error.empty());

    auto limited = StreamlabsTikTokAPI::ParseCategoryResponse(429, "{}", "Minecraft");
    CHECK(!limited.success);
    CHECK(limited.failure == TikTokFailure::RateLimited);

    auto server = StreamlabsTikTokAPI::ParseCategoryResponse(500, "{}", "Minecraft");
    CHECK(!server.success);
    CHECK(server.failure == TikTokFailure::Server);

    auto forbidden = StreamlabsTikTokAPI::ParseCategoryResponse(403, "{}", "Minecraft");
    CHECK(!forbidden.success);
    CHECK(forbidden.failure == TikTokFailure::Forbidden);
    return 0;
}

static int TestCategoryNumericId()
{
    const std::string body = R"({"categories":[{"full_name":"Minecraft","game_mask_id":222}]})";
    auto r = StreamlabsTikTokAPI::ParseCategoryResponse(200, body, "Minecraft");
    CHECK(r.success);
    CHECK(r.categoryId == "222");
    return 0;
}

static int TestTruncateToChars()
{
    CHECK(StreamlabsTikTokAPI::TruncateToChars("Minecraft", 25) == "Minecraft");
    CHECK(StreamlabsTikTokAPI::TruncateToChars("abcdefghijklmnopqrstuvwxyz", 25) == "abcdefghijklmnopqrstuvwxy");
    CHECK(StreamlabsTikTokAPI::TruncateToChars("", 25).empty());
    // "é" is 2 bytes / 1 char: truncation counts chars, not bytes.
    CHECK(StreamlabsTikTokAPI::TruncateToChars("caf\xC3\xA9meng", 4) == "caf\xC3\xA9");
    // Never split a multibyte sequence.
    const std::string split = StreamlabsTikTokAPI::TruncateToChars("ab\xC3\xA9", 2);
    CHECK(split == "ab");
    return 0;
}

static int TestStartResponseValid()
{
    const std::string body = R"({"id":"12345","rtmp":"rtmp://example/live","key":"stream-key"})";
    auto r = StreamlabsTikTokAPI::ParseStartResponse(200, body);
    CHECK(r.success);
    CHECK(r.failure == TikTokFailure::None);
    CHECK(r.streamId == "12345");
    CHECK(r.server == "rtmp://example/live");
    CHECK(r.key == "stream-key");
    CHECK(r.error.empty());

    const std::string numeric = R"({"id":12345,"rtmp":"rtmp://example/live","key":"k"})";
    auto n = StreamlabsTikTokAPI::ParseStartResponse(200, numeric);
    CHECK(n.success);
    CHECK(n.streamId == "12345");
    return 0;
}

static int TestStartResponseRequiresAllFields()
{
    auto missingKey = StreamlabsTikTokAPI::ParseStartResponse(200, R"({"id":"1","rtmp":"r"})");
    CHECK(!missingKey.success);
    CHECK(missingKey.failure == TikTokFailure::InvalidResponse);

    auto missingId = StreamlabsTikTokAPI::ParseStartResponse(200, R"({"rtmp":"r","key":"k"})");
    CHECK(!missingId.success);

    auto missingServer = StreamlabsTikTokAPI::ParseStartResponse(200, R"({"id":"1","key":"k"})");
    CHECK(!missingServer.success);

    auto emptyStrings = StreamlabsTikTokAPI::ParseStartResponse(200, R"({"id":"","rtmp":"","key":""})");
    CHECK(!emptyStrings.success);

    auto wrongTypes = StreamlabsTikTokAPI::ParseStartResponse(200, R"({"id":"1","rtmp":42,"key":"k"})");
    CHECK(!wrongTypes.success);

    auto badJson = StreamlabsTikTokAPI::ParseStartResponse(200, "not json");
    CHECK(!badJson.success);
    CHECK(badJson.failure == TikTokFailure::InvalidResponse);

    auto notObject = StreamlabsTikTokAPI::ParseStartResponse(200, "[1,2]");
    CHECK(!notObject.success);
    return 0;
}

static int TestStartResponseHttpErrors()
{
    auto auth = StreamlabsTikTokAPI::ParseStartResponse(401, "{}");
    CHECK(!auth.success);
    CHECK(auth.failure == TikTokFailure::Auth);

    auto forbidden = StreamlabsTikTokAPI::ParseStartResponse(403, "{}");
    CHECK(forbidden.failure == TikTokFailure::Forbidden);

    auto missing = StreamlabsTikTokAPI::ParseStartResponse(404, "{}");
    CHECK(missing.failure == TikTokFailure::NotFound);

    auto bad = StreamlabsTikTokAPI::ParseStartResponse(400, "{}");
    CHECK(bad.failure == TikTokFailure::BadRequest);

    auto limited = StreamlabsTikTokAPI::ParseStartResponse(429, "{}");
    CHECK(limited.failure == TikTokFailure::RateLimited);

    auto server = StreamlabsTikTokAPI::ParseStartResponse(503, "{}");
    CHECK(server.failure == TikTokFailure::Server);
    return 0;
}

static int TestServerMessageHandling()
{
    auto direct = StreamlabsTikTokAPI::ParseStartResponse(400, R"({"message":"no tiktok linked"})");
    CHECK(!direct.success);
    CHECK(direct.error.find("no tiktok linked") != std::string::npos);

    auto nested = StreamlabsTikTokAPI::ParseStartResponse(400, R"({"data":{"message":"nested detail"}})");
    CHECK(nested.error.find("nested detail") != std::string::npos);

    // Control characters are flattened so the detail stays single-line.
    auto multiline = StreamlabsTikTokAPI::ParseStartResponse(400, R"({"message":"a\nb\rc"})");
    CHECK(multiline.error.find('\n') == std::string::npos);
    CHECK(multiline.error.find('\r') == std::string::npos);

    // Long messages are bounded.
    const std::string big(2000, 'x');
    auto capped = StreamlabsTikTokAPI::ParseStartResponse(400, "{\"message\":\"" + big + "\"}");
    CHECK(capped.error.size() < 2000);

    // Secrets in non-message fields never leak into errors.
    auto leak = StreamlabsTikTokAPI::ParseStartResponse(200, R"({"id":"1","rtmp":"r","other":"KEY-SECRET-123"})");
    CHECK(!leak.success);
    CHECK(leak.error.find("KEY-SECRET-123") == std::string::npos);
    return 0;
}

static int TestEndResponse()
{
    std::string error;
    CHECK(StreamlabsTikTokAPI::ParseEndResponse(200, R"({"success":true})", &error));
    CHECK(!StreamlabsTikTokAPI::ParseEndResponse(200, R"({"success":false})", &error));
    CHECK(!error.empty());
    // 2xx with a non-JSON body still counts as ended.
    CHECK(StreamlabsTikTokAPI::ParseEndResponse(200, "ok", &error));
    CHECK(!StreamlabsTikTokAPI::ParseEndResponse(404, "{}", &error));
    CHECK(!error.empty());
    return 0;
}

static int TestEmptyCredentialRejectedLocally()
{
    // These validate input before any network use, so they are safe offline.
    auto start = StreamlabsTikTokAPI::StartStream("", "title", "", false);
    CHECK(!start.success);
    CHECK(start.failure == TikTokFailure::Auth);

    auto category = StreamlabsTikTokAPI::ResolveCategory("", "Minecraft");
    CHECK(!category.success);

    auto account = StreamlabsTikTokAPI::GetAccountInfo("");
    CHECK(!account.success);
    CHECK(account.failure == TikTokFailure::Auth);

    auto list = StreamlabsTikTokAPI::ListCategories("", "Minecraft");
    CHECK(!list.success);

    std::string error;
    CHECK(!StreamlabsTikTokAPI::EndStream("", "123", &error));
    CHECK(!StreamlabsTikTokAPI::EndStream("tok", "", &error));
    return 0;
}

static int TestAccountResponseValid()
{
    const std::string body =
        R"({"user":{"username":"someuser","display_name":"Some User"},"categories":[]})";
    auto r = StreamlabsTikTokAPI::ParseAccountResponse(200, body);
    CHECK(r.success);
    CHECK(r.failure == TikTokFailure::None);
    CHECK(r.account.username == "someuser");
    CHECK(r.account.displayName == "Some User");
    CHECK(!r.account.liveEligibilityKnown);

    // Nested data.user shape is tolerated too.
    auto nested =
        StreamlabsTikTokAPI::ParseAccountResponse(200, R"({"data":{"user":{"username":"u"}}})");
    CHECK(nested.success);
    CHECK(nested.account.username == "u");
    return 0;
}

static int TestAccountResponseDegradesGracefully()
{
    // 2xx without identity fields still proves the credential: auth
    // succeeds with a partial profile.
    auto missing = StreamlabsTikTokAPI::ParseAccountResponse(200, R"({"ok":true})");
    CHECK(missing.success);
    CHECK(missing.account.username.empty());

    auto noDisplay = StreamlabsTikTokAPI::ParseAccountResponse(200, R"({"user":{"username":"u"}})");
    CHECK(noDisplay.success);
    CHECK(noDisplay.account.displayName.empty());
    return 0;
}

static int TestAccountLiveEligibility()
{
    const std::string body = R"({"user":{"username":"u"},"can_be_live":false,)"
                               R"("application_status":"review","reason":"need followers"})";
    auto r = StreamlabsTikTokAPI::ParseAccountResponse(200, body);
    CHECK(r.success);
    CHECK(r.account.liveEligibilityKnown);
    CHECK(!r.account.canGoLive);
    CHECK(r.account.liveUnavailableReason.find("followers") != std::string::npos);
    CHECK(r.account.applicationStatus == "review");

    auto eligible =
        StreamlabsTikTokAPI::ParseAccountResponse(200, R"({"user":{"username":"u"},"can_be_live":true})");
    CHECK(eligible.success);
    CHECK(eligible.account.liveEligibilityKnown);
    CHECK(eligible.account.canGoLive);

    // Application status without an eligibility verdict: surfaced for
    // the pending display, eligibility stays unknown.
    auto pending = StreamlabsTikTokAPI::ParseAccountResponse(
        200, R"({"user":{"username":"u"},"application_status":"pending"})");
    CHECK(pending.success);
    CHECK(!pending.account.liveEligibilityKnown);
    CHECK(pending.account.applicationStatus == "pending");
    return 0;
}

static int TestAccountResponseHttpErrors()
{
    auto auth = StreamlabsTikTokAPI::ParseAccountResponse(401, "{}");
    CHECK(!auth.success);
    CHECK(auth.failure == TikTokFailure::Auth);

    auto forbidden = StreamlabsTikTokAPI::ParseAccountResponse(403, "{}");
    CHECK(forbidden.failure == TikTokFailure::Forbidden);

    auto server = StreamlabsTikTokAPI::ParseAccountResponse(500, "{}");
    CHECK(server.failure == TikTokFailure::Server);

    auto bad = StreamlabsTikTokAPI::ParseAccountResponse(200, "not json");
    CHECK(!bad.success);
    CHECK(bad.failure == TikTokFailure::InvalidResponse);
    return 0;
}

static int TestCategoryListResponse()
{
    const std::string body = R"({"categories":[
        {"full_name":"Just Chatting","game_mask_id":"111"},
        {"full_name":"Minecraft","game_mask_id":222},
        {"full_name":"","game_mask_id":"x"},
        {"nope":true}
    ]})";
    auto r = StreamlabsTikTokAPI::ParseCategoryListResponse(200, body);
    CHECK(r.success);
    CHECK(r.categories.size() == 2);
    CHECK(r.categories[0].fullName == "Just Chatting");
    CHECK(r.categories[0].gameMaskId == "111");
    CHECK(r.categories[1].gameMaskId == "222");

    // 2xx without categories: empty list, still success.
    auto empty = StreamlabsTikTokAPI::ParseCategoryListResponse(200, R"({"ok":true})");
    CHECK(empty.success);
    CHECK(empty.categories.empty());

    auto auth = StreamlabsTikTokAPI::ParseCategoryListResponse(401, "{}");
    CHECK(!auth.success);
    CHECK(auth.failure == TikTokFailure::Auth);

    auto bad = StreamlabsTikTokAPI::ParseCategoryListResponse(200, "not json");
    CHECK(!bad.success);
    CHECK(bad.failure == TikTokFailure::InvalidResponse);
    return 0;
}

int main()
{
    if (TestCategoryExactMatch())
        return 1;
    if (TestCategoryNoFuzzyMatch())
        return 1;
    if (TestCategoryOtherFallback())
        return 1;
    if (TestCategoryDegradesGracefully())
        return 1;
    if (TestCategoryHttpErrors())
        return 1;
    if (TestCategoryNumericId())
        return 1;
    if (TestTruncateToChars())
        return 1;
    if (TestStartResponseValid())
        return 1;
    if (TestStartResponseRequiresAllFields())
        return 1;
    if (TestStartResponseHttpErrors())
        return 1;
    if (TestServerMessageHandling())
        return 1;
    if (TestEndResponse())
        return 1;
    if (TestEmptyCredentialRejectedLocally())
        return 1;
    if (TestAccountResponseValid())
        return 1;
    if (TestAccountResponseDegradesGracefully())
        return 1;
    if (TestAccountLiveEligibility())
        return 1;
    if (TestAccountResponseHttpErrors())
        return 1;
    if (TestCategoryListResponse())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
