#include "vpnprofile.h"

#include <QSettings>

const QMap<QString, QString> &vpnProtocolNames()
{
    static const QMap<QString, QString> protocols{{"atrust", "aTrust"}, {"easyconnect", "EasyConnect"}};
    return protocols;
}

QString vpnProtocolId(const QSettings &settings)
{
    const QString previousAdapter = settings.value("Profile/Adapter").toString().trimmed().toLower();
    if (!previousAdapter.isEmpty() && !vpnProtocolNames().contains(previousAdapter))
    {
        return {};
    }
    const QString fallback = previousAdapter.isEmpty() ? QStringLiteral("easyconnect") : previousAdapter;
    return settings.value("ZJUConnect/Protocol", fallback).toString().trimmed().toLower();
}

VpnProfile readVpnProfile(const QSettings &settings, const QString &id)
{
    VpnProfile profile;
    profile.id = id;
    profile.settingsPath = settings.fileName();
    profile.name = settings.value("Profile/DisplayName").toString().trimmed();
    if (profile.name.isEmpty())
    {
        profile.name = id.isEmpty() ? QStringLiteral("默认配置") : id;
    }
    profile.protocolId = vpnProtocolId(settings);
    profile.server = settings.value("ZJUConnect/ServerAddress").toString().trimmed();
    profile.port = settings.value("ZJUConnect/ServerPort", 443).toInt();
    profile.socksPort = settings.value("ZJUConnect/SOCKS5Port", 11080).toInt();
    profile.httpPort = settings.value("ZJUConnect/HTTPPort", 11081).toInt();
    return profile;
}

QString VpnProfile::protocolName() const
{
    return vpnProtocolNames().value(protocolId);
}

bool VpnProfile::isSupported() const
{
    return vpnProtocolNames().contains(protocolId);
}

void writeVpnProfile(QSettings &settings, const VpnProfile &profile)
{
    settings.setValue("Profile/DisplayName", profile.name.trimmed());
    settings.remove("Profile/Adapter");
    settings.setValue("ZJUConnect/Protocol", profile.protocolId);
    settings.setValue("ZJUConnect/ServerAddress", profile.server.trimmed());
    settings.setValue("ZJUConnect/ServerPort", profile.port);
}

bool tunEnabled(const QSettings &settings)
{
    const bool previousBehavior = automaticIntranetRoutesEnabled(settings)
        || settings.value("ZJUConnect/EnableTun", false).toBool();
    return settings.value("ZJUConnect/TunEnabled", previousBehavior).toBool();
}

bool advancedNetworkOptionsEnabled(const QSettings &settings)
{
    return settings.contains("ZJUConnect/TunEnabled") || settings.contains("ZJUConnect/EnableTun")
        || settings.value("Common/ProxyMode").toString() == "standalone";
}

bool automaticIntranetRoutesEnabled(const QSettings &settings)
{
#if defined(Q_OS_WIN)
    constexpr bool defaultEnabled = true;
#else
    constexpr bool defaultEnabled = false;
#endif
    return settings.value("Common/AutoIntranetRoutes", defaultEnabled).toBool();
}

IntranetRouting preferredIntranetRouting(const QSettings &settings)
{
    if (!tunEnabled(settings))
    {
        return IntranetRouting::LocalProxy;
    }
    return automaticIntranetRoutesEnabled(settings) ? IntranetRouting::Automatic : IntranetRouting::Configured;
}
