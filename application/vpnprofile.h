#ifndef VPNPROFILE_H
#define VPNPROFILE_H

#include <QMap>
#include <QString>

#include "core/connectionprofile.h"

class QSettings;

struct VpnProfile
{
    QString id;
    QString name;
    QString protocolId;
    QString server;
    int port = 443;
    QString settingsPath;
    int socksPort = 11080;
    int httpPort = 11081;

    QString protocolName() const;
    bool isSupported() const;
};

const QMap<QString, QString> &vpnProtocolNames();
QString vpnProtocolId(const QSettings &settings);
VpnProfile readVpnProfile(const QSettings &settings, const QString &id);
void writeVpnProfile(QSettings &settings, const VpnProfile &profile);
bool tunEnabled(const QSettings &settings);
bool advancedNetworkOptionsEnabled(const QSettings &settings);
bool automaticIntranetRoutesEnabled(const QSettings &settings);
IntranetRouting preferredIntranetRouting(const QSettings &settings);

struct ProfileNetworkPlan
{
    IntranetRouting routing = IntranetRouting::LocalProxy;
    QString blockedReason;
    QString tunAdapterId;
};

#endif // VPNPROFILE_H
