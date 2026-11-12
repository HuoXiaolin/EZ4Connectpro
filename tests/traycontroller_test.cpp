#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QMenu>
#include <QPainter>
#include <QStyleFactory>
#include <QSystemTrayIcon>
#include <QWidget>

#include "presentation/main/traycontroller.h"

namespace
{
bool check(bool result, const char *description)
{
    if (!result) qCritical() << description;
    return result;
}

QMenu *lineMenu(TrayController &tray, const QString &path)
{
    for (auto *action : tray.menu()->actions())
    {
        if (action->data().toString() == path) return action->menu();
    }
    return nullptr;
}

QColor badge(TrayController &tray, int size = 32)
{
    return tray.icon()->icon().pixmap(QSize(size, size), 1.0).toImage().pixelColor(size * 0.78, size * 0.18);
}

bool capture(TrayController &tray, const QString &directory, const QString &name, bool submenu = false)
{
    if (directory.isEmpty()) return true;
    QDir().mkpath(directory);
    auto *menu = submenu ? lineMenu(tray, "office.ini") : tray.menu();
    menu->popup(QPoint(60, 60));
    QApplication::processEvents();
    const bool saved = menu->grab().save(directory + '/' + name + ".png");
    menu->hide();
    QApplication::processEvents();
    return saved;
}
}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QApplication::setApplicationName("EZ4Connectpro tray test");
    QWidget parent;
    TrayController tray(&parent);
    const QString captures = qEnvironmentVariable("EZ4_TRAY_CAPTURE_DIR");
    TrayConnection office{{"office", "办公线路", "atrust", "office.example", 443, "office.ini", 11080, 11081}};
    office.routing = IntranetRouting::Automatic;
    TrayConnection backup{{"backup", "备用线路", "easyconnect", "backup.example", 443, "backup.ini", 11082, 11083}};
    const auto update = [&]() { tray.updateConnections({office, backup}, "backup.ini", false, false, false); };
    update();
    const QColor idle = badge(tray);
    bool ok = check(tray.icon()->toolTip().contains("未连接"), "idle tooltip missing");

    office.state = ConnectionState::Starting;
    update();
    const QColor yellow = badge(tray);
    ok &= check(yellow.red() > 180 && yellow.green() > 100 && yellow.blue() < 100 && yellow != idle,
                "starting connection must show a yellow badge even when another profile is selected");
    ok &= check(tray.icon()->toolTip().contains("办公线路 · 连接中 · TUN"), "starting tunnel is missing from tooltip");
    office.state = ConnectionState::Running;
    update();
    ok &= check(badge(tray) == yellow && tray.icon()->toolTip().contains("通道准备中"), "not-ready tunnel reported as connected");

    office.ready = true;
    update();
    const QColor green = badge(tray);
    ok &= check(green.green() > 120 && green.red() < 100 && green != yellow, "ready connection must show green");
    for (int size : {16, 20, 24, 32, 48})
    {
        ok &= check(badge(tray, size).green() > 120 && badge(tray, size).red() < 100, "badge missing at native DPI size");
    }
    backup.state = ConnectionState::Reconnecting;
    update();
    ok &= check(badge(tray) == yellow && tray.icon()->toolTip().contains("备用线路 · 重连中 · 本地代理")
        && tray.icon()->toolTip().contains("办公线路 · 已连接 · TUN"), "mixed ready/reconnecting state is misleading");
    ok &= capture(tray, captures, "tray-mixed");
    ok &= capture(tray, captures, "tray-channel", true);

    QString requestedPath;
    bool requestedStart = true;
    int requests = 0;
    QObject::connect(&tray, &TrayController::connectionRequested, [&](const QString &path, bool start)
    {
        requestedPath = path;
        requestedStart = start;
        ++requests;
    });
    auto *officeMenu = lineMenu(tray, "office.ini");
    auto *operate = officeMenu->findChild<QAction *>("trayOperateConnection");
    operate->trigger();
    ok &= check(requestedPath == "office.ini" && !requestedStart, "disconnect action targeted selected profile instead of named profile");
    tray.menu()->popup(QPoint(60, 60));
    QApplication::processEvents();
    office.state = ConnectionState::Stopping;
    update();
    operate->trigger();
    ok &= check(requests == 1 && !operate->isEnabled() && lineMenu(tray, "office.ini") == officeMenu,
                "open menu actions must remain stable and stopping must reject duplicate operations");
    office.state = ConnectionState::Disconnected;
    update();
    operate->trigger();
    ok &= check(requests == 2 && requestedStart, "disconnected action did not switch back to connect");
    tray.updateConnections({backup}, "backup.ini", false, false, false);
    operate->trigger();
    ok &= check(requests == 2, "removed profile retained a live action");
    tray.menu()->hide();
    QApplication::processEvents();
    ok &= check(!lineMenu(tray, "office.ini"), "removed menu was not retired after closing");

    backup.state = ConnectionState::Failed;
    update();
    ok &= check(badge(tray) == idle && tray.icon()->toolTip().contains("连接失败"), "failure retained success badge");
    backup.state = ConnectionState::Running;
    backup.ready = true;
    backup.ownsSystemProxy = true;
    tray.updateConnections({office, backup}, "backup.ini", false, true, false);
    ok &= check(tray.icon()->toolTip().contains("系统代理"), "system proxy channel is missing");

    QList<TrayConnection> many;
    for (int i = 0; i < 20; ++i)
    {
        auto item = backup;
        item.profile.name = QString(60, QChar(0x529e)) + "&" + QString::number(i);
        item.profile.settingsPath = QString::number(i) + ".ini";
        many.append(item);
    }
    tray.updateConnections(many, "0.ini", false, false, false);
    ok &= check(tray.icon()->toolTip().size() <= 127 && tray.icon()->toolTip().contains("右键查看")
        && tray.icon()->toolTip().contains("已连接 20 条"), "long multi-profile tooltip exceeded native limit or lost overflow indication");

    office.profile.name = "研发 & 运维";
    office.state = ConnectionState::Running;
    office.ready = true;
    update();
    ok &= check(lineMenu(tray, "office.ini")->title().contains("&&"), "profile name ampersand became a menu accelerator");
    ok &= capture(tray, captures, "tray-connected");
    app.setStyle(QStyleFactory::create("Fusion"));
    QPalette dark;
    dark.setColor(QPalette::Window, QColor("#202020"));
    dark.setColor(QPalette::Base, QColor("#202020"));
    dark.setColor(QPalette::Text, Qt::white);
    dark.setColor(QPalette::WindowText, Qt::white);
    dark.setColor(QPalette::Disabled, QPalette::WindowText, QColor("#aaaaaa"));
    app.setPalette(dark);
    ok &= capture(tray, captures, "tray-dark");
    tray.updateConnections({office, backup}, "office.ini", true, false, false);
    lineMenu(tray, "office.ini")->findChild<QAction *>("trayOperateConnection")->trigger();
    ok &= check(requests == 2, "shutdown still allows connection actions");
    if (ok) qInfo() << "Tray transitions, independent actions, live menu updates, DPI and tooltip limits passed";
    return ok ? 0 : 1;
}
