#ifndef CONNECTIONUICONTROLLER_H
#define CONNECTIONUICONTROLLER_H

#include <QObject>
#include <QSystemTrayIcon>

#include <functional>

#include "application/vpnprofile.h"
#include "core/connectionerror.h"

class ApplicationLogger;
class AuthDialogCoordinator;
class ConnectionSession;
class QSettings;
class SystemProxySession;
class QWidget;

class ConnectionUiController : public QObject
{
    Q_OBJECT

public:
    using SettingsProvider = std::function<QSettings *()>;
    using ProfileIdProvider = std::function<QString()>;
    using StartPermission = std::function<ProfileNetworkPlan()>;
    using NotificationHandler = std::function<void(
        const QString &title,
        const QString &content,
        QSystemTrayIcon::MessageIcon icon
    )>;

    ConnectionUiController(
        QWidget *parentWidget,
        ConnectionSession *connectionSession,
        SystemProxySession *systemProxySession,
        AuthDialogCoordinator *authenticationDialogs,
        ApplicationLogger *applicationLogger,
        SettingsProvider settingsProvider,
        ProfileIdProvider profileIdProvider,
        StartPermission startPermission,
        NotificationHandler notificationHandler,
        QObject *parent = nullptr
    );

    void handleConnectClicked();
    void handleProxyClicked();

private:
    bool checkStartPermission(const ProfileNetworkPlan &plan);
    void startConnection(
        const QString &username,
        const QString &password,
        const QString &phone = QString()
    );
    void showConnectionError(ZJU_ERROR error);
    QSettings *settings() const;

    QWidget *parentWidget;
    ConnectionSession *connectionSession;
    SystemProxySession *systemProxySession;
    AuthDialogCoordinator *authenticationDialogs;
    SettingsProvider settingsProvider;
    ProfileIdProvider profileIdProvider;
    StartPermission startPermission;
    NotificationHandler notificationHandler;
};

#endif // CONNECTIONUICONTROLLER_H
