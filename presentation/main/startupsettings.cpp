#include "mainwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTimer>

#include "infrastructure/platform/autostart.h"
#include "infrastructure/platform/privileges.h"
#include "infrastructure/settings/profilemanager.h"

void MainWindow::showStartupSettings()
{
    ProfileManager storage;
    const auto saved = storage.startupOptions();
    using Status = AutoStart::RegistrationStatus;
    const auto registered = AutoStart::registrationStatus();

    QDialog dialog(this);
    dialog.setObjectName("startupSettingsDialog");
    dialog.setWindowTitle("启动设置");
    dialog.setWindowFlag(Qt::WindowContextHelpButtonHint, false);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(22, 20, 22, 16);
    layout->setSpacing(10);

    auto *enabled = new QCheckBox("登录系统后启动 EZ4Connectpro", &dialog);
    enabled->setObjectName("startupEnabledCheckBox");
    enabled->setChecked(saved.enabled);
    layout->addWidget(enabled);
    auto *silent = new QCheckBox("静默启动，直接进入托盘", &dialog);
    silent->setObjectName("startupSilentCheckBox");
    silent->setChecked(saved.silent);
    layout->addWidget(silent);

    layout->addWidget(new QLabel("开机自动连接（可多选）", &dialog));
    auto *profiles = new QListWidget(&dialog);
    profiles->setObjectName("startupProfilesList");
    profiles->setAccessibleName("开机自动连接线路");
    QStringList shownIds;
    for (const auto &line : profileService->vpnProfiles())
    {
        if ((profileService->usesOverrideConfiguration() && line.settingsPath == settings->fileName())
            || line.server.isEmpty() || line.port < 1 || line.port > 65535)
        {
            continue;
        }
        auto *item = new QListWidgetItem(line.name + " · " + line.protocolName(), profiles);
        item->setData(Qt::UserRole, line.id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(saved.profileIds.contains(line.id) ? Qt::Checked : Qt::Unchecked);
        item->setToolTip(line.name + "\n" + line.server + ':' + QString::number(line.port));
        item->setSizeHint(QSize(0, 32));
        shownIds.append(line.id);
    }
    for (const auto &id : saved.profileIds)
    {
        if (shownIds.contains(id)) continue;
        auto *item = new QListWidgetItem((id.isEmpty() ? "默认配置" : id) + " · 线路不可用", profiles);
        item->setData(Qt::UserRole, id);
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
        item->setCheckState(Qt::Checked);
        item->setToolTip("此线路已删除或尚未配置有效网关，可取消勾选。");
        item->setSizeHint(QSize(0, 32));
    }
    profiles->setMinimumHeight(130);
    profiles->setMaximumHeight(230);
    layout->addWidget(profiles);
    auto *selectionHint = new QLabel(&dialog);
    selectionHint->setObjectName("startupSelectionHint");
    const auto updateSelectionHint = [profiles, selectionHint]()
    {
        int count = 0;
        for (int i = 0; i < profiles->count(); ++i)
        {
            count += profiles->item(i)->checkState() == Qt::Checked;
        }
        selectionHint->setText(count ? QString("已选择 %1 条线路").arg(count) : "不勾选线路时，仅启动软件。");
    };
    connect(profiles, &QListWidget::itemChanged, &dialog, updateSelectionHint);
    updateSelectionHint();
    layout->addWidget(selectionHint);

    QString statusText = "启动项尚未登记";
    if (registered == Status::Registered)
    {
        statusText = "启动项已登记";
    }
    else if (registered == Status::NeedsRepair)
    {
        statusText = "启动路径或参数已变更";
    }
    else if (registered == Status::Unknown)
    {
        statusText = "由系统登录项管理";
    }
    auto *form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->addRow("启动项状态", new QLabel(statusText, &dialog));
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->button(QDialogButtonBox::Save);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    const auto refresh = [=](bool checked)
    {
        silent->setEnabled(checked);
        profiles->setEnabled(checked);
        save->setText(checked && saved.enabled && registered != Status::Registered && registered != Status::Unknown
            ? "保存并修复" : "保存");
    };
    connect(enabled, &QCheckBox::toggled, &dialog, refresh);
    refresh(enabled->isChecked());

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
    {
        ProfileManager::StartupOptions options{enabled->isChecked(), silent->isChecked(), {}};
        for (int i = 0; i < profiles->count(); ++i)
        {
            const auto *item = profiles->item(i);
            if (item->checkState() == Qt::Checked)
            {
                options.profileIds.append(item->data(Qt::UserRole).toString());
            }
        }
        if (!storage.saveStartupOptions(options))
        {
            QMessageBox::warning(&dialog, "启动设置", "无法保存启动设置，请检查配置目录是否可写。");
            return;
        }

        const auto actual = AutoStart::registrationStatus();
        const auto expected = options.enabled ? Status::Registered : Status::Missing;
        if ((actual == Status::Unknown ? options.enabled != saved.enabled : actual != expected)
            && !AutoStart::setEnabled(options.enabled))
        {
            const bool restored = storage.saveStartupOptions(saved);
            QMessageBox::warning(&dialog, "启动设置", restored ? "系统启动项更新失败，已恢复先前的设置。"
                : "系统启动项更新失败，请重新打开启动设置核对。");
            return;
        }
        qInfo().noquote() << "已保存启动设置。";
        dialog.accept();
    });
    dialog.setMinimumWidth(440);
    dialog.exec();
}

void MainWindow::startStartupProfiles(const QStringList &profileIds)
{
    if (quitting) return;
    const auto available = profileService->vpnProfiles();
    QList<VpnProfile> selected;
    QStringList seen;
    for (const auto &id : profileIds)
    {
        if (seen.contains(id)) continue;
        seen.append(id);
        bool found = false;
        for (const auto &profile : available)
        {
            if (profile.id == id && profile.isSupported() && !profile.server.isEmpty()
                && profile.port > 0 && profile.port <= 65535)
            {
                selected.append(profile);
                found = true;
                break;
            }
        }
        if (!found) qWarning().noquote() << "跳过不可用的开机连接线路：" << (id.isEmpty() ? "默认配置" : id);
    }
#if defined(Q_OS_WIN)
    if (!Privileges::isElevated())
    {
        QList<VpnProfile> localOnly;
        for (const auto &profile : selected)
        {
            QSettings stored(profile.settingsPath, QSettings::IniFormat);
            if (preferredIntranetRouting(stored) == IntranetRouting::LocalProxy) localOnly.append(profile);
        }
        if (localOnly.size() != selected.size())
        {
            // Elevate the batch before starting any core, keeping all saved selections.
            if (!coordinator->hasActiveConnections() && Privileges::relaunchElevated({}, !isVisible(), true))
            {
                quitting = true;
                qApp->quit();
                return;
            }
            qWarning().noquote() << "开机连接未取得管理员权限，已跳过需要 TUN 的线路。";
            showNotification("开机连接", "TUN 线路需要管理员授权，可稍后在工作台连接。", QSystemTrayIcon::Warning);
            selected = localOnly;
        }
    }
#endif
    for (const auto &profile : selected)
    {
        QTimer::singleShot(0, this, [this, profile]()
        {
            const auto *runtime = coordinator->connectionFor(profile.settingsPath);
            if (!quitting && (!runtime || !runtime->session->isActive())) startProfile(profile);
        });
    }
}
