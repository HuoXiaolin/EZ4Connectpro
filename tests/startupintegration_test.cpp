#include <QAction>
#include <QApplication>
#include <QDesktopServices>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QStyleHints>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QProcess>
#include <QTableWidget>
#include <QListWidget>
#include <QLocale>
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
#include "infrastructure/browser/browserssosession.h"
#include "infrastructure/browser/temporarybrowserssosession.h"
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

class ResourceUrlProbe : public QObject
{
    Q_OBJECT
public:
    QUrl url;
    int count = 0;
public slots:
    void open(const QUrl &value) { url = value; ++count; }
};

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
    QLocale::setDefault(QLocale(QLocale::Chinese, QLocale::SimplifiedChineseScript, QLocale::China));
#if defined(Q_OS_WINDOWS)
    QFont applicationFont = app.font();
    applicationFont.setFamily("Microsoft YaHei UI");
    if (applicationFont.pointSizeF() <= 0) applicationFont.setPointSizeF(9.75);
    app.setFont(applicationFont);
#endif
    app.setApplicationName("EZ4Connectpro");
    app.setApplicationDisplayName("EZ4Connectpro");
    app.setApplicationVersion(PROJ_VER);
    scenario = CommandLineOptions::value(app.arguments(), "--scenario");
    if (scenario == "signature") qputenv("EZ4_QA_SIGNATURE_FAILURE", "1");
    ResourceUrlProbe resourceUrls;
    const bool temporaryPortal = scenario == "manual-portal-temporary";
    const bool manualPortal = scenario == "manual-portal" || temporaryPortal;
    const bool testResourcePage = scenario == "resources" || manualPortal;
    if (testResourcePage)
    {
        QDesktopServices::setUrlHandler("http", &resourceUrls, "open");
        QDesktopServices::setUrlHandler("https", &resourceUrls, "open");
    }
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
        if (testResourcePage) settings.setValue(ApplicationConstants::SsoBrowserModeKey, temporaryPortal ? "temporary" : "system");
        settings.setValue("ZJUConnect/AuthType", manualPortal ? "cas" : "psw");
        settings.setValue("ZJUConnect/ServerAddress", manualPortal ? "127.0.0.1" : "office.example");
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
    if (scenario == "ui" || scenario == "signature") selected = QStringList{QString{}};
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
    if (manualPortal)
    {
        QFile cache(ApplicationPaths::clientDataFile("manual", true));
        if (!cache.open(QIODevice::WriteOnly)) return 2;
        cache.write(R"({"cookies":[{"scheme":"https","host":"127.0.0.1:443","name":"sid","value":"resource-fixture"}]})");
        cache.close();
        QTimer::singleShot(0, window.findChild<QPushButton *>("pushButton1"), &QPushButton::click);
    }
    int conflictDialogs = 0;
    int signatureDialogs = 0;
    int cleanShutdowns = 0;
    int ticks = 0;
    QTimer poll;
    poll.setInterval(30);
    QObject::connect(&poll, &QTimer::timeout, [&]()
    {
        ++ticks;
        if (auto *message = qobject_cast<QMessageBox *>(QApplication::activeModalWidget()); message && scenario != "devices")
        {
            if (scenario == "signature")
            {
                if (!message->text().contains("签名校验失败") || !message->text().contains("10000008") || message->text().contains("超时"))
                    qFatal("Environment signature failure was misreported");
                const QString capture = qEnvironmentVariable("EZ4_SIGNATURE_CAPTURE");
                if (!capture.isEmpty() && !message->grab().save(capture)) qFatal("Could not capture signature error");
                ++signatureDialogs;
            }
            else
            {
                if (scenario != "conflict" || message->windowTitle() != "无法启动配置") qFatal("Unexpected startup dialog");
                ++conflictDialogs;
            }
            message->accept();
        }
        if (verified)
        {
            if (!coordinator->hasActiveConnections()) app.quit();
            return;
        }
        if (scenario == "ui" || scenario == "elevate") return;
        if (manualPortal)
        {
            if (!ready("manual")) return;
            const auto *runtime = coordinator->connectionFor(storage.profilePath("manual"));
            const auto *browser = runtime->findChild<TemporaryBrowserSsoSession *>();
            const auto *bridge = runtime->findChild<BrowserSsoSession *>();
            if (resourceUrls.count || !browser || browser->isActive() || !bridge || !bridge->entryUrl().isEmpty())
                qFatal("A connection without browser authentication opened a resource page");
            const auto *core = runtime->session->findChild<QProcess *>();
            if (!core || !core->processId()) qFatal("Manual connection core missing");
            auto *button = window.findChild<QPushButton *>("resourcePageButton");
            if (!button || !button->isEnabled()) qFatal("Connected resource action unavailable");
            if (!temporaryPortal)
            {
                const auto pid = core->processId();
                button->click();
                if (resourceUrls.count != 1 || resourceUrls.url != bridge->entryUrl() || resourceUrls.url.host() != "127.0.0.1"
                    || core->processId() != pid || !ready("manual")) qFatal("Explicit portal request did not use the active connection");
            }
            verified = true;
            coordinator->stopConnections();
            return;
        }
        if (scenario == "signature")
        {
            const auto *runtime = coordinator->connectionFor(storage.profilePath({}));
            if (!signatureDialogs || !runtime) return;
            if (signatureDialogs != 1 || runtime->session->state() != ConnectionState::Failed
                || coordinator->hasActiveConnections()) qFatal("Signature rejection was retried or ignored");
            verified = true;
            return;
        }
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
        if (scenario == "resources")
        {
            if (resourceUrls.count) qFatal("Background connections unexpectedly opened a browser");
            auto *button = window.findChild<QPushButton *>("resourcePageButton");
            auto *list = window.findChild<QListWidget *>("profileList");
            if (!button || !list || button->isEnabled()) qFatal("Disconnected resource action should be disabled");
            for (const auto &id : {QString{}, QString("office")})
            {
                for (int i = 0; i < list->count(); ++i)
                {
                    if (list->item(i)->data(Qt::UserRole).toString() != id) continue;
                    list->setCurrentRow(i); emit list->itemClicked(list->item(i)); break;
                }
                QEventLoop selection;
                QTimer::singleShot(0, &selection, &QEventLoop::quit); selection.exec();
                auto *runtime = coordinator->connectionFor(storage.profilePath(id));
                auto *core = runtime->session->findChild<QProcess *>();
                const qint64 pid = core ? core->processId() : 0;
                if (!pid || !button->isEnabled()) qFatal("Connected resource action unavailable");
                const QString sid = "resource-fixture-" + id;
                QFile file(ApplicationPaths::clientDataFile(id, true));
                if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("Could not write resource fixture");
                file.write(QJsonDocument(QJsonObject{{"cookies", QJsonArray{QJsonObject{
                    {"scheme", "https"}, {"host", "office.example:443"}, {"name", "sid"}, {"value", sid}}}}}).toJson());
                file.close();
                resourceUrls.url = QUrl{}; button->click();
                if (resourceUrls.url.host() != "127.0.0.1") qFatal("Resource action did not use the local session bridge");
                QNetworkAccessManager network; network.setProxy(QNetworkProxy::NoProxy);
                const auto get = [&](const QString &path)
                {
                    QNetworkRequest request(resourceUrls.url.resolved(QUrl(path))); request.setTransferTimeout(2000);
                    QEventLoop loop; auto *reply = network.get(request);
                    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit); loop.exec();
                    const auto data = QJsonDocument::fromJson(reply->readAll()).object(); delete reply; return data;
                };
                if (!get("metadata").value("resourceOnly").toBool()) qFatal("Resource action tried to start authentication");
                const auto data = get("resources");
                const auto cookies = data.value("cookies").toArray();
                if (cookies.size() != 1 || cookies.at(0).toObject().value("value") != sid || !ready(id)
                    || core->processId() != pid) qFatal("Resource action crossed profile sessions or interrupted the VPN core");
            }
            const QString capture = qEnvironmentVariable("EZ4_RESOURCES_CAPTURE");
            if (!capture.isEmpty())
            {
                window.show(); window.resize(980, 690);
                app.styleHints()->setColorScheme(Qt::ColorScheme::Light); app.processEvents();
                if (!window.grab().save(capture + "-light.png")) qFatal("Could not capture resource button");
                window.resize(window.minimumSize());
                app.styleHints()->setColorScheme(Qt::ColorScheme::Dark); app.processEvents();
                if (!window.grab().save(capture + "-dark.png")) qFatal("Could not capture compact resource button");
                int actionRow = -1;
                for (const auto *name : {"pushButton1", "pushButton2", "resourcePageButton", "manageTrustedDevicesButton", "settingPushButton", "profileActionsButton"})
                {
                    auto *action = window.findChild<QPushButton *>(name);
                    if (!action || action->isHidden()) qFatal("Expected direct action missing");
                    const QPoint position = action->mapTo(&window, QPoint{});
                    const int center = position.y() + action->height()/2;
                    if (actionRow < 0) actionRow = center;
                    if (qAbs(center-actionRow) > 1 || action->width() < action->sizeHint().width()
                        || position.x() + action->width() > window.width())
                        qFatal("Direct action %s clipped or not on one row: x=%d width=%d hint=%d y=%d row=%d window=%d",
                               name, position.x(), action->width(), action->sizeHint().width(), center, actionRow, window.width());
                }
            }
        }
        if (scenario == "devices")
        {
            auto *list=window.findChild<QListWidget *>("profileList");
            if(!list)qFatal("Workspace profile list missing");
            for (int i = 0; i < list->count(); ++i)
            {
                if (list->item(i)->data(Qt::UserRole).toString() != "office") continue;
                list->setCurrentRow(i);
                emit list->itemClicked(list->item(i));
                break;
            }
            // Workspace selection is queued to avoid rebuilding the list in its
            // click handler. Wait for that real user action before opening its menu.
            QEventLoop selection;
            QTimer::singleShot(0, &selection, &QEventLoop::quit);
            selection.exec();
            if (storage.activeProfile() != "office") qFatal("Connected profile was not selected");
            auto *button=window.findChild<QPushButton *>("profileActionsButton");
            auto *devices=window.findChild<QPushButton *>("manageTrustedDevicesButton");
            if(!button || !button->menu() || !devices || !devices->isEnabled() || devices->isHidden())qFatal("Direct device management blocked while connected");
            QMetaObject::invokeMethod(button->menu(),"aboutToShow",Qt::DirectConnection);
            for(auto *entry:button->menu()->actions())if(!entry->isSeparator() && (entry->isEnabled() || entry->text().contains("授信")))qFatal("Destructive action enabled or device manager still hidden in menu");
            QFile cache(ApplicationPaths::clientDataFile("office",true));
            if(!cache.open(QIODevice::WriteOnly|QIODevice::Truncate))qFatal("Could not write mock device data");
            cache.write(R"({"selfId":"this-computer","deviceTrusted":false,"currentTrustDeviceCount":2,"trustDeviceConfig":{"enable":true,"enableSelfBind":1,"limit":2},"data":[{"id":"retired-desktop","deviceName":"Retired desktop"},{"id":"travel-notebook","deviceName":"Retained notebook"}]})");cache.close();
            QList<QPair<QProcess *,qint64>> cores;
            for(const auto &id: {QString{},QString("office")}) {
                auto *core=coordinator->connectionFor(storage.profilePath(id))->session->findChild<QProcess *>();
                if(!core || !core->processId())qFatal("Active VPN core missing");
                cores.append({core,core->processId()});
            }
            int phase=0;
            QTimer devicePoll;devicePoll.setInterval(20);
            QObject::connect(&devicePoll,&QTimer::timeout,[&]{
                auto *dialog=window.findChild<QDialog *>("deviceTrustDialog");if(!dialog)return;
                auto *table=dialog->findChild<QTableWidget *>("deviceTrustTable");
                auto *status=dialog->findChild<QLabel *>("deviceTrustStatus");
                if(phase==0 && table && table->rowCount()==2) {
                    phase=1;table->selectRow(0);
                    QTimer::singleShot(0,dialog,[dialog]{auto *confirm=dialog->findChild<QMessageBox *>("deviceTrustConfirmation");if(!confirm)qFatal("Connected replacement confirmation missing");confirm->button(QMessageBox::Yes)->click();});
                    dialog->findChild<QPushButton *>("deviceTrustUnbind")->click();
                } else if(phase==1 && status && status->text().contains("换绑已完成")) {
                    if(!ready({}) || !ready("office"))qFatal("Device management interrupted VPN sessions");
                    for(const auto &entry:cores)if(entry.first->state()!=QProcess::Running || entry.first->processId()!=entry.second)qFatal("VPN core stopped or restarted during device management");
                    phase=2;devicePoll.stop();dialog->reject();
                }
            });
            devicePoll.start();devices->click();
            if(phase!=2)qFatal("Connected device replacement flow incomplete");
        }
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

#include "startupintegration_test.moc"
