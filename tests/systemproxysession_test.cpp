#include <functional>
#include <memory>

#include <QCoreApplication>
#include <QDebug>
#include <QElapsedTimer>
#include <QSemaphore>
#include <QThread>

#include "application/systemproxysession.h"

namespace
{
class FakeSystemProxyBackend : public SystemProxyBackend
{
public:
    bool conflict = false;
    int conflictChecks = 0;
    int applyCalls = 0;
    int clearCalls = 0;
    bool applySucceeds = true;
    bool clearSucceeds = true;
    SystemProxyConfig lastConfig;
    QSemaphore operationStarted;
    QSemaphore allowOperationToFinish;

    bool hasConflict(const SystemProxyConfig &config) override
    {
        ++conflictChecks;
        lastConfig = config;
        return conflict;
    }

    bool apply(const SystemProxyConfig &config) override
    {
        ++applyCalls;
        lastConfig = config;
        operationStarted.release();
        allowOperationToFinish.acquire();
        return applySucceeds;
    }

    bool clear() override
    {
        ++clearCalls;
        return clearSucceeds;
    }
};

bool waitUntil(const std::function<bool()> &condition)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < 1000)
    {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return condition();
}

bool delegatesPlatformOperationsAsynchronouslyAndTracksOwnedState()
{
    auto backend = std::make_unique<FakeSystemProxyBackend>();
    FakeSystemProxyBackend *fake = backend.get();
    fake->conflict = true;
    SystemProxySession session(std::move(backend));
    const SystemProxyConfig config{1081, 1080, "localhost"};

    bool conflictResult = false;
    QObject::connect(&session, &SystemProxySession::conflictCheckFinished,
                     [&](bool conflict) { conflictResult = conflict; });
    if (!session.checkConflict(config)
        || !waitUntil([&]() { return !session.isBusy(); })
        || !conflictResult
        || fake->conflictChecks != 1
        || session.isEnabled())
    {
        qCritical() << "delegatesPlatformOperationsAsynchronouslyAndTracksOwnedState failed at conflict check";
        return false;
    }

    if (!session.enable(config)
        || !fake->operationStarted.tryAcquire(1, 1000)
        || !session.isBusy()
        || session.isEnabled()
        || session.disable())
    {
        qCritical() << "delegatesPlatformOperationsAsynchronouslyAndTracksOwnedState failed while enabling";
        return false;
    }

    fake->allowOperationToFinish.release();
    if (!waitUntil([&]() { return !session.isBusy(); })
        || !session.isEnabled()
        || fake->applyCalls != 1
        || fake->lastConfig.httpPort != 1081
        || fake->lastConfig.socksPort != 1080
        || fake->lastConfig.bypass != "localhost")
    {
        qCritical() << "delegatesPlatformOperationsAsynchronouslyAndTracksOwnedState failed after enabling";
        return false;
    }

    if (!session.disable()
        || !waitUntil([&]() { return !session.isBusy(); })
        || session.isEnabled()
        || fake->clearCalls != 1)
    {
        qCritical() << "delegatesPlatformOperationsAsynchronouslyAndTracksOwnedState failed at disable";
        return false;
    }

    if (session.disable() || fake->clearCalls != 1)
    {
        qCritical() << "disable must leave externally-owned proxy state alone";
        return false;
    }

    fake->applySucceeds = false;
    if (!session.enable(config)
        || !fake->operationStarted.tryAcquire(1, 1000))
    {
        qCritical() << "failed enable did not start";
        return false;
    }
    fake->allowOperationToFinish.release();
    if (!waitUntil([&]() { return !session.isBusy(); }) || session.isEnabled())
    {
        qCritical() << "failed enable must not change owned state";
        return false;
    }
    fake->applySucceeds = true;

    if (!session.enable(config)
        || !fake->operationStarted.tryAcquire(1, 1000))
    {
        qCritical() << "clearBeforeShutdown failed to start enable";
        return false;
    }
    fake->allowOperationToFinish.release();
    session.clearBeforeShutdown();
    if (session.isBusy()
        || session.isEnabled()
        || fake->applyCalls != 3
        || fake->clearCalls != 2)
    {
        qCritical() << "clearBeforeShutdown must clear an in-flight enable";
        return false;
    }
    return true;
}

bool onlyTheOwningConnectionCanReleaseTheProxy()
{
    auto backend = std::make_unique<FakeSystemProxyBackend>();
    FakeSystemProxyBackend *fake = backend.get();
    SystemProxySession session(std::move(backend));
    const SystemProxyConfig config{1081, 1080, {}};
    if (!session.enable(config, "office"))
    {
        return false;
    }
    fake->allowOperationToFinish.release();
    if (!waitUntil([&]() { return !session.isBusy(); })
        || !session.isOwnedBy("office") || session.enable(config, "lab"))
    {
        return false;
    }
    session.releaseFor("lab");
    if (session.isBusy() || !session.isEnabled() || fake->clearCalls != 0)
    {
        return false;
    }
    session.releaseFor("office");
    if (!waitUntil([&]() { return !session.isBusy(); })
        || session.isEnabled() || session.isOwnedBy("office") || fake->clearCalls != 1)
    {
        return false;
    }

    if (!session.enable(config, "lab"))
    {
        return false;
    }
    session.releaseFor("lab");
    fake->allowOperationToFinish.release();
    return waitUntil([&]() { return !session.isBusy(); })
        && !session.isEnabled() && !session.isOwnedBy("lab") && fake->clearCalls == 2;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    return delegatesPlatformOperationsAsynchronouslyAndTracksOwnedState()
        && onlyTheOwningConnectionCanReleaseTheProxy() ? 0 : 1;
}
