#include "platformsystemproxybackend.h"

#include "infrastructure/platform/systemproxyfunctions.h"

#if defined(Q_OS_WIN)
#include <QSettings>
#include <windows.h>
#include <wininet.h>

namespace
{
constexpr auto internetSettings = R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Internet Settings)";

std::optional<WindowsProxyState> readWindowsProxy()
{
    INTERNET_PER_CONN_OPTIONW options[4]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS_UI;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    INTERNET_PER_CONN_OPTION_LISTW list{sizeof(list), nullptr, 4, 0, options};
    DWORD bytes = sizeof(list);
    const bool read = InternetQueryOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, &bytes);
    WindowsProxyState state;
    state.flags = options[0].Value.dwValue;
    QString *strings[]{&state.server, &state.bypass, &state.pac};
    for (int i = 0; i < 3; ++i)
    {
        if (wchar_t *value = options[i + 1].Value.pszValue)
        {
            *strings[i] = QString::fromWCharArray(value);
            GlobalFree(value);
        }
    }
    return read ? std::optional<WindowsProxyState>{state} : std::nullopt;
}

bool writeWindowsProxy(const WindowsProxyState &state)
{
    INTERNET_PER_CONN_OPTIONW options[4]{};
    options[0].dwOption = INTERNET_PER_CONN_FLAGS;
    options[0].Value.dwValue = state.flags;
    options[1].dwOption = INTERNET_PER_CONN_PROXY_SERVER;
    options[2].dwOption = INTERNET_PER_CONN_PROXY_BYPASS;
    options[3].dwOption = INTERNET_PER_CONN_AUTOCONFIG_URL;
    const QString *strings[]{&state.server, &state.bypass, &state.pac};
    for (int i = 0; i < 3; ++i)
    {
        options[i + 1].Value.pszValue = reinterpret_cast<wchar_t *>(const_cast<ushort *>(strings[i]->utf16()));
    }
    INTERNET_PER_CONN_OPTION_LISTW list{sizeof(list), nullptr, 4, 0, options};
    if (!InternetSetOptionW(nullptr, INTERNET_OPTION_PER_CONNECTION_OPTION, &list, sizeof(list)))
    {
        return false;
    }
    InternetSetOptionW(nullptr, INTERNET_OPTION_SETTINGS_CHANGED, nullptr, 0);
    InternetSetOptionW(nullptr, INTERNET_OPTION_REFRESH, nullptr, 0);
    return true;
}

bool occupied(const WindowsProxyState &state)
{
    const QSettings registry(internetSettings, QSettings::NativeFormat);
    return (state.flags & (PROXY_TYPE_PROXY | PROXY_TYPE_AUTO_PROXY_URL)) != 0
        || !state.pac.isEmpty() || registry.value("ProxyEnable").toBool()
        || !registry.value("AutoConfigURL").toString().trimmed().isEmpty();
}

bool stillOwned(const WindowsProxyState &state, const WindowsProxyState &owned)
{
    const QSettings registry(internetSettings, QSettings::NativeFormat);
    return state.flags == owned.flags && state.server == owned.server && state.bypass == owned.bypass
        && state.pac == owned.pac && registry.value("ProxyEnable", true).toBool()
        && registry.value("ProxyServer", owned.server).toString() == owned.server
        && registry.value("ProxyOverride", owned.bypass).toString() == owned.bypass
        && registry.value("AutoConfigURL", owned.pac).toString() == owned.pac;
}
}
#endif

bool PlatformSystemProxyBackend::hasConflict(const SystemProxyConfig &config)
{
    Q_UNUSED(config)
#if defined(Q_OS_WIN)
    const auto current = readWindowsProxy();
    return !current || occupied(*current);
#else
    return PlatformSystemProxy::isSet();
#endif
}

bool PlatformSystemProxyBackend::apply(const SystemProxyConfig &config)
{
#if defined(Q_OS_WIN)
    const auto current = readWindowsProxy();
    if (!current || occupied(*current) || config.httpPort < 1 || config.httpPort > 65535)
    {
        return false;
    }
    WindowsProxyState desired = *current;
    desired.flags = PROXY_TYPE_DIRECT | PROXY_TYPE_PROXY;
    desired.server = "127.0.0.1:" + QString::number(config.httpPort);
    desired.bypass = config.bypass;
    if (!writeWindowsProxy(desired))
    {
        return false;
    }
    previous = current;
    applied = desired;
#else
    if (hasConflict(config))
    {
        return false;
    }
    PlatformSystemProxy::set(config.httpPort, config.socksPort, config.bypass);
    applied = config;
#endif
    return true;
}

bool PlatformSystemProxyBackend::clear()
{
    if (!applied)
    {
        return true;
    }
#if defined(Q_OS_WIN)
    const auto current = readWindowsProxy();
    if (!current)
    {
        return false;
    }
    if (stillOwned(*current, *applied) && !writeWindowsProxy(*previous))
    {
        return false;
    }
    previous.reset();
#else
    if (PlatformSystemProxy::isSet() && !PlatformSystemProxy::isSet(applied->httpPort, applied->socksPort))
    {
        PlatformSystemProxy::clear();
    }
#endif
    applied.reset();
    return true;
}
