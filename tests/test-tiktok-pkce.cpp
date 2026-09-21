// PKCE generation tests. No network, no secrets in diagnostics.
#include <cassert>
#include <iostream>

#include <QByteArray>

#include "tiktok/pkce.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static bool IsLowerHex(const QByteArray &text)
{
    if (text.isEmpty())
        return false;
    for (char c : text) {
        const bool digit = c >= '0' && c <= '9';
        const bool hex = c >= 'a' && c <= 'f';
        if (!digit && !hex)
            return false;
    }
    return true;
}

static int TestVerifierShape()
{
    const PkcePair pair = GeneratePkce();
    CHECK(pair.verifier.size() == 128);
    CHECK(IsLowerHex(pair.verifier));
    return 0;
}

static int TestChallengeShape()
{
    const PkcePair pair = GeneratePkce();
    // base64url(sha256) without padding: 43 chars, URL-safe alphabet.
    CHECK(pair.challenge.size() == 43);
    CHECK(!pair.challenge.contains('='));
    CHECK(!pair.challenge.contains('+'));
    CHECK(!pair.challenge.contains('/'));
    for (char c : pair.challenge) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '-' || c == '_';
        CHECK(ok);
    }
    return 0;
}

static int TestKnownVector()
{
    // RFC 7636 Appendix B test vector.
    const QByteArray verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
    CHECK(PkceChallengeFor(verifier) == "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    return 0;
}

static int TestFreshPairPerAttempt()
{
    const PkcePair a = GeneratePkce();
    const PkcePair b = GeneratePkce();
    CHECK(a.verifier != b.verifier);
    CHECK(a.challenge != b.challenge);
    CHECK(PkceChallengeFor(a.verifier) == a.challenge);
    return 0;
}

int main()
{
    if (TestVerifierShape())
        return 1;
    if (TestChallengeShape())
        return 1;
    if (TestKnownVector())
        return 1;
    if (TestFreshPairPerAttempt())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
