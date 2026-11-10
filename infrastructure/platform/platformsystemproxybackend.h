#ifndef PLATFORMSYSTEMPROXYBACKEND_H
#define PLATFORMSYSTEMPROXYBACKEND_H

#include "application/systemproxybackend.h"

#include <optional>

#if defined(Q_OS_WIN)
struct WindowsProxyState
{
    unsigned long flags = 0;
    QString server;
    QString bypass;
    QString pac;
};
#endif

class PlatformSystemProxyBackend : public SystemProxyBackend
{
public:
    bool hasConflict(const SystemProxyConfig &config) override;
    bool apply(const SystemProxyConfig &config) override;
    bool clear() override;

private:
#if defined(Q_OS_WIN)
    std::optional<WindowsProxyState> previous;
    std::optional<WindowsProxyState> applied;
#else
    std::optional<SystemProxyConfig> applied;
#endif
};

#endif // PLATFORMSYSTEMPROXYBACKEND_H
