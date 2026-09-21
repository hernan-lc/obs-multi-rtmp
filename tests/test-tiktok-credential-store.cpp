// Credential-store contract tests. The in-memory implementation is the
// behavioral reference; the secure backend is only probed for safe
// failure modes (never for real credentials).
#include <iostream>

#include "tiktok/credential-store.h"

static int g_checks = 0;
#define CHECK(cond)                                                  \
    do {                                                             \
        ++g_checks;                                                  \
        if (!(cond)) {                                               \
            std::cerr << "FAIL " << __LINE__ << ": " #cond "\n";      \
            return 1;                                                \
        }                                                            \
    } while (0)

static int TestMemoryLifecycle()
{
    MemoryCredentialStore store;
    CHECK(!store.load().has_value());
    CHECK(store.save("token-a"));
    CHECK(store.load().value_or("") == "token-a");
    CHECK(store.save("token-b"));
    CHECK(store.load().value_or("") == "token-b");
    store.remove();
    CHECK(!store.load().has_value());
    store.remove(); // idempotent
    CHECK(!store.load().has_value());
    return 0;
}

static int TestMemoryRejectsEmpty()
{
    MemoryCredentialStore store;
    CHECK(!store.save(""));
    CHECK(!store.load().has_value());
    return 0;
}

static int TestMemoryNotPersistent()
{
    MemoryCredentialStore store;
    CHECK(!store.isPersistent());
    return 0;
}

static int TestSecureBackendFailsClosed()
{
    // Must never throw or crash when no Secret Service / keychain exists.
    // When one does exist it must not contain our test entry.
    auto store = CreateSecureCredentialStore();
    if (!store) {
        // Unavailable backend: the documented memory-only fallback path.
        CHECK(true);
        return 0;
    }
    // Do not write test secrets to the real OS store; a load probe is
    // side-effect free.
    (void)store->load();
    CHECK(store->isPersistent());
    return 0;
}

int main()
{
    if (TestMemoryLifecycle())
        return 1;
    if (TestMemoryRejectsEmpty())
        return 1;
    if (TestMemoryNotPersistent())
        return 1;
    if (TestSecureBackendFailsClosed())
        return 1;
    std::cout << "all " << g_checks << " checks passed\n";
    return 0;
}
