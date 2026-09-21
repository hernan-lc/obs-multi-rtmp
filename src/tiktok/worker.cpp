#include "tiktok/worker.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QRunnable>
#include <QThread>

#include "streamlabs-tiktok.h"

TikTokJobContext::TikTokJobContext(QObject *receiver, bool activity, bool cleanup)
    : receiver(receiver)
    , activity(activity)
    , cleanup(cleanup)
{
}

class TikTokWorkerService::Runnable : public QRunnable {
public:
    Runnable(TikTokWorkerService *service, std::shared_ptr<TikTokJobContext> context, Work work,
             Done done)
        : service_(service)
        , context_(std::move(context))
        , work_(std::move(work))
        , done_(std::move(done))
    {
    }

    void run() override
    {
        ran_.store(true, std::memory_order_relaxed);
        const bool suppressed = service_->isShuttingDown() && !context_->cleanup;
        if (!suppressed && !context_->cancelled.load(std::memory_order_relaxed)) {
            TikTokNetworkCancel cancelScope(&context_->cancelled);
            work_(context_);
        }
        service_->finishJob(context_, std::move(done_));
    }

    ~Runnable() override
    {
        // Fires for queued runnables dropped by QThreadPool::clear() at
        // shutdown; their completions must never run.
        if (!ran_.load(std::memory_order_relaxed))
            service_->abandonJob(context_);
    }

private:
    TikTokWorkerService *service_;
    std::shared_ptr<TikTokJobContext> context_;
    Work work_;
    Done done_;
    std::atomic<bool> ran_{false};
};

TikTokWorkerService::TikTokWorkerService(QObject *parent)
    : QObject(parent)
{
    normalPool_.setMaxThreadCount(4);
    cleanupPool_.setMaxThreadCount(2);
}

TikTokWorkerService &TikTokWorkerService::instance()
{
    // Static lifetime is intentional, but safety never relies on it:
    // shutdown() joins every worker thread before module unload.
    static TikTokWorkerService *service = [] {
        auto *created = new TikTokWorkerService();
        // Done callbacks must land on the main thread even if the first
        // submit races in from an output signal thread. This runs once,
        // on the creating thread, so the move is legal.
        QCoreApplication *app = QCoreApplication::instance();
        if (app != nullptr && created->thread() != app->thread())
            created->moveToThread(app->thread());
        return created;
    }();
    return *service;
}

std::shared_ptr<TikTokJobContext> TikTokWorkerService::submit(QObject *receiver, Work work,
                                                             Done done, bool activity, bool cleanup)
{
    if (shuttingDown_.load(std::memory_order_relaxed))
        return nullptr;
    auto context = std::make_shared<TikTokJobContext>(receiver, activity, cleanup);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shuttingDown_.load(std::memory_order_relaxed))
            return nullptr;
        pending_.insert(context);
    }
    auto *job = new Runnable(this, context, std::move(work), std::move(done));
    (cleanup ? cleanupPool_ : normalPool_).start(job);
    return context;
}

void TikTokWorkerService::cancelReceiver(QObject *receiver)
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &context : pending_) {
        if (!context->cleanup && context->receiver == receiver)
            context->orphaned.store(true, std::memory_order_relaxed);
    }
}

bool TikTokWorkerService::hasActiveActivity() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto &context : pending_) {
        // Cancelled jobs still count until their worker finishes.
        if (context->activity)
            return true;
    }
    return false;
}

void TikTokWorkerService::finishJob(const std::shared_ptr<TikTokJobContext> &context, Done done)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.erase(context);
    }
    if (!done)
        return;
    if (shuttingDown_.load(std::memory_order_relaxed))
        return;
    if (context->cancelled.load(std::memory_order_relaxed))
        return;
    if (context->orphaned.load(std::memory_order_relaxed))
        return;
    QMetaObject::invokeMethod(
        this,
        [context, done = std::move(done)]() {
            if (context->cancelled.load(std::memory_order_relaxed))
                return;
            if (context->orphaned.load(std::memory_order_relaxed))
                return;
            if (context->receiver.isNull())
                return;
            done(context);
        },
        Qt::QueuedConnection);
}

void TikTokWorkerService::abandonJob(const std::shared_ptr<TikTokJobContext> &context)
{
    std::lock_guard<std::mutex> lock(mutex_);
    pending_.erase(context);
}

void TikTokWorkerService::shutdown()
{
    bool expected = false;
    if (!shuttingDown_.compare_exchange_strong(expected, true)) {
        normalPool_.waitForDone(-1);
        cleanupPool_.waitForDone(-1);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto &context : pending_) {
            if (!context->cleanup)
                context->cancelled.store(true, std::memory_order_relaxed);
        }
    }
    normalPool_.clear();
    // Joins are bounded in practice: normal jobs abort promptly via
    // TikTokNetworkCancel, and every remaining operation carries its own
    // timeout (curl) or bus timeout (credential store).
    normalPool_.waitForDone(-1);
    cleanupPool_.waitForDone(-1);
}
