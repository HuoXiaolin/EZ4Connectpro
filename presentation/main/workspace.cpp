#include "mainwindow.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStandardPaths>
#include <QStyle>
#include <QTimer>

#include "infrastructure/storage/applicationpaths.h"
#include "presentation/coordinators/connectionuicontroller.h"
#include "ui_mainwindow.h"

namespace
{
QString stateTitle(ConnectionState state)
{
    switch (state)
    {
    case ConnectionState::Starting: return "正在连接";
    case ConnectionState::Running: return "已连接";
    case ConnectionState::Stopping: return "正在断开";
    case ConnectionState::Reconnecting: return "正在重连";
    case ConnectionState::Interrupted: return "连接中断";
    case ConnectionState::Failed: return "连接失败";
    case ConnectionState::Disconnected: return "未连接";
    }
    return {};
}

QString endpointText(const VpnProfile &profile)
{
    if (profile.server.isEmpty())
    {
        return "尚未填写网关";
    }
    const QString host = profile.server.contains(':') && !profile.server.startsWith('[')
        ? '[' + profile.server + ']' : profile.server;
    return host + ':' + QString::number(profile.port);
}

void refreshProperty(QWidget *widget, const char *name, const QVariant &value)
{
    if (widget->property(name) != value)
    {
        widget->setProperty(name, value);
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }
}
}

void MainWindow::setupWorkspace()
{
    ui->profileNameLabel->setWordWrap(true);
    ui->protocolFilter->addItem("全部协议", QString{});
    const auto &protocols = vpnProtocolNames();
    for (auto it = protocols.cbegin(); it != protocols.cend(); ++it)
    {
        ui->protocolFilter->addItem(it.value(), it.key());
    }
    auto *search = new QShortcut(QKeySequence::Find, this);
    connect(search, &QShortcut::activated, ui->profileSearch, qOverload<>(&QLineEdit::setFocus));
    connect(ui->profileSearch, &QLineEdit::textChanged, this, &MainWindow::refreshProfileList);
    connect(ui->protocolFilter, &QComboBox::currentIndexChanged, this, &MainWindow::refreshProfileList);
    const auto select = [this](QListWidgetItem *item)
    {
        const QString id = item->data(Qt::UserRole).toString();
        const QString path = item->data(Qt::UserRole + 1).toString();
        QTimer::singleShot(0, this, [this, id, path]()
        {
            if (!quitting && path != settings->fileName())
            {
                switchProfile(id);
            }
        });
    };
    connect(ui->profileList, &QListWidget::itemClicked, this, select);
    connect(ui->profileList, &QListWidget::itemActivated, this, select);
    connect(ui->newProfilePushButton, &QPushButton::clicked, this, &MainWindow::createProfile);
    connect(ui->settingPushButton, &QPushButton::clicked, this, &MainWindow::editSelectedProfile);
    connect(ui->guidePushButton, &QPushButton::clicked, this, &MainWindow::openConfigurationGuide);
    connect(ui->importConfigAction, &QAction::triggered, this, &MainWindow::importConfiguration);
    connect(ui->exportConfigAction, &QAction::triggered, this, &MainWindow::exportConfiguration);

    auto *profileActions = new QMenu(ui->profileActionsButton);
    auto *rename = profileActions->addAction("重命名配置", this, &MainWindow::renameCurrentProfile);
    auto *remove = profileActions->addAction("删除配置", this, &MainWindow::deleteCurrentProfile);
    profileActions->addSeparator();
    auto *clearCache = profileActions->addAction("清理登录缓存", this, &MainWindow::clearLoginCache);
    auto *trust = profileActions->addAction("设为授信设备", this, [this]() { setDeviceTrust(true); });
    auto *untrust = profileActions->addAction("取消授信设备", this, [this]() { setDeviceTrust(false); });
    connect(profileActions, &QMenu::aboutToShow, this, [this, rename, remove, clearCache, trust, untrust]()
    {
        const auto profile = profileService->currentVpnProfile();
        const bool managed = !profileService->usesOverrideConfiguration();
        const bool isATrust = profile.protocolId == "atrust";
        rename->setVisible(managed);
        remove->setVisible(managed && !profile.id.isEmpty());
        clearCache->setVisible(isATrust);
        QFile cache(ApplicationPaths::clientDataFile(profile.id, false));
        clearCache->setEnabled(isATrust && cache.exists());
        const bool canManageTrust = isATrust && !profile.server.isEmpty() && cache.open(QIODevice::ReadOnly)
            && !QJsonDocument::fromJson(cache.readAll()).object().value("cookies").toArray().isEmpty();
        trust->setVisible(canManageTrust);
        untrust->setVisible(canManageTrust);
    });
    ui->profileActionsButton->setMenu(profileActions);

    connect(ui->pushButton1, &QPushButton::clicked, this, &MainWindow::startSelectedProfile);
    connect(ui->pushButton2, &QPushButton::clicked, this, [this]()
    {
        if (auto *runtime = selectedConnection())
        {
            runtime->controls->handleProxyClicked();
        }
    });
}

ProfileConnection *MainWindow::selectedConnection() const
{
    return coordinator->connectionFor(settings->fileName());
}

bool MainWindow::selectedConnectionIsActive() const
{
    const auto *runtime = selectedConnection();
    return runtime && runtime->session->isActive();
}

void MainWindow::refreshProfileList()
{
    const QString search = ui->profileSearch->text().trimmed();
    const QString protocol = ui->protocolFilter->currentData().toString();
    const QSignalBlocker blocker(ui->profileList);
    ui->profileList->clear();
    int active = 0;
    for (const VpnProfile &profile : profileEntries)
    {
        const auto *runtime = coordinator->connectionFor(profile.settingsPath);
        const bool running = runtime && runtime->session->isActive();
        active += running ? 1 : 0;
        const QString endpoint = endpointText(profile);
        const QString protocolName = profile.protocolName();
        const QString searchable = profile.name + ' ' + protocolName + ' ' + endpoint;
        if ((!protocol.isEmpty() && protocol != profile.protocolId) || !searchable.contains(search, Qt::CaseInsensitive))
        {
            continue;
        }
        const QString subtitle = running ? protocolName + " · " + stateTitle(runtime->session->state()) : protocolName;
        auto *item = new QListWidgetItem(profile.name + '\n' + subtitle, ui->profileList);
        item->setData(Qt::UserRole, profile.id);
        item->setData(Qt::UserRole + 1, profile.settingsPath);
        item->setSizeHint(QSize(0, 53));
        item->setToolTip(profile.name + '\n' + subtitle + '\n' + endpoint);
        if (profile.settingsPath == settings->fileName())
        {
            ui->profileList->setCurrentItem(item);
        }
    }
    const QString count = ui->profileList->count() == 0 ? QStringLiteral("没有匹配的配置")
        : QString::number(ui->profileList->count()) + " 个配置";
    ui->profileFilterHint->setText(active > 0 ? count + " · " + QString::number(active) + " 个本地会话" : count);
}

void MainWindow::resetConnectionUi()
{
    updateProfileSummary();
    const auto *runtime = selectedConnection();
    updateConnectionState(runtime ? runtime->session->state() : ConnectionState::Disconnected);
}

void MainWindow::updateProfileSummary()
{
    const VpnProfile profile = profileService->currentVpnProfile();
    const bool supported = profile.isSupported();
    const QString name = supported ? profile.name : QStringLiteral("请选择或新建深信服配置");
    const QString endpoint = supported ? endpointText(profile) : QString{};
    ui->profileNameLabel->setText(name);
    ui->profileNameLabel->setToolTip(name);
    ui->profileDetailLabel->setText(profile.protocolName());
    ui->endpointLabel->setText(supported ? "网关  " + endpoint : QString{});
    ui->endpointLabel->setToolTip(endpoint);
}

void MainWindow::updateConnectionState(ConnectionState state)
{
    const VpnProfile profile = profileService->currentVpnProfile();
    const auto *runtime = selectedConnection();
    const bool supported = profile.isSupported();
    const bool active = runtime && runtime->session->isActive();
    const bool running = state == ConnectionState::Running;
    const bool authenticating = state == ConnectionState::Starting || state == ConnectionState::Reconnecting;
    const IntranetRouting routing = active ? runtime->routing : preferredIntranetRouting(*settings);
    const bool usesTun = routing != IntranetRouting::LocalProxy;
    const bool proxyReady = running && runtime && runtime->session->isHttpProxyReady();
    const bool ownsProxy = runtime && systemProxySession->isOwnedBy(runtime->settings->fileName());
    const bool canEdit = supported && !active && !quitting;
    const bool managed = !profileService->usesOverrideConfiguration();
    const QString button = active ? QStringLiteral("断开此连接") : QStringLiteral("连接 VPN");
    QString detail;
    QString appearance = "disconnected";
    if (!supported)
    {
        detail = "选择 aTrust 或 EasyConnect 配置后即可连接。";
    }
    else if (running)
    {
        appearance = "running";
        if (usesTun)
        {
            detail = proxyReady ? "内网分流已启动，可在常用浏览器输入内网网址。"
                                : "VPN 已认证，正在准备内网通道。";
        }
        else if (ownsProxy && systemProxySession->isEnabled())
        {
            detail = "浏览器已接入系统代理。";
        }
        else if (ownsProxy && systemProxySession->isBusy())
        {
            detail = "正在检查并设置系统代理。";
        }
        else if (!proxyReady)
        {
            detail = "VPN 已连接，等待 HTTP 代理就绪。";
        }
        else if (systemProxySession->isEnabled())
        {
            detail = "本机代理已就绪。系统代理由另一条连接使用。";
        }
        else
        {
            detail = "本机代理已就绪，可使用系统代理访问内网页面。";
        }
    }
    else if (authenticating || state == ConnectionState::Stopping)
    {
        appearance = "starting";
        detail = "正在处理此配置的连接。";
    }
    else if (state == ConnectionState::Failed || state == ConnectionState::Interrupted)
    {
        appearance = "failed";
        detail = "请查看活动日志中的连接原因。";
    }
    else
    {
        detail = usesTun ? "完成认证后自动建立内网通道。" : "连接后启动本机代理。";
    }
    if (supported)
    {
        detail += "\n本机端口  SOCKS5 " + QString::number(profile.socksPort) + "   HTTP " + QString::number(profile.httpPort);
    }
    ui->statusTitleLabel->setText(supported ? stateTitle(state) : QStringLiteral("暂无所选配置"));
    ui->statusDetailLabel->setText(detail);
    ui->pushButton1->setText(button);
    const bool canOperate = supported && !quitting && state != ConnectionState::Stopping;
    ui->pushButton1->setEnabled(canOperate);
    ui->pushButton2->setVisible(running && !usesTun);
    ui->pushButton2->setEnabled(proxyReady && !quitting && !systemProxySession->isBusy()
        && (ownsProxy || !systemProxySession->isEnabled()));
    ui->pushButton2->setText(ownsProxy && systemProxySession->isEnabled() ? "释放系统代理" : "设置系统代理");
    ui->settingPushButton->setEnabled(canEdit);
    ui->guidePushButton->setEnabled(canEdit);
    ui->profileActionsButton->setEnabled(canEdit && (managed || profile.protocolId == "atrust"));
    ui->importConfigAction->setEnabled(!quitting);
    ui->exportConfigAction->setEnabled(supported && !quitting);
    refreshProperty(ui->routeProfileLabel, "stageActive", !authenticating && !running);
    refreshProperty(ui->routeAuthLabel, "stageActive", authenticating);
    refreshProperty(ui->routeTunnelLabel, "stageActive", running);
    refreshProperty(ui->statusIndicator, "connectionState", appearance);
    refreshProperty(ui->pushButton1, "connectionState", appearance);
    updateTray();
}

void MainWindow::importConfiguration()
{
    const QString path = QFileDialog::getOpenFileName(this, "导入配置文件",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "INI 配置文件 (*.ini)");
    if (path.isEmpty() || quitting)
    {
        return;
    }
    if (settingWindow)
    {
        settingWindow->close();
    }
    const auto result = profileService->importProfile(path);
    if (result != ProfileFileResult::Success)
    {
        QString message = "无法读取有效的 INI 配置。请核对服务器地址和端口。";
        if (result == ProfileFileResult::UnsupportedProtocol)
        {
            message = "请选择 aTrust 或 EasyConnect 配置文件。";
        }
        else if (result == ProfileFileResult::NewerVersion)
        {
            message = "此配置文件需要更新版本的 EZ4Connectpro。";
        }
        else if (result == ProfileFileResult::WriteFailed)
        {
            message = "无法保存导入的配置。请检查配置目录是否可写。";
        }
        QMessageBox::warning(this, "导入配置文件", message);
        return;
    }
    settings = profileService->settings();
    currentProfileId = profileService->currentProfileId();
    ui->profileSearch->clear();
    ui->protocolFilter->setCurrentIndex(0);
    refreshProfiles();
    qInfo().noquote() << "已导入配置「" + profileService->currentVpnProfile().name + "」。";
}

void MainWindow::exportConfiguration()
{
    const QString selectedPath = settings->fileName();
    QFileDialog dialog(this, "导出当前配置（含已保存的账号和凭据）",
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation), "INI 配置文件 (*.ini)");
    dialog.setAcceptMode(QFileDialog::AcceptSave);
    dialog.setDefaultSuffix("ini");
    dialog.selectFile(QFileInfo(selectedPath).fileName());
    if (dialog.exec() != QDialog::Accepted || quitting)
    {
        return;
    }
    if (settings->fileName() != selectedPath)
    {
        QMessageBox::warning(this, "导出当前配置", "当前配置已切换，请重新选择要导出的配置。");
        return;
    }
    const QString path = dialog.selectedFiles().value(0);
    if (QFileInfo(path).suffix().compare("ini", Qt::CaseInsensitive) != 0)
    {
        QMessageBox::warning(this, "导出当前配置", "请选择扩展名为 .ini 的保存位置。");
        return;
    }
    const auto result = profileService->exportCurrentProfile(path);
    if (result != ProfileFileResult::Success)
    {
        QMessageBox::warning(this, "导出当前配置", result == ProfileFileResult::ProtectedDestination
            ? "此位置是正在使用的配置文件，请选择其他保存位置。"
            : "无法写入配置文件，请检查保存位置和权限。");
        return;
    }
    qInfo().noquote() << "已导出配置「" + profileService->currentVpnProfile().name + "」。";
}

void MainWindow::editSelectedProfile()
{
    if (selectedConnectionIsActive() || !profileService->currentVpnProfile().isSupported())
    {
        return;
    }
    if (settingWindow)
    {
        settingWindow->raise();
        settingWindow->activateWindow();
        return;
    }
    settingWindow = new SettingWindow(this, settings, currentProfileId);
    connect(settingWindow, &QDialog::finished, this, &MainWindow::refreshProfiles);
    settingWindow->show();
}

void MainWindow::startSelectedProfile()
{
    startProfile(profileService->currentVpnProfile());
}

void MainWindow::startProfile(const VpnProfile &profile)
{
    if (!profile.isSupported())
    {
        return;
    }
    auto *runtime = coordinator->connectionFor(profile.settingsPath);
    if (!runtime)
    {
        runtime = coordinator->createConnection(profile);
        runtime->controls = new ConnectionUiController(
            this, runtime->session, systemProxySession, runtime->auth, applicationLogger,
            [runtime]() { return runtime->settings; },
            [runtime]() { return runtime->profileId; },
            [this, runtime]() { return coordinator->reserveNetwork(runtime); },
            [this, runtime](const QString &, const QString &content, QSystemTrayIcon::MessageIcon icon)
            {
                showNotification(readVpnProfile(*runtime->settings, runtime->profileId).name, content, icon);
            }, runtime);
        connect(runtime->session, &ConnectionSession::stateChanged, this, [this, runtime](ConnectionState state)
        {
            if (runtime == selectedConnection())
            {
                updateConnectionState(state);
            }
            else
            {
                updateTray();
            }
            refreshProfileList();
        });
        connect(runtime->session, &ConnectionSession::httpProxyReadyChanged, this, [this, runtime]()
        {
            if (runtime == selectedConnection())
            {
                updateConnectionState(runtime->session->state());
            }
            else
            {
                updateTray();
            }
        });
        connect(runtime->session, &ConnectionSession::finished, this, [this](ZJU_ERROR)
        {
            if (quitting && !coordinator->hasActiveConnections())
            {
                qApp->quit();
            }
        });
    }
    if (!runtime->session->isActive())
    {
        settings->sync();
        runtime->settings->sync();
    }
    runtime->controls->handleConnectClicked();
}
