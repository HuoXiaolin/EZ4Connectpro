#ifndef MAINWINDOWCOORDINATOR_H
#define MAINWINDOWCOORDINATOR_H

#include <QObject>
#include <QHash>

#include "application/vpnprofile.h"

class AuthDialogCoordinator;
class ConnectionSession;
class ConnectionUiController;
class ProfileService;
class SystemProxySession;
class QSettings;
class UpdateChecker;
class QWidget;

struct ProfileConnection : QObject
{
    using QObject::QObject;

    QSettings *settings = nullptr;
    ConnectionSession *session = nullptr;
    AuthDialogCoordinator *auth = nullptr;
    ConnectionUiController *controls = nullptr;
    QString profileId;
    IntranetRouting routing = IntranetRouting::LocalProxy;
    bool interceptsDns = false;
    int socksPort = 0;
    int httpPort = 0;
};

class MainWindowCoordinator : public QObject
{
    Q_OBJECT

public:
    MainWindowCoordinator(
        QWidget *parentWidget,
        const QString &overrideConfigPath,
        QObject *parent = nullptr
    );

    ProfileService *profiles() const;
    ProfileConnection *connectionFor(const QString &settingsPath) const;
    ProfileConnection *createConnection(const VpnProfile &profile);
    void forgetConnection(const QString &settingsPath);
    bool hasActiveConnections() const;
    ProfileNetworkPlan reserveNetwork(ProfileConnection *connection);
    void stopConnections();
    SystemProxySession *systemProxy() const;
    UpdateChecker *updates() const;

    void prepareForShutdown();

private:
    QWidget *parentWidget;
    ProfileService *profileService;
    SystemProxySession *systemProxySession;
    UpdateChecker *updateChecker;
    QHash<QString, ProfileConnection *> connections;
};

#endif // MAINWINDOWCOORDINATOR_H
