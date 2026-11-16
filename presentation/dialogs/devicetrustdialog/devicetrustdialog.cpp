#include "devicetrustdialog.h"
#include "infrastructure/storage/applicationpaths.h"
#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

DeviceTrustDialog::DeviceTrustDialog(const VpnProfile &profile, QWidget *parent, DeviceTrust::Client *backend)
    : QDialog(parent), client(backend ? backend : new DeviceTrust::Client(profile.server, profile.port,
        ApplicationPaths::clientDataFile(profile.id, false), this))
{
    client->setParent(this);
    setObjectName("deviceTrustDialog");
    setWindowTitle("授信设备管理 - EZ4Connectpro");
    setWindowModality(Qt::WindowModal);
    resize(850, 450);
    setMinimumSize(680, 400);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);
    auto *title = new QLabel("授信设备管理", this);
    title->setObjectName("deviceTrustTitle");
    auto titleFont = title->font(); titleFont.setPixelSize(20); titleFont.setBold(true); title->setFont(titleFont);
    layout->addWidget(title);
    auto *scope = new QLabel(profile.name + " · " + profile.server + ':' + QString::number(profile.port), this);
    scope->setTextFormat(Qt::PlainText); scope->setWordWrap(true);
    layout->addWidget(scope);
    auto *toolbar = new QHBoxLayout;
    summary = new QLabel("正在读取已授信设备…", this);
    summary->setObjectName("deviceTrustSummary");
    summary->setWordWrap(true);
    toolbar->addWidget(summary, 1);
    refreshButton = new QPushButton("刷新", this);
    refreshButton->setObjectName("deviceTrustRefresh");
    trustButton = new QPushButton("添加本机为授信设备", this);
    trustButton->setObjectName("deviceTrustAddSelf");
    toolbar->addWidget(refreshButton); toolbar->addWidget(trustButton);
    layout->addLayout(toolbar);
    table = new QTableWidget(0, 5, this);
    table->setObjectName("deviceTrustTable");
    table->setHorizontalHeaderLabels({"设备名称", "系统", "最近登录", "登录 IP", "在线状态"});
    table->setSelectionBehavior(QAbstractItemView::SelectRows);
    table->setSelectionMode(QAbstractItemView::SingleSelection);
    table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table->setAlternatingRowColors(false);
    table->setShowGrid(false);
    table->setWordWrap(false);
    table->setTextElideMode(Qt::ElideRight);
    table->verticalHeader()->hide();
    table->verticalHeader()->setDefaultSectionSize(38);
    table->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    table->horizontalHeaderItem(2)->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    table->setColumnWidth(1, 122); table->setColumnWidth(2, 172);
    table->setColumnWidth(3, 120); table->setColumnWidth(4, 78);
    table->setStyleSheet("QTableWidget::item { padding: 4px 6px; }");
    table->setAccessibleName("已授信设备列表");
    layout->addWidget(table, 1);
    auto *selection = new QHBoxLayout;
    selection->addWidget(new QLabel("所选设备标识", this));
    identifier = new QLineEdit(this);
    identifier->setObjectName("deviceTrustIdentifier");
    identifier->setReadOnly(true);
    identifier->setPlaceholderText("选择设备后可核对并复制完整标识");
    selection->addWidget(identifier, 1);
    layout->addLayout(selection);
    status = new QLabel(this);
    status->setObjectName("deviceTrustStatus");
    status->setTextFormat(Qt::PlainText);
    status->setWordWrap(true);
    status->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(status);
    auto *footer = new QHBoxLayout;
    untrustButton = new QPushButton("解绑所选设备…", this);
    untrustButton->setObjectName("deviceTrustUnbind");
    footer->addWidget(untrustButton); footer->addStretch();
    closeButton = new QPushButton("关闭", this);
    closeButton->setObjectName("deviceTrustClose");
    footer->addWidget(closeButton);
    layout->addLayout(footer);
    for (auto *button : {refreshButton, trustButton, untrustButton, closeButton})
    {
        button->setMinimumHeight(32);
        button->setAutoDefault(false);
    }
    connect(refreshButton, &QPushButton::clicked, this, &DeviceTrustDialog::refresh);
    connect(closeButton, &QPushButton::clicked, this, &DeviceTrustDialog::reject);
    connect(table, &QTableWidget::itemSelectionChanged, this, &DeviceTrustDialog::updateActions);
    connect(untrustButton, &QPushButton::clicked, this, &DeviceTrustDialog::untrustSelected);
    connect(trustButton, &QPushButton::clicked, this, [this]()
    {
        if (!validSnapshot || client->busy() || snapshot.selfTrusted) return;
        expectedAction = DeviceTrust::Client::Action::TrustSelf;
        expectedId.clear();
        completion = "添加本机的请求已成功，已刷新设备状态。";
        setStatus("正在添加本机为授信设备…");
        client->trustSelf();
    });
    connect(client, &DeviceTrust::Client::busyChanged, this, &DeviceTrustDialog::updateActions);
    connect(client, &DeviceTrust::Client::loaded, this, &DeviceTrustDialog::display);
    connect(client, &DeviceTrust::Client::changed, this, [this]()
    {
        verifyChange = true;
        validSnapshot = false;
        setStatus("请求已返回，正在核对设备状态…");
        QTimer::singleShot(0, client, &DeviceTrust::Client::refresh);
    });
    connect(client, &DeviceTrust::Client::failed, this, [this](const QString &message)
    {
        verifyChange = false;
        validSnapshot = false;
        summary->setText("设备状态待刷新确认");
        if (client->action() != DeviceTrust::Client::Action::List)
        {
            completion = message;
            setStatus(message + " 正在刷新设备状态…");
            QTimer::singleShot(0, client, &DeviceTrust::Client::refresh);
        }
        else
        {
            setStatus(completion.isEmpty() ? message : completion + "\n列表刷新失败：" + message);
            completion.clear();
        }
        updateActions();
    });
    updateActions();
    QTimer::singleShot(0, this, &DeviceTrustDialog::refresh);
}

void DeviceTrustDialog::setStatus(const QString &text) { status->setText(text); }

void DeviceTrustDialog::refresh()
{
    if (client->busy()) return;
    completion.clear();
    validSnapshot = false;
    setStatus("正在读取服务器上的授信设备…");
    client->refresh();
}

void DeviceTrustDialog::display(const DeviceTrust::Snapshot &value)
{
    const QString selectedId = identifier->text();
    snapshot = value;
    validSnapshot = true;
    table->setRowCount(0);
    for (const auto &device : snapshot.devices)
    {
        const int row = table->rowCount();
        table->insertRow(row);
        const QStringList values{device.name + (device.self ? "（本机）" : ""), device.system,
            device.lastLogin, device.address, device.onlineKnown ? (device.online ? "在线" : "离线") : ""};
        for (int column = 0; column < values.size(); ++column)
        {
            auto *item = new QTableWidgetItem(values[column].isEmpty() ? "—" : values[column]);
            item->setToolTip(values[column]);
            if (column == 0) item->setData(Qt::UserRole, device.id);
            if (column == 2) item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            table->setItem(row, column, item);
        }
        if (device.id == selectedId) table->selectRow(row);
    }
    const QString quantity = snapshot.limit > 0 && snapshot.trustedCount >= 0
        ? QString("%1 / %2").arg(snapshot.trustedCount).arg(snapshot.limit) : QString::number(snapshot.devices.size());
    summary->setText(QString("已授信 %1 台 · 本机%2").arg(quantity, snapshot.selfTrusted ? "已授信" : "未授信"));
    if (verifyChange)
    {
        bool removed = true;
        for (const auto &device : snapshot.devices) if (device.id == expectedId) removed = false;
        if (expectedAction == DeviceTrust::Client::Action::ReplaceSelf)
            completion = removed && snapshot.selfTrusted ? "换绑已完成：所选旧设备已解绑，本机已授信。"
                : removed ? "旧设备已移除，但服务器尚未确认本机授信。请核对当前状态后再操作。"
                : "服务器尚未确认换绑完成，已刷新当前列表，请勿重复提交。";
        else if (expectedAction == DeviceTrust::Client::Action::TrustSelf && !snapshot.selfTrusted)
            completion = "服务器尚未确认本机授信，已刷新当前列表，请勿重复提交。";
        else if (expectedAction == DeviceTrust::Client::Action::Untrust && !removed)
            completion = "服务器尚未确认解绑完成，已刷新当前列表，请勿重复提交。";
        verifyChange = false;
    }
    setStatus(!completion.isEmpty() ? completion : snapshot.devices.isEmpty()
        ? "暂无已授信设备，可点击「添加本机为授信设备」。"
        : snapshot.canReplace() ? "授信名额已满。选择不再使用的旧设备，点击「解绑并添加本机」完成换绑。"
        : !snapshot.selfTrusted ? "可先添加本机为授信设备，再管理旧设备；操作受网关策略限制。"
        : "选择不再使用的设备进行解绑。解绑后，该设备可能需要重新认证或申请授信。" );
    completion.clear();
    updateActions();
}

void DeviceTrustDialog::updateActions()
{
    const bool busy = client->busy();
    const auto selected = table->selectionModel()->selectedRows();
    const bool hasSelection = selected.size() == 1 && table->item(selected.first().row(), 0);
    identifier->setText(hasSelection ? table->item(selected.first().row(), 0)->data(Qt::UserRole).toString() : QString{});
    refreshButton->setEnabled(!busy);
    trustButton->setEnabled(!busy && validSnapshot && !snapshot.selfTrusted && !snapshot.canReplace());
    trustButton->setText(validSnapshot && snapshot.selfTrusted ? "本机已授信" : "添加本机为授信设备");
    trustButton->setToolTip(snapshot.canReplace() ? "请选择旧设备，再点击「解绑并添加本机」。" : QString{});
    untrustButton->setText(validSnapshot && snapshot.canReplace() ? "解绑并添加本机…" : "解绑所选设备…");
    untrustButton->setEnabled(!busy && validSnapshot && hasSelection && (snapshot.selfTrusted || snapshot.canReplace()));
    table->setEnabled(!busy);
    closeButton->setEnabled(!busy || client->action() == DeviceTrust::Client::Action::List);
}

void DeviceTrustDialog::untrustSelected()
{
    if (!validSnapshot || client->busy() || identifier->text().isEmpty()) return;
    const bool replace = snapshot.canReplace();
    if (!snapshot.selfTrusted && !replace) return;
    const QString id = identifier->text();
    for (const auto &device : snapshot.devices)
    {
        if (device.id != id) continue;
        QMessageBox confirm(QMessageBox::Warning, replace ? "确认换绑到本机" : "确认解绑设备", {}, QMessageBox::Yes | QMessageBox::Cancel, this);
        confirm.setObjectName("deviceTrustConfirmation");
        confirm.setTextFormat(Qt::PlainText);
        confirm.setText("解绑「" + device.name + (device.self ? "（本机）" : "")
            + (replace ? "」并添加本机为授信设备？" : "」的授信？"));
        confirm.setInformativeText("设备标识：" + device.id + "\n最近登录：" + (device.lastLogin.isEmpty() ? "未知" : device.lastLogin)
            + (replace ? "\n\n将通过网关换绑流程移除这台旧设备，并为本机申请授信。"
                       : "\n\n将移除这台设备的授信。")
            + "原设备的在线连接可能被注销，下次使用需要重新认证或申请授信。"
            + (device.self ? " 当前本机的 VPN 连接也可能中断。" : ""));
        confirm.button(QMessageBox::Yes)->setText(replace ? "确认换绑" : "确认解绑");
        confirm.button(QMessageBox::Cancel)->setText("取消");
        confirm.setDefaultButton(QMessageBox::Cancel);
        if (confirm.exec() != QMessageBox::Yes) return;
        expectedAction = replace ? DeviceTrust::Client::Action::ReplaceSelf : DeviceTrust::Client::Action::Untrust;
        expectedId = id;
        completion = replace ? "换绑请求已返回，正在核对实际状态。" : "所选设备已解绑，已刷新设备状态。";
        setStatus(replace ? "正在解绑旧设备并添加本机…" : "正在解绑所选设备…");
        if (replace) client->replaceSelf(id);
        else client->untrust(id);
        return;
    }
}

void DeviceTrustDialog::reject()
{
    if (client->busy() && client->action() != DeviceTrust::Client::Action::List) return;
    client->cancel();
    QDialog::reject();
}
