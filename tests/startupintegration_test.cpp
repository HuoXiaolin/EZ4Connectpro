#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTimer>
#include <cstdio>

#include "application/applicationconstants.h"
#include "application/applicationlogger.h"
#include "application/commandlineoptions.h"
#include "infrastructure/logging/applicationlogfile.h"
#include "infrastructure/platform/autostart.h"
#include "infrastructure/platform/privileges.h"
#include "infrastructure/settings/profilemanager.h"
#include "infrastructure/storage/applicationpaths.h"
#include "presentation/main/mainwindow.h"
#include "presentation/main/traycontroller.h"

namespace
{
QString scenario;
int elevationCalls = 0;
bool elevatedBatch = false;
bool elevatedSilent = false;
bool registered = true;
int registrationWrites = 0;
}

// Exercise the real startup UI and connection coordinator without touching UAC or Run.
bool Privileges::isElevated() { return false; }
bool Privileges::relaunchElevated(const QString &, bool silent, bool startupBatch)
{
    ++elevationCalls;
    elevatedBatch = startupBatch;
    elevatedSilent = silent;
    return scenario == "elevate";
}
void Privileges::waitForPreviousInstance(int, char **) {}
AutoStart::RegistrationStatus AutoStart::registrationStatus()
{
    return registered ? RegistrationStatus::Registered : RegistrationStatus::Missing;
}
bool AutoStart::setEnabled(bool enabled)
{
    ++registrationWrites;
    registered = enabled;
    return true;
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("EZ4Connectpro");
    app.setApplicationDisplayName("EZ4Connectpro");
    app.setApplicationVersion(PROJ_VER);
    scenario = CommandLineOptions::value(app.arguments(), "--scenario");
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    ApplicationPaths::setProfileRoot(directory.path());
    ProfileManager storage;
    storage.createProfile("office");
    storage.createProfile("manual");
    storage.createProfile("lab");
    const QStringList ids{QString{}, "office", "manual", "lab"};
    QTcpServer ports[8];
    for (auto &port : ports) if (!port.listen(QHostAddress::LocalHost)) return 2;
    for (int index = 0; index < ids.size(); ++index)
    {
        QSettings settings(storage.profilePath(ids[index]), QSettings::IniFormat);
        settings.setValue("Common/ConfigVersion", ApplicationConstants::ConfigVersion);
        settings.setValue("Common/CheckUpdateAfterStart", false);
        settings.setValue("Common/AutoIntranetRoutes", false);
        settings.setValue("Common/ConnectAfterStart", true);
        settings.setValue("Profile/DisplayName", QStringList{"办公线路", "备用线路", "手动线路", "测试线路"}[index]);
        settings.setValue("ZJUConnect/Protocol", "atrust");
        settings.setValue("ZJUConnect/AuthType", "psw");
        settings.setValue("ZJUConnect/ServerAddress", "office.example");
        settings.setValue("ZJUConnect/ServerPort", 443);
        settings.setValue("ZJUConnect/TunEnabled", index == 0 && (scenario == "elevate" || scenario == "denied"));
        const int bindIndex = scenario == "conflict" && index == 1 ? 0 : index * 2;
        settings.setValue("ZJUConnect/SOCKS5Port", ports[bindIndex].serverPort());
        settings.setValue("ZJUConnect/HTTPPort", ports[bindIndex + 1].serverPort());
        settings.setValue("Credential/Username", "fixture");
        settings.setValue("Credential/Password", "Zml4dHVyZQ==");
    }
    for (auto &port : ports) port.close();
    QStringList selected{QString{}, "office", "missing"};
    if (scenario == "empty") selected.clear();
    if (scenario == "conflict") selected << "lab";
    if (scenario == "ui") selected = QStringList{QString{}};
    if (!storage.saveStartupOptions({scenario != "disabled", true, selected})
        || !storage.setActiveProfile("manual")) return 2;
    ApplicationLogger logger;
    ApplicationLogFile logfile(directory.filePath("test.log"));
    MainWindow window(&logger, &logfile);
    auto *coordinator = window.findChild<MainWindowCoordinator *>();
    if (!coordinator) return 2;
    QObject::connect(&app, &QApplication::aboutToQuit, &window, &MainWindow::cleanUpWhenQuit);
    const auto ready = [&](const QString &id)
    {
        const auto *runtime = coordinator->connectionFor(storage.profilePath(id));
        return runtime && runtime->session->isHttpProxyReady();
    };
    bool verified = false;
    int conflictDialogs = 0;
    int cleanShutdowns = 0;
    int ticks = 0;
    QTimer poll;
    poll.setInterval(30);
    QObject::connect(&poll, &QTimer::timeout, [&]()
    {
        ++ticks;
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()))
        {
            if (scenario != "conflict" || message->windowTitle() != "无法启动配置") qFatal("Unexpected startup dialog");
            ++conflictDialogs;
            message->accept();
        }
        if (verified)
        {
            if (!coordinator->hasActiveConnections()) app.quit();
            return;
        }
        if (scenario == "ui" || scenario == "elevate") return;
        if (scenario == "manual" || scenario == "empty" || scenario == "disabled")
        {
            if (ticks < 15) return;
            if (coordinator->hasActiveConnections() || elevationCalls) qFatal("Unrequested startup connection");
        }
        else if (scenario == "denied")
        {
            if (!ready("office")) return;
            if (coordinator->connectionFor(storage.profilePath({})) || elevationCalls != 1 || !elevatedBatch)
                qFatal("Denied batch retried UAC or started a TUN core");
        }
        else if (scenario == "conflict")
        {
            if (!ready({}) || !ready("lab") || !conflictDialogs) return;
            const auto *blocked = coordinator->connectionFor(storage.profilePath("office"));
            if (!blocked || blocked->session->isActive()) qFatal("Conflicting startup line was allowed");
        }
        else
        {
            if (!ready({}) || !ready("office")) return;
        }
        if (storage.activeProfile() != "manual" || ready("manual")) qFatal("Startup changed or connected the unrelated selected profile");
        verified = true;
        if (scenario == "shutdown")
        {
            for (const QString id : {QString{}, QString("office")})
            {
                auto *runtime = coordinator->connectionFor(storage.profilePath(id));
                auto *core = runtime->session->findChild<CoreProcess *>();
                if (!core) qFatal("Shutdown fixture core missing");
                QObject::connect(runtime->session, &ConnectionSession::stateChanged, core, [core](ConnectionState state)
                {
                    if (state == ConnectionState::Stopping)
                    {
                        emit core->outputRead("invalid SID during fixture shutdown");
                        emit core->error(ZJU_ERROR::AUTH_EXPIRED, "invalid SID during fixture shutdown");
                    }
                });
                QObject::connect(runtime->session, &ConnectionSession::finished, [&](ZJU_ERROR error)
                {
                    if (error != ZJU_ERROR::NONE) qFatal("Intentional shutdown was reported as authentication expiry");
                    ++cleanShutdowns;
                });
            }
            auto *tray = window.findChild<TrayController *>();
            if (!tray) qFatal("Shutdown tray controller missing");
            emit tray->quitRequested();
            return;
        }
        coordinator->stopConnections();
    });
    if (scenario == "ui")
    {
        QTimer::singleShot(0, [&]()
        {
            QTimer::singleShot(50, [&]()
            {
                auto *dialog = QApplication::activeModalWidget();
                auto *list = dialog ? dialog->findChild<QListWidget *>("startupProfilesList") : nullptr;
                if (!list || list->count() != 4) qFatal("Startup multi-select list missing");
                for (int i = 0; i < list->count(); ++i)
                {
                    auto *item = list->item(i);
                    const auto id = item->data(Qt::UserRole).toString();
                    if (id.isEmpty() && item->checkState() != Qt::Checked) qFatal("Saved default profile selection was lost");
                    item->setCheckState(id.isEmpty() || id == "office" ? Qt::Checked : Qt::Unchecked);
                }
                const QString capture = qEnvironmentVariable("EZ4_STARTUP_CAPTURE");
                if (!capture.isEmpty() && !dialog->grab().save(capture)) qFatal("Could not capture startup settings");
                dialog->findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->click();
            });
            window.findChild<QAction *>("startupSettingsAction")->trigger();
            const auto saved = storage.startupOptions();
            if (saved.profileIds != QStringList{QString{}, "office"} || !saved.enabled || !saved.silent || registrationWrites)
                qFatal("Startup UI did not persist both selections independently of registration");
            verified = true;
        });
    }
    QTimer::singleShot(10000, [&]() { qFatal("Startup scenario timed out: %s", qPrintable(scenario)); });
    poll.start();
    const int result = app.exec();
    if (scenario == "shutdown" && (cleanShutdowns != 2 || coordinator->hasActiveConnections()))
        qFatal("Application exited without completing both shutdowns");
    if (scenario == "elevate")
    {
        verified = elevationCalls == 1 && elevatedBatch && !coordinator->hasActiveConnections()
            && elevatedSilent == QSystemTrayIcon::isSystemTrayAvailable()
            && storage.startupOptions().profileIds == selected;
    }
    if (!verified || result) return 1;
    std::printf("Startup scenario %s passed; no real registry or UAC mutation.\n", qPrintable(scenario));
    return 0;
}
