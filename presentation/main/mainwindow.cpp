#include <QApplication>
#include <QMessageBox>
#include <QSysInfo>
#include <QNetworkInterface>
#include <QClipboard>
#include <QDesktopServices>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QActionGroup>
#include <QInputDialog>
#include <QMargins>
#include <QStyle>
#include <QStyleHints>
#include <QTimer>

#include <algorithm>
#include <stdexcept>
#include <utility>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include "mainwindow.h"

#include "application/applicationlogger.h"
#include "application/applicationconstants.h"
#include "application/commandlineoptions.h"
#include "application/settingsmigrator.h"
#include "infrastructure/coreprocess/devicetrust.h"
#include "infrastructure/logging/applicationlogfile.h"
#include "infrastructure/settings/profilemanager.h"
#include "infrastructure/storage/applicationpaths.h"
#include "infrastructure/update/updatechecker.h"
#include "presentation/dialogs/configurationguidedialog/configurationguidedialog.h"
#include "presentation/dialogs/vpnprofiledialog/vpnprofiledialog.h"
#include "presentation/presentationhelpers.h"
#include "ui_mainwindow.h"

namespace
{
bool appendStyleSheet(const QString &path, QString &styleSheet)
{
    QFile styleFile(path);
    if (!styleFile.open(QIODevice::ReadOnly | QIODevice::Text))
    {
        qWarning().noquote() << "无法加载样式资源：" << path;
        return false;
    }

    styleSheet.append(QString::fromUtf8(styleFile.readAll()));
    styleSheet.append('\n');
    return true;
}
}

MainWindow::MainWindow(
    ApplicationLogger *logger,
    ApplicationLogFile *logFile,
    QWidget *parent
) :
    QMainWindow(parent),
    ui(new Ui::MainWindow),
    applicationLogger(logger),
    applicationLogFile(logFile)
{
    const QStringList arguments = QCoreApplication::arguments();
    const QString overrideConfigPath = CommandLineOptions::value(arguments, "--config-path");
    coordinator = new MainWindowCoordinator(this, overrideConfigPath, this);
    profileService = coordinator->profiles();

    const bool startupLaunch = arguments.contains(ApplicationConstants::AutoStartArgument);
    const bool explicitConnect = arguments.contains("--connect");
    ProfileManager profileStorage;
    if (!profileService->usesOverrideConfiguration())
    {
        profileStorage.migrateStartupOptions(*profileService->settings(), profileService->currentProfileId());
    }
    const auto startup = profileStorage.startupOptions();
    const bool silent = ((startupLaunch && startup.silent) || arguments.contains("--silent"))
        && QSystemTrayIcon::isSystemTrayAvailable();
    const bool requestedStartupProfile = startupLaunch && startup.enabled && startup.profileId.has_value()
        && !explicitConnect && overrideConfigPath.isEmpty();
    bool connectStartupProfile = false;
    if (requestedStartupProfile)
    {
        const auto profiles = profileService->vpnProfiles();
        const auto selected = std::find_if(profiles.cbegin(), profiles.cend(), [&startup](const VpnProfile &profile)
        {
            return profile.id == *startup.profileId && !profile.server.isEmpty() && profile.port > 0 && profile.port <= 65535;
        });
        connectStartupProfile = selected != profiles.cend() && profileService->switchTo(selected->id);
    }

    currentProfileId = profileService->currentProfileId();
    settings = profileService->settings();

    const bool supported = profileService->currentVpnProfile().isSupported();
    const bool isFirstLaunch = supported && !settings->contains("Common/ConfigVersion");
    upgradeSettings(!startupLaunch && !silent);

    ui->setupUi(this);
#ifdef Q_OS_MACOS
    const QMargins centralMargins = ui->centralWidget->contentsMargins();
    ui->centralWidget->setContentsMargins(centralMargins.left(), 12, centralMargins.right(), centralMargins.bottom());
#endif
    ui->logPlainTextEdit->setFont(
        QFontDatabase::systemFont(QFontDatabase::FixedFont)
    );
    QStyleHints *styleHints = QGuiApplication::styleHints();
    applyColorScheme(styleHints->colorScheme());
    connect(styleHints, &QStyleHints::colorSchemeChanged,
            this, &MainWindow::applyColorScheme);
    systemProxySession = coordinator->systemProxy();
    updateChecker = coordinator->updates();
    connect(updateChecker, &UpdateChecker::versionInfoChanged, this,
            [this](const VersionInfo &) { updateVersionInfo(); });
    connect(updateChecker, &UpdateChecker::checkFailed, this, [this](const QString &reason)
    {
        if (std::exchange(manualUpdateCheck, false))
        {
            QMessageBox::warning(this, "检查更新", reason);
        }
    });
    connect(updateChecker, &UpdateChecker::checkSucceeded, this,
            [this](const QString &latestVersion)
            {
                const bool manual = std::exchange(manualUpdateCheck, false);
                const QString currentVersion = QApplication::applicationVersion();
                if (!UpdateChecker::isNewerVersion(currentVersion, latestVersion))
                {
                    if (manual)
                    {
                        QMessageBox::information(this, "检查更新",
                            "当前版本 " + currentVersion + "\n最新发布 " + latestVersion);
                    }
                    return;
                }
                if (!manual && !isVisible())
                {
                    showNotification(ApplicationConstants::ApplicationName, "发现新版本 " + latestVersion);
                    return;
                }

                QMessageBox messageBox(this);
                messageBox.setWindowTitle("检查更新");
                messageBox.setTextFormat(Qt::PlainText);
                messageBox.setText("发现新版本 " + latestVersion);
                messageBox.setInformativeText("当前版本 " + currentVersion);
                auto *openRelease = messageBox.addButton("查看发布", QMessageBox::AcceptRole);
                messageBox.addButton("稍后", QMessageBox::RejectRole);
                messageBox.exec();
                if (messageBox.clickedButton() == openRelease)
                {
                    QDesktopServices::openUrl(QUrl(ApplicationConstants::LatestReleaseUrl));
                }
            });
    connect(applicationLogger, &ApplicationLogger::entryAdded, this,
            [this](const QString &entry)
            {
                ui->logPlainTextEdit->appendPlainText(entry);
            });
    connect(systemProxySession, &SystemProxySession::busyChanged, this,
            &MainWindow::resetConnectionUi);
    connect(systemProxySession, &SystemProxySession::enabledChanged, this,
            &MainWindow::resetConnectionUi);
    setupTrayIcon();
    setupWorkspace();
    refreshProfiles();

    setWindowIcon(QIcon(QPixmap(":/resource/icon.png").scaled(
        512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation
    )));

    ui->applicationNameLabel->setText(QApplication::applicationDisplayName());

    updateVersionInfo();

    connect(ui->startupSettingsAction, &QAction::triggered, this, &MainWindow::showStartupSettings);

    connect(ui->openLogPushButton, &QPushButton::clicked,
            [this]()
            {
                const QString logFilePath = applicationLogFile->filePath();
                QFileInfo logFileInfo(logFilePath);

                if (logFileInfo.exists())
                {
                    QDesktopServices::openUrl(QUrl::fromLocalFile(logFilePath));
                }
                else
                {
                    QMessageBox::warning(this, "日志文件", "日志文件创建失败。");
                }
            });

    // 帮助-检查更新
    connect(ui->checkUpdateAction, &QAction::triggered, this, [this]()
    {
        manualUpdateCheck = true;
        updateChecker->check();
    });

    // 帮助-项目主页
    connect(ui->projectAction, &QAction::triggered,
            []()
            {
                QDesktopServices::openUrl(QUrl(ApplicationConstants::ProjectUrl));
            });

    // 帮助-关于本软件
    connect(ui->aboutAction, &QAction::triggered,
            [&]()
            {
                PresentationHelpers::showAboutDialog(this);
            });

    // 复制日志
    connect(ui->copyLogPushButton, &QPushButton::clicked,
            [&]()
            {
                auto logText = ui->logPlainTextEdit->toPlainText();
                QApplication::clipboard()->setText(logText);
            }
    );

    // 清空日志
    connect(ui->clearLogPushButton, &QPushButton::clicked,
            [&]()
            {
                ui->logPlainTextEdit->clear();
            }
    );

    clearLog();
    if (requestedStartupProfile && !connectStartupProfile)
    {
        qWarning().noquote() << "开机连接线路不可用。请在「设置 → 启动设置」中选择线路。";
    }
    if (connectStartupProfile || (explicitConnect && supported && !profileService->recoveredUnsupportedProfile()))
    {
        QTimer::singleShot(0, ui->pushButton1, &QPushButton::click);
    }

    if (supported && settings->value("Common/CheckUpdateAfterStart", true).toBool())
    {
        updateChecker->check();
    }
    else
    {
        updateChecker->markDisabled();
    }

    if (!silent)
    {
        show();
        if (isFirstLaunch && !startupLaunch)
        {
            QTimer::singleShot(
                0,
                this,
                &MainWindow::promptFirstLaunchGuide
            );
        }
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (coordinator->hasActiveConnections())
    {
        event->ignore();
        hide();
        showNotification(ApplicationConstants::ApplicationName, "程序已最小化到系统托盘，单击图标可恢复窗口。", QSystemTrayIcon::MessageIcon::Information);
    }
    else
    {
        event->accept();
    }
}

void MainWindow::changeEvent(QEvent *event)
{
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange)
    {
        const auto *stateChangeEvent = static_cast<QWindowStateChangeEvent *>(event);
        if (isMinimized() && !(stateChangeEvent->oldState() & Qt::WindowMinimized))
        {
            hide();
            showNotification(ApplicationConstants::ApplicationName, "程序已最小化到系统托盘，单击图标可恢复窗口。", QSystemTrayIcon::MessageIcon::Information);
        }
    }
}

void MainWindow::clearLog()
{
    ui->logPlainTextEdit->clear();
    ui->logPlainTextEdit->appendPlainText(
        "欢迎使用 " + QApplication::applicationDisplayName() + "\n"
        "当前版本：" + QApplication::applicationVersion() + "\n"
        "系统版本：" + QSysInfo::prettyProductName() + "\n"
        "当前配置：" + profileService->currentVpnProfile().name + "\n"
        "配置路径：" + settings->fileName() + "\n");
}

void MainWindow::applyColorScheme(Qt::ColorScheme scheme)
{
    const QString themePath = scheme == Qt::ColorScheme::Dark
        ? QStringLiteral(":/resource/mainwindow-dark.qss")
        : QStringLiteral(":/resource/mainwindow-light.qss");

    QString styleSheet;
    if (!appendStyleSheet(QStringLiteral(":/resource/mainwindow.qss"), styleSheet)
        || !appendStyleSheet(themePath, styleSheet))
    {
        return;
    }

    setStyleSheet(styleSheet);
}

void MainWindow::setupTrayIcon()
{
    // 系统托盘
    trayIcon = new QSystemTrayIcon(this);
    trayIcon->setIcon(
        QIcon(QPixmap(":/resource/icon.png").scaled(512, 512, Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    trayIcon->setToolTip(QApplication::applicationName());
    connect(trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
    {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick
            || reason == QSystemTrayIcon::MiddleClick)
        {
            restoreFromTray();
        }
    });
    connect(trayIcon, &QSystemTrayIcon::messageClicked, this, &MainWindow::restoreFromTray);

    trayConnectAction = new QAction("连接服务器", this);
    trayProfileMenu = new QMenu("配置选择", this);
    traySwitchGroup = new QActionGroup(this);
    trayMenu = new QMenu(this);
    trayMenu->addAction(trayConnectAction);
    trayMenu->addSeparator();
    trayMenu->addMenu(trayProfileMenu);
    trayMenu->addSeparator();
    trayReleaseProxyAction = trayMenu->addAction("释放本工具代理", systemProxySession, &SystemProxySession::disable);
    trayMenu->addAction("显示主界面", this, &MainWindow::restoreFromTray);
    trayMenu->addAction("退出 " + QApplication::applicationName(), this, &MainWindow::gracefullyQuit);
    connect(trayConnectAction, &QAction::triggered, ui->pushButton1, &QPushButton::click);
    trayIcon->setContextMenu(trayMenu);
    trayIcon->show();
}

void MainWindow::restoreFromTray()
{
    if (quitting)
    {
        return;
    }
    if (isMinimized())
    {
        setWindowState(windowState() & ~Qt::WindowMinimized);
    }
    show();

    QWidget *target = QApplication::activeModalWidget();
    if (!target)
    {
        target = this;
    }
    target->raise();
#ifdef Q_OS_WIN
    const HWND handle = reinterpret_cast<HWND>(target->winId());
    if (!SetForegroundWindow(handle) && !(GetWindowLongPtrW(handle, GWL_EXSTYLE) & WS_EX_TOPMOST))
    {
        // Show the window above other applications even if Windows withholds focus.
        constexpr UINT flags = SWP_NOSIZE | SWP_NOMOVE | SWP_NOACTIVATE;
        if (SetWindowPos(handle, HWND_TOPMOST, 0, 0, 0, 0, flags))
        {
            SetWindowPos(handle, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
        }
    }
#else
    target->activateWindow();
#endif
}

void MainWindow::refreshProfiles()
{
    trayProfileMenu->clear();
    profileEntries = profileService->vpnProfiles();
    for (const VpnProfile &profile : profileEntries)
    {
        auto *action = trayProfileMenu->addAction(profile.name);
        action->setCheckable(true);
        action->setChecked(profile.settingsPath == settings->fileName());
        traySwitchGroup->addAction(action);
        connect(action, &QAction::triggered, this, [this, profile]()
        {
            if (profile.settingsPath != settings->fileName())
            {
                switchProfile(profile.id);
            }
        });
    }
    refreshProfileList();
    resetConnectionUi();
}

bool MainWindow::switchProfile(const QString &profileId)
{
    if (!profileService->usesOverrideConfiguration()
        && profileId == currentProfileId)
    {
        return true;
    }

    if (settingWindow != nullptr)
    {
        settingWindow->close();
    }

    if (!profileService->switchTo(profileId))
    {
        refreshProfiles();
        return false;
    }
    settings = profileService->settings();
    currentProfileId = profileService->currentProfileId();

    upgradeSettings();
    updateVersionInfo();
    refreshProfiles();

    qInfo().noquote() << "已切换到配置「" + profileService->currentVpnProfile().name.simplified() + "」。";
    return true;
}

void MainWindow::createProfile()
{
    VpnProfileDialog dialog(this);
    if (dialog.exec() != QDialog::Accepted)
    {
        return;
    }
    if (settingWindow)
    {
        settingWindow->close();
    }
    if (!profileService->createAndSwitch(dialog.profile()))
    {
        QMessageBox::critical(this, "创建失败", "无法保存新配置。");
        return;
    }
    settings = profileService->settings();
    currentProfileId = profileService->currentProfileId();
    ui->profileSearch->clear();
    ui->protocolFilter->setCurrentIndex(0);
    refreshProfiles();
    qInfo().noquote() << "已创建配置「" + profileService->currentVpnProfile().name + "」。";
}

void MainWindow::openConfigurationGuide()
{
    if (!profileService->currentVpnProfile().isSupported())
    {
        return;
    }
    if (selectedConnectionIsActive())
    {
        QMessageBox::warning(
            this,
            "无法修改配置",
            "请先断开 VPN 连接，再使用配置引导。"
        );
        return;
    }

    if (settingWindow != nullptr)
    {
        settingWindow->close();
    }

    const QString oldServerAddress =
        settings->value("ZJUConnect/ServerAddress").toString();
    const int oldServerPort =
        settings->value("ZJUConnect/ServerPort").toInt();
    const QString oldProtocol =
        settings->value("ZJUConnect/Protocol").toString();
    const QString oldAuthType =
        settings->value("ZJUConnect/AuthType").toString();
    const QString oldEasyConnectAuthType =
        settings->value("ZJUConnect/EasyConnectAuthType").toString();
    const QString oldLoginDomain =
        settings->value("ZJUConnect/LoginDomain").toString();
    const QString oldLoginUrl =
        settings->value("ZJUConnect/LoginURL").toString();

    ConfigurationGuideDialog guide(this, settings);
    if (guide.exec() != QDialog::Accepted)
    {
        return;
    }
    guide.applyTo(*settings);

    const bool authenticationSettingsChanged =
        oldServerAddress != settings->value("ZJUConnect/ServerAddress").toString()
        || oldServerPort != settings->value("ZJUConnect/ServerPort").toInt()
        || oldProtocol != settings->value("ZJUConnect/Protocol").toString()
        || oldAuthType != settings->value("ZJUConnect/AuthType").toString()
        || oldEasyConnectAuthType
            != settings->value("ZJUConnect/EasyConnectAuthType").toString()
        || oldLoginDomain != settings->value("ZJUConnect/LoginDomain").toString()
        || oldLoginUrl != settings->value("ZJUConnect/LoginURL").toString();
    if (authenticationSettingsChanged)
    {
        ApplicationPaths::clearClientData(currentProfileId);
    }

    refreshProfiles();
    qInfo().noquote() << "已通过配置引导更新当前配置";
}

void MainWindow::promptFirstLaunchGuide()
{
    promptConfigurationGuide(
        "欢迎使用 EZ4Connectpro",
        "检测到这是首次启动，是否现在配置 VPN 服务器？"
    );
}

void MainWindow::promptConfigurationGuide(
    const QString &windowTitle,
    const QString &text
)
{
    QMessageBox messageBox(this);
    messageBox.setWindowTitle(windowTitle);
    messageBox.setText(text);
    messageBox.setInformativeText(
        "配置引导将协助你选择协议、填写服务器地址、认证方式和登录凭据。"
    );

    QPushButton *startButton = messageBox.addButton(
        "使用配置引导",
        QMessageBox::AcceptRole
    );
    messageBox.addButton("稍后配置", QMessageBox::RejectRole);
    messageBox.setDefaultButton(startButton);
    messageBox.exec();

    if (messageBox.clickedButton() == startButton)
    {
        openConfigurationGuide();
    }
}

void MainWindow::renameCurrentProfile()
{
    if (selectedConnectionIsActive())
    {
        return;
    }
    bool accepted = false;
    const QString settingsPath = settings->fileName();
    const QString oldName = profileService->currentVpnProfile().name;
    const QString name = QInputDialog::getText(this, "重命名配置", "配置名称", QLineEdit::Normal, oldName, &accepted).trimmed();
    if (!accepted || name == oldName || settings->fileName() != settingsPath || selectedConnectionIsActive())
    {
        return;
    }
    if (name.size() > 60 || !profileService->renameDisplayName(name))
    {
        QMessageBox::warning(this, "重命名失败", "请输入 1 到 60 个字符的配置名称。");
        return;
    }
    refreshProfiles();
}

void MainWindow::deleteCurrentProfile()
{
    if (selectedConnectionIsActive())
    {
        QMessageBox::information(this, "配置正在使用", "请先断开此配置的连接。");
        return;
    }
    if (currentProfileId.isEmpty())
    {
        QMessageBox::warning(this, "删除失败", "默认配置不可删除。");
        return;
    }

    QMessageBox messageBox(this);
    const QString removedProfileId = currentProfileId;
    const QString removedSettingsPath = settings->fileName();
    messageBox.setWindowTitle("删除配置");
    messageBox.setText("确认删除配置「" + profileService->currentVpnProfile().name + "」吗？");
    messageBox.addButton(QMessageBox::Yes)->setText("是");
    messageBox.addButton(QMessageBox::No)->setText("否");
    messageBox.setDefaultButton(QMessageBox::No);
    if (messageBox.exec() != QMessageBox::Yes || settings->fileName() != removedSettingsPath || selectedConnectionIsActive())
    {
        return;
    }

    coordinator->forgetConnection(removedSettingsPath);
    if (settingWindow != nullptr)
    {
        settingWindow->close();
    }
    if (!profileService->removeCurrentAndSwitchToDefault())
    {
        QMessageBox::warning(this, "删除失败", "无法删除该配置文件。");
        return;
    }

    currentProfileId = profileService->currentProfileId();
    settings = profileService->settings();
    upgradeSettings();
    updateVersionInfo();
    refreshProfiles();
    qInfo().noquote() << "已删除配置：" + removedProfileId;
}

void MainWindow::clearLoginCache()
{
    const auto profile = profileService->currentVpnProfile();
    if (profile.protocolId != "atrust" || selectedConnectionIsActive())
    {
        return;
    }
    const auto answer = QMessageBox::question(this, "清理登录缓存",
        "清理「" + profile.name + "」的登录缓存后，下次连接需重新登录。",
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (answer != QMessageBox::Yes || settings->fileName() != profile.settingsPath || selectedConnectionIsActive())
    {
        return;
    }
    if (!ApplicationPaths::clearClientData(profile.id))
    {
        QMessageBox::warning(this, "清理登录缓存", "无法删除此配置的登录缓存文件。");
        return;
    }
    qInfo().noquote() << "已清理「" + profile.name + "」的登录缓存";
}

void MainWindow::setDeviceTrust(bool trusted)
{
    const auto profile = profileService->currentVpnProfile();
    if (profile.protocolId != "atrust" || profile.server.isEmpty() || selectedConnectionIsActive())
    {
        return;
    }
    const QString action = trusted ? QStringLiteral("设为授信设备") : QStringLiteral("取消授信设备");
    try
    {
        DeviceTrust::set(this, profile.protocolId, profile.server, profile.port, profile.id, trusted);
        qInfo().noquote() << action + "成功";
        QMessageBox::information(this, action, "操作成功。");
    }
    catch (const std::runtime_error &error)
    {
        const QString detail = QString::fromLocal8Bit(error.what());
        qWarning().noquote() << action + "失败" << detail;
        QMessageBox::warning(this, action, detail);
    }
}

void MainWindow::upgradeSettings(bool interactive)
{
    if (!profileService->currentVpnProfile().isSupported())
    {
        return;
    }
    const SettingsMigrationAction action = SettingsMigrator::prepare(*settings);
    if (interactive && action == SettingsMigrationAction::RecommendReset)
    {
        QMessageBox msgBox;
        msgBox.setText("存在配置更新");
        msgBox.setInformativeText("建议恢复默认设置，以使用优化的配置。\n\n是否恢复默认设置？");
        msgBox.setStandardButtons(QMessageBox::Ok | QMessageBox::Cancel);
        msgBox.setDefaultButton(QMessageBox::Cancel);

        const bool reset = msgBox.exec() == QMessageBox::Ok;
        SettingsMigrator::finish(*settings, reset);
        if (reset)
        {
            QMessageBox::information(this, "完成", "已恢复默认设置。");
        }
        return;
    }
    SettingsMigrator::finish(*settings, false);
}

void MainWindow::updateVersionInfo()
{
    const VersionInfo &versionInfo = updateChecker->versionInfo();
    ui->versionLabel->setText("版本 " + versionInfo.uiVersion);
    ui->versionLabel->setToolTip("当前版本 " + versionInfo.uiVersion + "\n最新发布 " + versionInfo.uiLatest);
    updateProfileSummary();
}

void MainWindow::showNotification(const QString &title, const QString &content, QSystemTrayIcon::MessageIcon icon)
{
    trayIcon->showMessage(title, content, icon, 10000);
}

void MainWindow::cleanUpWhenQuit()
{
    // 保存配置
    if (profileService->currentVpnProfile().isSupported()
        && settings->value("Common/ConfigVersion", 0).toInt() <=
        ApplicationConstants::ConfigVersion)
    {
        settings->setValue(
            "Common/ConfigVersion",
            ApplicationConstants::ConfigVersion
        );
    }
    settings->sync();

    // 清除系统代理
    coordinator->prepareForShutdown();
}

void MainWindow::gracefullyQuit()
{
    quitting = true;
    coordinator->stopConnections();
    if (!coordinator->hasActiveConnections())
    {
        qApp->quit();
    }
}

MainWindow::~MainWindow()
{
    delete coordinator;
    coordinator = nullptr;

    QObject::disconnect(applicationLogger, nullptr, this, nullptr);
    delete ui;
    ui = nullptr;
}
