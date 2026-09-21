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

std::string Narrow(const wchar_t *text)
{
    if (!text || !*text)
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr, nullptr);
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

#else

// Linux: Secret Service via libsecret, loaded with dlopen so the plugin
// keeps working (memory-only) where libsecret is not installed. Only the
// three password helpers plus error/password release are resolved.

#include <dlfcn.h>

namespace {

// Minimal ABI-compatible subset of libsecret-1/GLib types.
using gboolean = int;
using gchar = char;
using gint = int;
using guint32 = unsigned int;
using gpointer = void *;
using GQuark = guint32;
struct GError {
    GQuark domain;
    gint code;
    gchar *message;
};
struct GCancellable;
enum SecretSchemaFlags { SECRET_SCHEMA_NONE = 0 };
enum SecretSchemaAttributeType { SECRET_SCHEMA_ATTRIBUTE_STRING = 0 };
struct SecretSchemaAttribute {
    const gchar *name;
    SecretSchemaAttributeType type;
};
struct SecretSchema {
    const gchar *name;
    SecretSchemaFlags flags;
    SecretSchemaAttribute attributes[32];
    gint reserved;
    gpointer reserved2;
    gpointer reserved3;
    gpointer reserved4;
    gpointer reserved5;
    gpointer reserved6;
    gpointer reserved7;
};

using SecretPasswordStoreSync = gboolean (*)(const SecretSchema *, const gchar *, const gchar *,
                                             const gchar *, GCancellable *, GError **, ...);
using SecretPasswordLookupSync = gchar *(*)(const SecretSchema *, GCancellable *, GError **, ...);
using SecretPasswordClearSync = gboolean (*)(const SecretSchema *, GCancellable *, GError **, ...);
using SecretPasswordFree = void (*)(gchar *);
using GErrorFree = void (*)(GError *);

constexpr char kCollectionDefault[] = "default";

SecretSchema MakeSchema()
{
    SecretSchema schema{};
    schema.name = TikTokConstants::kCredentialService;
    schema.flags = SECRET_SCHEMA_NONE;
    schema.attributes[0] = {"service", SECRET_SCHEMA_ATTRIBUTE_STRING};
    schema.attributes[1] = {"account", SECRET_SCHEMA_ATTRIBUTE_STRING};
    return schema;
}

class LibsecretCredentialStore : public ICredentialStore {
public:
    LibsecretCredentialStore()
    {
        handle_ = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
        if (!handle_)
            handle_ = dlopen("libsecret-1.so", RTLD_NOW | RTLD_LOCAL);
        if (handle_) {
            store_ = reinterpret_cast<SecretPasswordStoreSync>(dlsym(handle_, "secret_password_store_sync"));
            lookup_ =
                reinterpret_cast<SecretPasswordLookupSync>(dlsym(handle_, "secret_password_lookup_sync"));
            clear_ =
                reinterpret_cast<SecretPasswordClearSync>(dlsym(handle_, "secret_password_clear_sync"));
            passFree_ = reinterpret_cast<SecretPasswordFree>(dlsym(handle_, "secret_password_free"));
            errorFree_ = reinterpret_cast<GErrorFree>(dlsym(handle_, "g_error_free"));
            if (!store_ || !lookup_ || !clear_ || !passFree_ || !errorFree_) {
                dlclose(handle_);
                handle_ = nullptr;
            }
        }
        schema_ = MakeSchema();
    }

    ~LibsecretCredentialStore() override
    {
        if (handle_)
            dlclose(handle_);
    }

    bool available() const { return handle_ != nullptr; }

    std::optional<std::string> load() override
    {
        if (!handle_)
            return std::nullopt;
        GError *error = nullptr;
        gchar *password = lookup_(&schema_, nullptr, &error, "service",
                                  TikTokConstants::kCredentialService, "account",
                                  TikTokConstants::kCredentialAccount, nullptr);
        if (error) {
            errorFree_(error);
            error = nullptr;
        }
        if (!password)
            return std::nullopt;
        std::string token(password);
        passFree_(password);
        if (token.empty())
            return std::nullopt;
        return token;
    }

    bool save(const std::string &token) override
    {
        if (!handle_ || token.empty())
            return false;
        GError *error = nullptr;
        const gboolean ok =
            store_(&schema_, kCollectionDefault, "OBS Multi-RTMP TikTok OAuth token", token.c_str(),
                   nullptr, &error, "service", TikTokConstants::kCredentialService, "account",
                   TikTokConstants::kCredentialAccount, nullptr);
        if (error) {
            errorFree_(error);
        }
        return ok != 0;
    }

    void remove() override
    {
        if (!handle_)
            return;
        GError *error = nullptr;
        clear_(&schema_, nullptr, &error, "service", TikTokConstants::kCredentialService, "account",
               TikTokConstants::kCredentialAccount, nullptr);
        if (error) {
            errorFree_(error);
        }
    }

private:
    void *handle_ = nullptr;
    SecretPasswordStoreSync store_ = nullptr;
    SecretPasswordLookupSync lookup_ = nullptr;
    SecretPasswordClearSync clear_ = nullptr;
    SecretPasswordFree passFree_ = nullptr;
    GErrorFree errorFree_ = nullptr;
    SecretSchema schema_{};
};

} // namespace

std::unique_ptr<ICredentialStore> CreateSecureCredentialStore()
{
    auto store = std::make_unique<LibsecretCredentialStore>();
    if (!store->available())
        return nullptr;
    // Probe the daemon without touching the real entry: a load attempt
    // fails closed (nullopt) when the Secret Service is unreachable, but
    // that is indistinguishable from "no credential". Availability is
    // decided lazily at save time instead; returning the store here is
    // safe because every method fails closed.
    return store;
}

#endif
