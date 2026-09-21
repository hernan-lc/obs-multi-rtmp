#include "tiktok/credential-store.h"
#include "tiktok/constants.h"

#include <cstring>

std::optional<std::string> MemoryCredentialStore::load()
{
    std::lock_guard<std::mutex> lock(mutex_);
    return token_;
}

bool MemoryCredentialStore::save(const std::string &token)
{
    if (token.empty())
        return false;
    std::lock_guard<std::mutex> lock(mutex_);
    token_ = token;
    return true;
}

void MemoryCredentialStore::remove()
{
    std::lock_guard<std::mutex> lock(mutex_);
    token_.reset();
}

#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>

namespace {

std::wstring Widen(const std::string &text)
{
    if (text.empty())
        return {};
    const int needed =
        MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::wstring TargetName()
{
    return Widen(std::string(TikTokConstants::kCredentialService) + "/" +
                 TikTokConstants::kCredentialAccount);
}

class WindowsCredentialStore : public ICredentialStore {
public:
    std::optional<std::string> load() override
    {
        PCREDENTIALW cred = nullptr;
        if (!CredReadW(TargetName().c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
            return std::nullopt;
        std::optional<std::string> token;
        if (cred->CredentialBlob && cred->CredentialBlobSize > 0) {
            // Blob was stored as raw UTF-8 bytes.
            const auto *bytes = static_cast<const char *>(static_cast<const void *>(cred->CredentialBlob));
            token = std::string(bytes, bytes + cred->CredentialBlobSize);
        }
        CredFree(cred);
        if (!token.has_value() || token->empty())
            return std::nullopt;
        return token;
    }

    bool save(const std::string &token) override
    {
        if (token.empty())
            return false;
        const std::wstring target = TargetName();
        const std::wstring user = Widen(TikTokConstants::kCredentialAccount);
        CREDENTIALW cred{};
        cred.Type = CRED_TYPE_GENERIC;
        cred.TargetName = const_cast<LPWSTR>(target.c_str());
        cred.UserName = const_cast<LPWSTR>(user.c_str());
        cred.CredentialBlobSize = static_cast<DWORD>(token.size());
        cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char *>(token.data()));
        cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
        return CredWriteW(&cred, 0) != FALSE;
    }

    void remove() override { CredDeleteW(TargetName().c_str(), CRED_TYPE_GENERIC, 0); }
};

} // namespace

std::unique_ptr<ICredentialStore> CreateSecureCredentialStore()
{
    return std::make_unique<WindowsCredentialStore>();
}

#elif defined(__APPLE__)

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace {

class KeychainCredentialStore : public ICredentialStore {
public:
    std::optional<std::string> load() override
    {
        CFStringRef service = CFStringCreateWithCString(nullptr, TikTokConstants::kCredentialService,
                                                        kCFStringEncodingUTF8);
        CFStringRef account = CFStringCreateWithCString(nullptr, TikTokConstants::kCredentialAccount,
                                                        kCFStringEncodingUTF8);
        CFMutableDictionaryRef query =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, service);
        CFDictionarySetValue(query, kSecAttrAccount, account);
        CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
        CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);
        CFTypeRef result = nullptr;
        const OSStatus status = SecItemCopyMatching(query, &result);
        CFRelease(query);
        CFRelease(service);
        CFRelease(account);
        if (status != errSecSuccess || !result)
            return std::nullopt;
        std::optional<std::string> token;
        if (CFGetTypeID(result) == CFDataGetTypeID()) {
            CFDataRef data = static_cast<CFDataRef>(result);
            token = std::string(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
                                static_cast<size_t>(CFDataGetLength(data)));
        }
        CFRelease(result);
        if (!token.has_value() || token->empty())
            return std::nullopt;
        return token;
    }

    bool save(const std::string &token) override
    {
        if (token.empty())
            return false;
        CFStringRef service = CFStringCreateWithCString(nullptr, TikTokConstants::kCredentialService,
                                                        kCFStringEncodingUTF8);
        CFStringRef account = CFStringCreateWithCString(nullptr, TikTokConstants::kCredentialAccount,
                                                        kCFStringEncodingUTF8);
        CFDataRef secret = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(token.data()),
                                        static_cast<CFIndex>(token.size()));
        CFMutableDictionaryRef query =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, service);
        CFDictionarySetValue(query, kSecAttrAccount, account);
        CFMutableDictionaryRef update =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(update, kSecValueData, secret);
        OSStatus status = SecItemUpdate(query, update);
        if (status == errSecItemNotFound) {
            CFDictionarySetValue(query, kSecValueData, secret);
            status = SecItemAdd(query, nullptr);
        }
        CFRelease(update);
        CFRelease(query);
        CFRelease(secret);
        CFRelease(service);
        CFRelease(account);
        return status == errSecSuccess;
    }

    void remove() override
    {
        CFStringRef service = CFStringCreateWithCString(nullptr, TikTokConstants::kCredentialService,
                                                        kCFStringEncodingUTF8);
        CFStringRef account = CFStringCreateWithCString(nullptr, TikTokConstants::kCredentialAccount,
                                                        kCFStringEncodingUTF8);
        CFMutableDictionaryRef query =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                      &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
        CFDictionarySetValue(query, kSecAttrService, service);
        CFDictionarySetValue(query, kSecAttrAccount, account);
        SecItemDelete(query);
        CFRelease(query);
        CFRelease(service);
        CFRelease(account);
    }
};

} // namespace

std::unique_ptr<ICredentialStore> CreateSecureCredentialStore()
{
    return std::make_unique<KeychainCredentialStore>();
}

#else // Linux

#if defined(TIKTOK_USE_LIBSECRET)

#include <libsecret/secret.h>

namespace {

const SecretSchema kTikTokSchema = {
    TikTokConstants::kCredentialService,
    SECRET_SCHEMA_NONE,
    {
        {"service", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {"account", SECRET_SCHEMA_ATTRIBUTE_STRING},
        {nullptr, SECRET_SCHEMA_ATTRIBUTE_STRING},
    },
};

class LibsecretCredentialStore : public ICredentialStore {
public:
    std::optional<std::string> load() override
    {
        GError *error = nullptr;
        gchar *password = secret_password_lookup_sync(
            &kTikTokSchema, nullptr, &error, "service", TikTokConstants::kCredentialService,
            "account", TikTokConstants::kCredentialAccount, nullptr);
        if (error != nullptr) {
            g_error_free(error);
            error = nullptr;
        }
        if (password == nullptr)
            return std::nullopt;
        std::string token(password);
        secret_password_free(password);
        if (token.empty())
            return std::nullopt;
        return token;
    }

    bool save(const std::string &token) override
    {
        if (token.empty())
            return false;
        GError *error = nullptr;
        const gboolean ok = secret_password_store_sync(
            &kTikTokSchema, SECRET_COLLECTION_DEFAULT, "OBS Multi-RTMP TikTok OAuth token",
            token.c_str(), nullptr, &error, "service", TikTokConstants::kCredentialService,
            "account", TikTokConstants::kCredentialAccount, nullptr);
        if (error != nullptr)
            g_error_free(error);
        return ok != 0;
    }

    void remove() override
    {
        GError *error = nullptr;
        secret_password_clear_sync(&kTikTokSchema, nullptr, &error, "service",
                                   TikTokConstants::kCredentialService, "account",
                                   TikTokConstants::kCredentialAccount, nullptr);
        if (error != nullptr)
            g_error_free(error);
    }
};

} // namespace

std::unique_ptr<ICredentialStore> CreateSecureCredentialStore()
{
    // Every method fails closed when the Secret Service is unreachable.
    return std::make_unique<LibsecretCredentialStore>();
}

#else // !TIKTOK_USE_LIBSECRET

std::unique_ptr<ICredentialStore> CreateSecureCredentialStore()
{
    // Built without libsecret development files: memory-only fallback.
    return nullptr;
}

#endif // TIKTOK_USE_LIBSECRET

#endif
