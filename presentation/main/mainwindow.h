#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QSystemTrayIcon>
#include <QCloseEvent>
#include <QSettings>
#include <QPointer>

#include "application/connectionsession.h"
#include "application/profileservice.h"
#include "application/systemproxysession.h"
#include "presentation/dialogs/settingwindow/settingwindow.h"
#include "presentation/coordinators/authdialogcoordinator.h"
#include "presentation/main/mainwindowcoordinator.h"

namespace Ui
{
    class MainWindow;
}

class ApplicationLogger;
class ApplicationLogFile;
class TrayController;
class UpdateChecker;

class MainWindow : public QMainWindow
{
Q_OBJECT

public:
    explicit MainWindow(
        ApplicationLogger *logger,
        ApplicationLogFile *logFile,
        QWidget *parent = nullptr
    );

    ~MainWindow() override;

public slots:

    void cleanUpWhenQuit();

protected:
    void closeEvent(QCloseEvent *e) override;

    void changeEvent(QEvent *event) override;

private:
    void upgradeSettings(bool interactive = true);

    void clearLog();

    void resetConnectionUi();
    void setupWorkspace();
    void refreshProfileList();
    void editSelectedProfile();
    void startSelectedProfile();
    void startProfile(const VpnProfile &profile);
    void startStartupProfiles(const QStringList &profileIds);
    ProfileConnection *selectedConnection() const;
    bool selectedConnectionIsActive() const;

    void applyColorScheme(Qt::ColorScheme scheme);

    void updateConnectionState(ConnectionState state);

    void updateProfileSummary();

    void showNotification(
        const QString &title,
        const QString &content,
        QSystemTrayIcon::MessageIcon icon = QSystemTrayIcon::MessageIcon::Information
    );

    void updateVersionInfo();

    void setupTrayIcon();
    void updateTray();

    void restoreFromTray();

    void refreshProfiles();

    bool switchProfile(const QString &profileId);

    void createProfile();

    void importConfiguration();

    void exportConfiguration();

    void showStartupSettings();

    void openConfigurationGuide();

    void promptConfigurationGuide(
        const QString &windowTitle,
        const QString &text
    );

    void promptFirstLaunchGuide();

    void renameCurrentProfile();

    void deleteCurrentProfile();

    void clearLoginCache();

    void setDeviceTrust(bool trusted);

    void gracefullyQuit();

    Ui::MainWindow *ui;
    QSystemTrayIcon *trayIcon;
    TrayController *trayController;
    bool manualUpdateCheck = false;
    SystemProxySession *systemProxySession = nullptr;
    MainWindowCoordinator *coordinator;
    ApplicationLogger *applicationLogger;
    ApplicationLogFile *applicationLogFile;
    UpdateChecker *updateChecker;
    QSettings *settings;
    ProfileService *profileService;
    QString currentProfileId;
    QList<VpnProfile> profileEntries;
    bool quitting = false;

    QPointer<SettingWindow> settingWindow;

};

#endif //MAINWINDOW_H
