#ifndef BROWSERLOGINCACHE_H
#define BROWSERLOGINCACHE_H

#include <QJsonArray>
#include <QString>
#include <QUrl>

// Only cookies from the application's isolated login browser are stored here.
// The user's everyday browser profile is never read or modified.
namespace BrowserLoginCache
{
QJsonArray load(const QString &file, const QUrl &gateway);
bool save(const QString &file, const QUrl &gateway, const QJsonArray &cookies);
QJsonArray cookieParameters(const QJsonArray &cookies);
// Read only cookies belonging to this VPN origin for the authenticated resource page.
QJsonArray gatewaySessionCookies(const QString &clientDataFile, const QUrl &gateway);
}

#endif
