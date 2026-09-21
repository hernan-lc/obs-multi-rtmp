#pragma once

// Managed worker pool for TikTok background jobs (network calls,
// credential-store I/O, LIVE cleanup). Replaces every detached
// std::thread in the TikTok integration.
//
// Safety contract:
// - Work lambdas run on pool threads and must never touch QObjects.
// - Done lambdas run on the service (UI/main) thread and must use the
//   context's QPointer receiver, which is assigned on the submitting
//   thread and read back only here.
// - UI owners call cancelReceiver(this) in their destructors. That
//   orphans (never aborts) their jobs: Done never runs, but the worker
//   continues safely and performs its own terminal cleanup, so a LIVE
//   created after cancellation is still ended.
// - shutdown() aborts normal jobs via TikTokNetworkCancel, keeps
//   cleanup jobs, and joins all pool threads before module unload, so
//   plugin code can never execute after unload. Every job is bounded
//   (curl timeouts plus cooperative cancellation), so the join cannot
//   hang forever.

#include <QObject>
#include <QPointer>
#include <QThreadPool>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_set>

struct TikTokJobContext {
    explicit TikTokJobContext(QObject *receiver, bool activity, bool cleanup);

    // UI/main thread only: assigned at submit, read in Done.
    QPointer<QObject> receiver;
    // Activity jobs (prepare/cleanup) block logout while outstanding,
    // including after cancellation until the worker actually finishes.
    const bool activity;
    // Cleanup jobs are never cancelled or dropped; shutdown waits for
    // them so every created LIVE is eventually ended.
    const bool cleanup;
    // Abort in-flight transfers (shutdown only). Done never runs.
    std::atomic<bool> cancelled{false};
    // Receiver destroyed: Done never runs, but Work continues and must
    // perform its own terminal cleanup (e.g. end a created LIVE).
    std::atomic<bool> orphaned{false};
};

class TikTokWorkerService : public QObject {
public:
    // Work runs on a pool thread with TikTokNetworkCancel armed from the
    // context. Done runs on the service thread iff the job was not
    // cancelled, its receiver is alive, and shutdown has not begun.
    using Work = std::function<void(const std::shared_ptr<TikTokJobContext> &)>;
    using Done = std::function<void(const std::shared_ptr<TikTokJobContext> &)>;

    static TikTokWorkerService &instance();

    // Returns null once shutdown has begun (caller must handle it as a
    // soft failure, never by dereferencing).
    std::shared_ptr<TikTokJobContext> submit(QObject *receiver, Work work, Done done, bool activity,
                                             bool cleanup = false);

    // Orphans every pending non-cleanup job for receiver: their Done
    // callbacks are dropped while their workers run to completion
    // (performing terminal cleanup as needed). Must be called on the UI
    // thread, typically from a destructor before QObject teardown.
    void cancelReceiver(QObject *receiver);

    // True while activity-class jobs are queued or running.
    bool hasActiveActivity() const;
    bool isShuttingDown() const { return shuttingDown_.load(); }

    // Rejects new jobs, cancels normal jobs, drops their queued
    // runnables, then joins every worker thread. Idempotent.
    void shutdown();

private:
    explicit TikTokWorkerService(QObject *parent = nullptr);

    class Runnable;
    void finishJob(const std::shared_ptr<TikTokJobContext> &context, Done done);
    void abandonJob(const std::shared_ptr<TikTokJobContext> &context);

    QThreadPool normalPool_;
    QThreadPool cleanupPool_;
    mutable std::mutex mutex_;
    std::unordered_set<std::shared_ptr<TikTokJobContext>> pending_;
    std::atomic<bool> shuttingDown_{false};
};
