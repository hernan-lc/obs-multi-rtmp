// Exchange script builder / result parser tests. Fixture tokens are fake
// ("T", "TOKEN"); diagnostics never echo payloads.
#include <iostream>

#include "tiktok/exchange.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestPendingPayloads()
{
    for (const char *payload : {"", "   ", "null", "  null  "}) {
        const ExchangeOutcome outcome = ParseExchangePayload(payload);
        CHECK(!outcome.settled);
        CHECK(!outcome.success);
        CHECK(outcome.token.empty());
    }
    return 0;
}

static int TestValidTokenResponse()
{
    const ExchangeOutcome outcome = ParseExchangePayload(
        R"({"success": true, "data": {"success": true, "data": {"oauth_token": "T"}}, "status": 200, "page": "www.streamlabs.com/dashboard"})");
    CHECK(outcome.settled);
    CHECK(outcome.success);
    CHECK(outcome.token == "T");
    CHECK(outcome.error.empty());
    CHECK(outcome.httpStatus.value_or(0) == 200);
    CHECK(outcome.page == "www.streamlabs.com/dashboard");
    return 0;
}

static int TestRejectedShapes()
{
    // success=false envelope.
    CHECK(!ParseExchangePayload(R"({"success": true, "data": {"success": false}})").success);
    // Missing data.
    CHECK(!ParseExchangePayload(R"({"success": true})").success);
    CHECK(!ParseExchangePayload(R"({"success": true, "data": {}})").success);
    // Missing / empty token.
    CHECK(!ParseExchangePayload(R"({"success": true, "data": {"success": true, "data": {}}})").success);
    CHECK(!ParseExchangePayload(
               R"({"success": true, "data": {"success": true, "data": {"oauth_token": ""}}})")
               .success);
    // Transport failure shape.
    {
        const ExchangeOutcome outcome =
            ParseExchangePayload(R"({"success": false, "error": "TypeError: failed"})");
        CHECK(outcome.settled);
        CHECK(!outcome.success);
        CHECK(!outcome.error.empty());
        CHECK(outcome.token.empty());
    }
    // JSON parse failure shape.
    {
        const ExchangeOutcome outcome = ParseExchangePayload(
            R"({"success": false, "error": "JSON Parse Error", "body": "<html>", "status": 200})");
        CHECK(outcome.settled);
        CHECK(!outcome.success);
        // Raw body must not leak into the error text.
        CHECK(outcome.error.find("<html>") == std::string::npos);
    }
    // Invalid JSON / wrong types.
    CHECK(!ParseExchangePayload("<html>not json</html>").success);
    CHECK(!ParseExchangePayload("[1,2]").success);
    // Every settled failure carries a safe detail.
    for (const char *payload :
         {R"({"success": false})", "42", R"({"success": true, "data": 7})"}) {
        const ExchangeOutcome outcome = ParseExchangePayload(payload);
        CHECK(outcome.settled);
        CHECK(!outcome.error.empty());
    }
    return 0;
}

static int TestErrorSanitizing()
{
    // Control characters flattened, length bounded.
    const std::string evil(500, 'x');
    const ExchangeOutcome outcome =
        ParseExchangePayload("{\"success\": false, \"error\": \"a\\n\\rb" + evil + "\"}");
    CHECK(outcome.error.find('\n') == std::string::npos);
    CHECK(outcome.error.find('\r') == std::string::npos);
    CHECK(outcome.error.size() < 300);
    return 0;
}

static int TestScriptBuilders()
{
    const std::string starter =
        BuildExchangeStartJs("https://streamlabs.com/api/v5/slobs/auth/data?code=A", "h/p");
    CHECK(starter.find("credentials") != std::string::npos);
    CHECK(starter.find("X-Requested-With") != std::string::npos);
    CHECK(starter.find("__multirtmp_tiktok_exchange") != std::string::npos);
    CHECK(starter.find("window.location.host + window.location.pathname") != std::string::npos);
    // Dynamic values travel as JSON string literals.
    CHECK(starter.find(JsonEscape("h/p")) != std::string::npos);

    const std::string poll = BuildExchangePollJs("h/p");
    CHECK(poll.find("__multirtmp_tiktok_exchange") != std::string::npos);
    CHECK(poll.find(JsonEscape("h/p")) != std::string::npos);

    // Escaping keeps scripts syntactically intact.
    CHECK(JsonEscape("a\"b\\c\nd") == "\"a\\\"b\\\\c\\nd\"");
    CHECK(BuildExchangeStartJs("https://x/?q=\"", "p\"").find("\\\"") != std::string::npos);
    return 0;
}

static int TestExchangePageFence()
{
    const std::string starter = BuildExchangeStartJs("https://streamlabs.com/api/v5/slobs/auth/data", "h/p");
    const size_t pageCheck = starter.find("if (P !== EP) return 'wrong-page';");
    const size_t fetchCall = starter.find("fetch(");
    CHECK(pageCheck != std::string::npos);
    CHECK(fetchCall != std::string::npos);
    CHECK(pageCheck < fetchCall);
    CHECK(starter.find("window.__multirtmp_tiktok_exchange_seq", pageCheck) != std::string::npos);
    // Plain HTTP is never an acceptable exchange document.
    CHECK(starter.find("window.location.protocol !== 'https:'") != std::string::npos);
    return 0;
}

static int TestPollPageFence()
{
    const std::string poll = BuildExchangePollJs("h/p");
    // The poll script only reports a result on the expected document, so a
    // result from an unrelated page can never be mistaken for the exchange.
    CHECK(poll.find("if (P !== EP) return null;") != std::string::npos);
    CHECK(poll.find("window.location.protocol !== 'https:'") != std::string::npos);
    const size_t pageCheck = poll.find("if (P !== EP) return null;");
    const size_t read = poll.find("window.__multirtmp_tiktok_exchange");
    CHECK(pageCheck != std::string::npos);
    CHECK(read != std::string::npos);
    CHECK(pageCheck < read);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--emit-starter") {
        std::cout << BuildExchangeStartJs("https://fixture.invalid/exchange", "streamlabs.com/dashboard");
        return 0;
    }
    if (TestPendingPayloads())
        return 1;
    if (TestValidTokenResponse())
        return 1;
    if (TestRejectedShapes())
        return 1;
    if (TestErrorSanitizing())
        return 1;
    if (TestScriptBuilders())
        return 1;
    if (TestExchangePageFence())
        return 1;
    if (TestPollPageFence())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
