#include "systemproxysession.h"

#include <QtConcurrent>

#include <utility>

SystemProxySession::SystemProxySession(
    std::unique_ptr<SystemProxyBackend> backend,
    QObject *parent
)
    : QObject(parent),
      backend(std::move(backend))
{
    connect(&operationWatcher, &QFutureWatcher<OperationResult>::finished,
            this, &SystemProxySession::handleOperationFinished);
}

SystemProxySession::~SystemProxySession()
{
    operationWatcher.waitForFinished();
}

bool SystemProxySession::checkConflict(const SystemProxyConfig &config)
{
    return startOperation(Operation::CheckConflict, config);
}

bool SystemProxySession::enable(const SystemProxyConfig &config, const QString &owner)
{
    if (isBusy() || enabled)
    {
        return false;
    }
    ownerKey = owner;
    return startOperation(Operation::Enable, config);
}

bool SystemProxySession::disable()
{
    return enabled && startOperation(Operation::Disable);
}

void SystemProxySession::releaseFor(const QString &owner)
{
    if (!isOwnedBy(owner))
    {
        return;
    }
    if (isBusy())
    {
        releasePending = currentOperation != Operation::Disable;
    }
    else
    {
        disable();
    }
}

bool SystemProxySession::isOwnedBy(const QString &owner) const
{
    return !owner.isEmpty() && ownerKey == owner;
}

bool SystemProxySession::isEnabled() const
{
    return enabled;
}

bool SystemProxySession::isBusy() const
{
    return currentOperation != Operation::None;
}

void SystemProxySession::clearBeforeShutdown()
{
    const Operation operation = currentOperation;
    disconnect(&operationWatcher, nullptr, this, nullptr);
    operationWatcher.waitForFinished();

    const bool applied = operation == Operation::Enable && operationWatcher.result().succeeded;
    const bool cleared = operation == Operation::Disable && operationWatcher.result().succeeded;
    if (applied || (enabled && !cleared))
    {
        backend->clear();
    }

    currentOperation = Operation::None;
    enabled = false;
    ownerKey.clear();
    releasePending = false;
}

bool SystemProxySession::startOperation(Operation operation, const SystemProxyConfig &config)
{
    if (isBusy())
    {
        return false;
    }

    currentOperation = operation;

    SystemProxyBackend *proxyBackend = backend.get();
    operationWatcher.setFuture(QtConcurrent::run(
        [proxyBackend, operation, config]()
        {
            OperationResult result{operation};
            switch (operation)
            {
            case Operation::CheckConflict:
                result.conflict = proxyBackend->hasConflict(config);
                break;
            case Operation::Enable:
                result.succeeded = proxyBackend->apply(config);
                break;
            case Operation::Disable:
                result.succeeded = proxyBackend->clear();
                break;
            case Operation::None:
                break;
            }
            return result;
        }
    ));
    emit busyChanged(true);
    return true;
}

void SystemProxySession::handleOperationFinished()
{
    const OperationResult result = operationWatcher.result();
    const bool shouldRelease = std::exchange(releasePending, false);
    const QString completedOwner = ownerKey;
    currentOperation = Operation::None;

    const bool wasEnabled = enabled;
    if (result.operation == Operation::Enable)
    {
        enabled = result.succeeded;
    }
    else if (result.operation == Operation::Disable && result.succeeded)
    {
        enabled = false;
    }
    if (!enabled)
    {
        ownerKey.clear();
    }

    emit busyChanged(false);

    if (enabled != wasEnabled)
    {
        emit enabledChanged(enabled);
    }

    if (result.operation == Operation::CheckConflict)
    {
        emit conflictCheckFinished(result.conflict);
    }
    else
    {
        emit operationFinished(enabled);
    }
    if (shouldRelease)
    {
        releaseFor(completedOwner);
    }
}
