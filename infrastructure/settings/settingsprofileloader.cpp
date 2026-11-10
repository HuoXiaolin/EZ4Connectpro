#include "settingsprofileloader.h"

#include "application/vpnprofile.h"

ConnectionProfile SettingsProfileLoader::load(
    const QSettings &settings,
    const QString &profileId,
    const QString &username,
    const QString &password,
    IntranetRouting routing
)
{
    ConnectionProfile profile;
    profile.profileId = profileId;
    const QString protocol = vpnProtocolId(settings);
    const bool automaticRoutes = routing == IntranetRouting::Automatic;
    const bool useTun = routing != IntranetRouting::LocalProxy;
    const QString easyconnectAuthType = settings.value(
        "ZJUConnect/EasyConnectAuthType",
        settings.value("Credential/CertFile", "").toString().isEmpty()
            ? "password"
            : "certificate"
    ).toString();
    const bool useCertificate =
        protocol == "easyconnect" && easyconnectAuthType == "certificate";
    profile.credentials = {
        username,
        password,
        settings.value("Credential/TOTPSecret").toString(),
        useCertificate
            ? settings.value("Credential/CertFile", "").toString()
            : QString(),
        useCertificate
            ? QByteArray::fromBase64(
                  settings.value("Credential/CertPassword", "").toByteArray()
              )
            : QString(),
        settings.value("ZJUConnect/CredentialsAsArguments", false).toBool()
    };

    const QString countryCode = settings.value("ZJUConnect/PhoneCountryCode").toString();
    const QString phoneNumber = settings.value("ZJUConnect/PhoneNumber").toString();
    const QString phone = !countryCode.isEmpty() && !phoneNumber.isEmpty()
        ? countryCode + "-" + phoneNumber
        : QString();
    profile.endpoint = {
        protocol,
        settings.value("ZJUConnect/AuthType").toString(),
        settings.value("ZJUConnect/LoginDomain").toString(),
        phone,
        settings.value("ZJUConnect/ServerAddress").toString(),
        settings.value("ZJUConnect/ServerPort").toInt()
    };

    profile.dns = {
        settings.value("ZJUConnect/DNS").toString(),
        settings.value("ZJUConnect/DNSAuto").toBool(),
        settings.value("ZJUConnect/SecondaryDNS").toString(),
        settings.value("ZJUConnect/DNSTTL").toInt(),
        settings.value("ZJUConnect/DisableZJUDNS").toBool(),
        settings.value("ZJUConnect/CustomDNS", "").toString(),
        settings.value("ZJUConnect/LocalDNSServer", "").toString(),
        settings.value("ZJUConnect/DNSServerBind", "").toString()
    };

    const QString bindPrefix = settings.value("ZJUConnect/OutsideAccess", false).toBool()
        ? "[::]:"
        : "127.0.0.1:";
    profile.proxy = {
        bindPrefix + QString::number(settings.value("ZJUConnect/SOCKS5Port", 11080).toInt()),
        bindPrefix + QString::number(settings.value("ZJUConnect/HTTPPort", 11081).toInt()),
        settings.value("ZJUConnect/ShadowsocksURL").toString(),
        settings.value("ZJUConnect/DialDirectProxy").toString(),
        settings.value("ZJUConnect/ProxyAll").toBool(),
        settings.value("ZJUConnect/CustomProxyDomain", "").toString()
    };

    profile.tunnel = {
        useTun,
        automaticRoutes || (useTun && settings.value("ZJUConnect/AddRoute").toBool()),
        useTun && settings.value("ZJUConnect/DNSHijack").toBool(),
        useTun && settings.value("ZJUConnect/FakeIP").toBool(),
        !automaticRoutes && settings.value("ZJUConnect/TCPTunnelMode").toBool(),
        settings.value("ZJUConnect/TCPPortForwarding").toString(),
        settings.value("ZJUConnect/UDPPortForwarding").toString(),
        automaticRoutes
    };

    profile.behavior = {
        settings.value("ZJUConnect/UpdateBestNodesInterval", 300).toInt(),
        !settings.value("ZJUConnect/MultiLine").toBool(),
        !settings.value("ZJUConnect/KeepAlive").toBool(),
        settings.value("ZJUConnect/KeepAliveURL", "").toString(),
        settings.value("ZJUConnect/BindInterface", "").toString(),
        settings.value("ZJUConnect/AutoDetectInterface", false).toBool(),
        settings.value("ZJUConnect/SkipDomainResource").toBool(),
        !automaticRoutes && settings.value("ZJUConnect/DisableServerConfig").toBool(),
        !automaticRoutes && !settings.value("ZJUConnect/ZJUDefault").toBool()
    };

    profile.debug = {
        settings.value("ZJUConnect/Debug").toBool(),
        settings.value("ZJUConnect/DebugPCAP", false).toBool(),
        settings.value("ZJUConnect/DebugTLSLog", false).toBool()
    };

    profile.extraArguments = settings.value("ZJUConnect/ExtraArguments", "").toString();
    if (!advancedNetworkOptionsEnabled(settings))
    {
        profile.proxy.socksBind = "127.0.0.1:" + QString::number(settings.value("ZJUConnect/SOCKS5Port", 11080).toInt());
        profile.proxy.httpBind = "127.0.0.1:" + QString::number(settings.value("ZJUConnect/HTTPPort", 11081).toInt());
        profile.proxy.shadowsocksUrl.clear();
        profile.proxy.dialDirectProxy.clear();
        profile.dns.localDnsServer.clear();
        profile.dns.dnsServerBind.clear();
        profile.tunnel.tcpTunnelMode = false;
        profile.tunnel.tcpPortForwarding.clear();
        profile.tunnel.udpPortForwarding.clear();
        profile.extraArguments.clear();
    }
    return profile;
}
