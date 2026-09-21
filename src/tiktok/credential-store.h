#pragma once

// Secure OAuth credential storage. The bearer token is account state and
// must never live in obs-multi-rtmp.json, logs, or target configuration.
//
// Platform backends:
//   Linux:   Secret Service via linked libsecret when available,
//            otherwise memory-only
//   Windows: Windows Credential Manager
//   macOS:   Keychain Services
//
// One stable entry for the whole plugin (service + account from
// TikTokConstants). When no secure backend is available the manager
// falls back to memory-only storage for the current OBS session and
// warns the user; it never silently writes the token in plaintext.
//
// Qt-free and OBS-free for unit testing.

#include <memory>
#include <mutex>
#include <optional>
#include <string>

class ICredentialStore {
public:
    virtual ~ICredentialStore() = default;

    virtual std::optional<std::string> load() = 0;
    virtual bool save(const std::string &token) = 0;
    virtual void remove() = 0;

    // False when this store cannot persist across restarts.
    virtual bool isPersistent() const { return true; }
};

// In-memory store for tests and for the memory-only fallback session.
// Internally synchronized: the auth manager may touch it from a worker
// thread (restore) while the UI thread logs out.
class MemoryCredentialStore : public ICredentialStore {
public:
    std::optional<std::string> load() override;
    bool save(const std::string &token) override;
    void remove() override;
    bool isPersistent() const override { return false; }

private:
    std::mutex mutex_;
    std::optional<std::string> token_;
};

// Creates the best secure backend for the current platform, or nullptr
// when secure storage is unavailable (caller falls back to memory-only).
std::unique_ptr<ICredentialStore> CreateSecureCredentialStore();
