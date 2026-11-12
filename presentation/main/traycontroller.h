#ifndef TRAYCONTROLLER_H
#define TRAYCONTROLLER_H

#include <QHash>
#include <QIcon>
#include <QList>
#include <QObject>

#include "application/vpnprofile.h"
#include "core/connectionsessionstate.h"

class QAction;
class QMenu;
class QSystemTrayIcon;
class QWidget;

struct TrayConnection
{
    VpnProfile profile;
    ConnectionState state = ConnectionState::Disconnected;
    IntranetRouting routing = IntranetRouting::LocalProxy;
    bool ready = false;
    bool ownsSystemProxy = false;

    bool isActive() const;
    QString statusText() const;
    QString channelText() const;
};

class TrayController : public QObject
{
    Q_OBJECT

public:
    explicit TrayController(QWidget *parent);
    void updateConnections(const QList<TrayConnection> &connections, const QString &selectedPath,
                           bool quitting, bool proxyEnabled, bool proxyBusy);
    QSystemTrayIcon *icon() const;
    QMenu *menu() const;

signals:
    void showWindowRequested();
    void connectionRequested(const QString &settingsPath, bool connect);
    void showProfileRequested(const QString &settingsPath);
    void disconnectAllRequested();
    void newProfileRequested();
    void importRequested();
    void exportRequested();
    void startupSettingsRequested();
    void openLogRequested();
    void releaseProxyRequested();
    void quitRequested();

private:
    struct Row
    {
        QMenu *menu;
        QAction *details;
        QAction *endpoint;
        QAction *ports;
        QAction *operate;
        QAction *show;
    };
    void refresh();
    void rebuildRows();
    QString tooltip() const;
    const TrayConnection *connection(const QString &path) const;

    QSystemTrayIcon *trayIcon;
    QMenu *trayMenu;
    QAction *summary;
    QAction *rowsEnd;
    QAction *disconnectAll;
    QAction *configuration;
    QAction *exportAction;
    QAction *startupAction;
    QAction *releaseProxy;
    QAction *quitAction;
    QHash<QString, Row> rows;
    QStringList rowPaths;
    QList<TrayConnection> connections;
    QString selectedPath;
    QIcon idleIcon;
    QIcon pendingIcon;
    QIcon connectedIcon;
    int displayedBadge = -1;
    bool quitting = false;
    bool proxyEnabled = false;
    bool proxyBusy = false;
};

#endif // TRAYCONTROLLER_H
