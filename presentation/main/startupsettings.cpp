#include "mainwindow.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDebug>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "infrastructure/platform/autostart.h"
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
    enabled->setChecked(saved.enabled);
    layout->addWidget(enabled);
    auto *silent = new QCheckBox("静默启动，直接进入托盘", &dialog);
    silent->setChecked(saved.silent);
    layout->addWidget(silent);

    auto *profile = new QComboBox(&dialog);
    profile->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    profile->setMinimumContentsLength(18);
    profile->addItem("仅启动软件");
    for (const auto &line : profileService->vpnProfiles())
    {
        if ((profileService->usesOverrideConfiguration() && line.settingsPath == settings->fileName())
            || line.server.isEmpty() || line.port < 1 || line.port > 65535)
        {
            continue;
        }
        profile->addItem(line.name + " · " + line.protocolName(), line.id);
        if (saved.profileId == line.id)
        {
            profile->setCurrentIndex(profile->count() - 1);
        }
    }

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
    form->addRow("开机连接线路", profile);
    form->addRow("启动项状态", new QLabel(statusText, &dialog));
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dialog);
    auto *save = buttons->button(QDialogButtonBox::Save);
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);
    const auto refresh = [=](bool checked)
    {
        silent->setEnabled(checked);
        profile->setEnabled(checked);
        save->setText(checked && saved.enabled && registered != Status::Registered && registered != Status::Unknown
            ? "保存并修复" : "保存");
    };
    connect(enabled, &QCheckBox::toggled, &dialog, refresh);
    refresh(enabled->isChecked());

    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]()
    {
        ProfileManager::StartupOptions options{enabled->isChecked(), silent->isChecked(), std::nullopt};
        if (profile->currentIndex() > 0)
        {
            options.profileId = profile->currentData().toString();
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
    dialog.setMinimumWidth(400);
    dialog.exec();
}
