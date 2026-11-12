#include "traycontroller.h"

#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QPainter>
#include <QSystemTrayIcon>
#include <QTimer>

#include <algorithm>

namespace
{
QString shortText(const QString &text, int length)
{
    const QString value = text.simplified();
    if (value.size() <= length)
    {
        return value;
    }
    int end = length - 1;
    if (end > 0 && value.at(end - 1).isHighSurrogate())
    {
        --end;
    }
    return value.left(end) + QChar(0x2026);
}

QString menuText(const QString &text)
{
    return QString(text).replace('&', "&&");
}

QIcon badgedIcon(const QColor &color = {})
{
    QIcon icon;
    const QPixmap source(":/resource/icon.png");
    // Supply native sizes so the small status dot stays sharp at each Windows DPI.
    for (const int size : {16, 20, 24, 32, 40, 48, 64})
    {
        QPixmap pixmap(size, size);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.drawPixmap(0, 0, size, size, source);
        if (color.isValid())
        {
            painter.setRenderHint(QPainter::Antialiasing);
            const qreal diameter = size * 0.40;
            const qreal border = qMax(1.0, size / 16.0);
            painter.setPen(QPen(Qt::white, border));
            painter.setBrush(color);
            painter.drawEllipse(QRectF(size - diameter - border / 2, border / 2,
                                      diameter - border / 2, diameter - border / 2));
        }
        painter.end();
        icon.addPixmap(pixmap);
    }
    return icon;
}

bool isPending(const TrayConnection &connection)
{
    return connection.state == ConnectionState::Starting
        || connection.state == ConnectionState::Reconnecting
        || (connection.state == ConnectionState::Running && !connection.ready);
}
}

bool TrayConnection::isActive() const
{
    return state == ConnectionState::Starting || state == ConnectionState::Running
        || state == ConnectionState::Reconnecting || state == ConnectionState::Stopping;
}

QString TrayConnection::statusText() const
{
    switch (state)
    {
    case ConnectionState::Starting: return "连接中";
    case ConnectionState::Running: return ready ? "已连接" : "通道准备中";
    case ConnectionState::Reconnecting: return "重连中";
    case ConnectionState::Stopping: return "断开中";
    case ConnectionState::Interrupted: return "连接中断";
    case ConnectionState::Failed: return "连接失败";
    case ConnectionState::Disconnected: return "未连接";
    }
    return {};
}

QString TrayConnection::channelText() const
{
    if (routing != IntranetRouting::LocalProxy)
    {
        return "TUN";
    }
    return ownsSystemProxy ? "系统代理" : "本地代理";
}

TrayController::TrayController(QWidget *parent) : QObject(parent),
    trayIcon(new QSystemTrayIcon(this)), trayMenu(new QMenu(parent)),
    idleIcon(badgedIcon()), pendingIcon(badgedIcon(QColor("#e9b522"))),
    connectedIcon(badgedIcon(QColor("#22ad62")))
{
    trayIcon->setObjectName("connectionTrayIcon");
    trayMenu->setObjectName("connectionTrayMenu");
    trayMenu->setToolTipsVisible(true);
    summary = trayMenu->addAction("未连接");
    summary->setEnabled(false);
    trayMenu->addSeparator();
    rowsEnd = trayMenu->addSeparator();
    disconnectAll = trayMenu->addAction("断开全部连接", this, &TrayController::disconnectAllRequested);
    disconnectAll->setObjectName("trayDisconnectAll");
    auto *showWindow = trayMenu->addAction("显示主界面", this, &TrayController::showWindowRequested);
    trayMenu->setDefaultAction(showWindow);
    trayMenu->addSeparator();
    auto *files = trayMenu->addMenu("配置文件");
    configuration = files->menuAction();
    files->addAction("新建线路…", this, &TrayController::newProfileRequested);
    files->addAction("导入配置文件…", this, &TrayController::importRequested);
    exportAction = files->addAction("导出当前配置…", this, &TrayController::exportRequested);
    startupAction = trayMenu->addAction("启动设置…", this, &TrayController::startupSettingsRequested);
    trayMenu->addAction("打开日志文件", this, &TrayController::openLogRequested);
    releaseProxy = trayMenu->addAction("释放本工具代理", this, &TrayController::releaseProxyRequested);
    trayMenu->addSeparator();
    quitAction = trayMenu->addAction("退出 EZ4Connectpro", this, &TrayController::quitRequested);
    connect(trayMenu, &QMenu::aboutToShow, this, &TrayController::refresh);
    connect(trayMenu, &QMenu::aboutToHide, this, [this]()
    {
        QTimer::singleShot(0, this, &TrayController::refresh);
    });
    connect(trayIcon, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason)
    {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick
            || reason == QSystemTrayIcon::MiddleClick)
        {
            emit showWindowRequested();
        }
    });
    connect(trayIcon, &QSystemTrayIcon::messageClicked, this, &TrayController::showWindowRequested);
    trayIcon->setContextMenu(trayMenu);
    refresh();
    trayIcon->show();
}

QSystemTrayIcon *TrayController::icon() const { return trayIcon; }
QMenu *TrayController::menu() const { return trayMenu; }

void TrayController::updateConnections(const QList<TrayConnection> &value, const QString &selection,
                                     bool shuttingDown, bool enabled, bool busy)
{
    connections = value;
    selectedPath = selection;
    quitting = shuttingDown;
    proxyEnabled = enabled;
    proxyBusy = busy;
    refresh();
}

const TrayConnection *TrayController::connection(const QString &path) const
{
    for (const auto &item : connections)
    {
        if (item.profile.settingsPath == path)
        {
            return &item;
        }
    }
    return nullptr;
}

void TrayController::rebuildRows()
{
    QStringList paths;
    for (const auto &item : connections)
    {
        paths.append(item.profile.settingsPath);
    }
    // Never destroy actions under the pointer while a menu is open.
    if (paths == rowPaths || trayMenu->isVisible())
    {
        return;
    }
    for (const auto &row : rows)
    {
        delete row.menu;
    }
    rows.clear();
    rowPaths = paths;
    for (const auto &path : paths)
    {
        auto *menu = new QMenu(trayMenu);
        menu->setToolTipsVisible(true);
        menu->menuAction()->setData(path);
        trayMenu->insertMenu(rowsEnd, menu);
        Row row{menu, menu->addAction(QString{}), menu->addAction(QString{}), menu->addAction(QString{}), nullptr, nullptr};
        row.details->setEnabled(false);
        row.endpoint->setEnabled(false);
        row.ports->setEnabled(false);
        menu->addSeparator();
        row.operate = menu->addAction("连接此线路", this, [this, path]()
        {
            const auto *item = connection(path);
            if (item && !quitting && item->state != ConnectionState::Stopping)
            {
                emit connectionRequested(path, !item->isActive());
            }
        });
        row.operate->setObjectName("trayOperateConnection");
        row.show = menu->addAction("在工作台查看", this, [this, path]() { emit showProfileRequested(path); });
        row.show->setCheckable(true);
        rows.insert(path, row);
    }
}

QString TrayController::tooltip() const
{
    QString text = "EZ4Connectpro\n" + summary->text();
    QList<const TrayConnection *> visible;
    for (const auto &item : connections)
    {
        if (item.isActive() || item.state == ConnectionState::Failed || item.state == ConnectionState::Interrupted
            || item.profile.settingsPath == selectedPath)
        {
            visible.append(&item);
        }
    }
    std::stable_sort(visible.begin(), visible.end(), [](const auto *a, const auto *b)
    {
        const auto rank = [](const auto *item) { return isPending(*item) ? 0 : item->isActive() ? 1 : 2; };
        return rank(a) < rank(b);
    });
    for (int i = 0; i < visible.size(); ++i)
    {
        const auto &item = *visible[i];
        const QString line = "\n" + shortText(item.profile.name, 18) + " · " + item.statusText()
            + " · " + item.channelText();
        const QString overflow = "\n另 " + QString::number(visible.size() - i) + " 条，右键查看";
        // NOTIFYICONDATAW.szTip includes its NUL terminator in 128 UTF-16 units.
        if (text.size() + line.size() + (i + 1 < visible.size() ? overflow.size() : 0) > 127)
        {
            text += overflow;
            break;
        }
        text += line;
    }
    return text;
}

void TrayController::refresh()
{
    rebuildRows();
    int connected = 0;
    int pending = 0;
    int stopping = 0;
    for (const auto &item : connections)
    {
        connected += item.state == ConnectionState::Running && item.ready;
        pending += isPending(item);
        stopping += item.state == ConnectionState::Stopping;
    }
    QStringList counts;
    if (connected) counts << QString("已连接 %1 条").arg(connected);
    if (pending) counts << QString("连接中 %1 条").arg(pending);
    if (stopping) counts << QString("断开中 %1 条").arg(stopping);
    summary->setText(counts.isEmpty() ? "未连接" : counts.join(" · "));
    const int badge = pending ? 1 : connected ? 2 : 0;
    if (badge != displayedBadge)
    {
        displayedBadge = badge;
        trayIcon->setIcon(badge == 1 ? pendingIcon : badge == 2 ? connectedIcon : idleIcon);
    }
    trayIcon->setToolTip(tooltip());
    for (auto it = rows.cbegin(); it != rows.cend(); ++it)
    {
        const auto *item = connection(it.key());
        const auto &row = it.value();
        row.menu->setEnabled(item && !quitting);
        if (!item) continue;
        row.menu->setTitle(menuText(shortText(item->profile.name, 32)) + " · " + item->statusText());
        row.menu->menuAction()->setToolTip(item->profile.name.simplified() + "\n"
            + item->profile.protocolName() + " · " + item->channelText() + " · " + item->statusText());
        row.menu->setIcon(isPending(*item) ? pendingIcon : item->state == ConnectionState::Running && item->ready
            ? connectedIcon : idleIcon);
        row.details->setText(menuText(item->profile.protocolName() + " · " + item->channelText()));
        row.endpoint->setText(menuText(shortText(item->profile.server, 50)) + ':' + QString::number(item->profile.port));
        row.endpoint->setToolTip(item->profile.server + ':' + QString::number(item->profile.port));
        row.ports->setText(QString("SOCKS5 %1 · HTTP %2").arg(item->profile.socksPort).arg(item->profile.httpPort));
        row.ports->setVisible(item->state == ConnectionState::Running && item->ready);
        row.operate->setText(item->state == ConnectionState::Stopping ? "正在断开…"
            : isPending(*item) ? "取消连接" : item->isActive() ? "断开此线路" : "连接此线路");
        row.operate->setEnabled(!quitting && item->profile.isSupported() && item->state != ConnectionState::Stopping);
        row.show->setChecked(it.key() == selectedPath);
    }
    disconnectAll->setVisible(connected + pending + stopping > 0);
    disconnectAll->setEnabled(!quitting && connected + pending > 0);
    configuration->setEnabled(!quitting);
    const auto *selected = connection(selectedPath);
    exportAction->setEnabled(!quitting && selected && selected->profile.isSupported());
    exportAction->setText(selected ? "导出「" + menuText(shortText(selected->profile.name, 24)) + "」…" : "导出当前配置…");
    startupAction->setEnabled(!quitting);
    releaseProxy->setVisible(proxyEnabled);
    releaseProxy->setEnabled(!quitting && !proxyBusy);
    quitAction->setText(connected + pending + stopping > 0 ? "断开全部并退出" : "退出 EZ4Connectpro");
    quitAction->setEnabled(!quitting);
}
