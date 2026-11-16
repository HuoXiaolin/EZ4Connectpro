#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
#include <functional>
#include "presentation/dialogs/devicetrustdialog/devicetrustdialog.h"

namespace
{
QJsonObject fixture()
{
    return {{"selfId", "this-computer"}, {"deviceTrusted", false}, {"currentTrustDeviceCount", 2},
        {"trustDeviceConfig", QJsonObject{{"enable",true},{"enableSelfBind",1},{"limit",2}}}, {"data", QJsonArray{
        QJsonObject{{"id", "retired-desktop"}, {"deviceName", "旧办公台式机"}, {"os", "Windows"},
            {"osVersion", "10"}, {"lastLoginTime", "2026-10-28 09:15:00"}, {"lastLoginIp", "192.0.2.10"}, {"onlineStatus", false}},
        QJsonObject{{"id", "travel-notebook"}, {"deviceName", "出差笔记本（保留使用）"}, {"os", "Windows"},
            {"osVersion", "11"}, {"lastLoginTime", "2026-11-16 08:30:00"}, {"lastLoginIp", "192.0.2.20"}, {"onlineStatus", true}}
    }}};
}
void save(const QString &path, const QJsonObject &value)
{
    QFile file(path); if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) qFatal("fixture write failed");
    file.write(QJsonDocument(value).toJson(QJsonDocument::Compact));
}
QByteArray read(const QString &path)
{
    QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll();
}
bool waitFor(const std::function<bool()> &predicate, int limit = 4000)
{
    QElapsedTimer timer; timer.start();
    while (!predicate() && timer.elapsed() < limit) { QApplication::processEvents(); QThread::msleep(5); }
    return predicate();
}
bool check(bool result, const char *message) { if (!result) qCritical("%s", message); return result; }

int worker(const QStringList &args)
{
    const auto option = [&args](const QString &name) { const int i=args.indexOf(name); return i>=0 && i+1<args.size() ? args[i+1] : QString{}; };
    const QString file = option("-client-data-file"), action = option("-device-action"), mode = option("-server");
    if (mode == "slow.example") { QThread::sleep(8); return 1; }
    if (mode == "expired.example") { std::fputs("login method is nil, but user is not logged in; private-cookie", stderr); return 1; }
    if (mode == "large.example") { const QByteArray huge(3*1024*1024, 'x'); std::fwrite(huge.constData(),1,huge.size(),stdout); return 0; }
    if (mode == "malformed.example") { std::puts("{\"unexpected\":true}"); return 0; }
    auto current = QJsonDocument::fromJson(read(file)).object();
    if (action == "list")
    {
        const auto bytes = QJsonDocument(current).toJson(QJsonDocument::Compact); std::puts(bytes.constData()); return 0;
    }
    if (mode == "limit.example") { std::fputs("trustDevice failed with code 75500311: private-cookie",stderr); return 1; }
    if (mode == "replace-denied.example") { std::fputs("trustDevice failed with code 75510008: private-cookie",stderr); return 1; }
    if (mode == "unchanged.example") { std::puts("{\"changed\":true}"); return 0; }
    QFile log(file + ".actions"); if (!log.open(QIODevice::WriteOnly | QIODevice::Append)) return 3;
    log.write((action + ' ' + option("-device-record-id") + '\n').toUtf8());
    auto rows = current.value("data").toArray();
    if (action == "untrust" || action == "replace-self")
    {
        const QString id = option("-device-record-id");
        for (int i=rows.size()-1;i>=0;--i) if (rows[i].toObject().value("id").toString()==id) rows.removeAt(i);
    }
    if (action == "trust-self" || (action == "replace-self" && mode != "partial.example"))
    {
        current.insert("deviceTrusted",true);
        rows.append(QJsonObject{{"id","this-computer"},{"deviceName","当前办公电脑"},{"os","Windows"},{"osVersion","11"}});
    }
    else if (action != "untrust" && action != "replace-self") return 2;
    current.insert("currentTrustDeviceCount",rows.size());
    current.insert("data",rows); save(file,current); std::puts("{\"changed\":true}"); return 0;
}
}

int main(int argc, char **argv)
{
    QStringList args; for(int i=0;i<argc;++i) args << QString::fromLocal8Bit(argv[i]);
    if(args.contains("-device-action")) { QCoreApplication app(argc,argv); return worker(app.arguments()); }
    QApplication app(argc,argv);
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(QIcon(":/resource/icon.png"));
    const bool dark = qEnvironmentVariableIsSet("EZ4_DEVICE_TEST_DARK");
    app.setStyleSheet(QString::fromUtf8(read(":/resource/mainwindow.qss"))
        + QString::fromUtf8(read(dark ? ":/resource/mainwindow-dark.qss" : ":/resource/mainwindow-light.qss")));
    bool ok = true;
    DeviceTrust::Snapshot parsed;
    QString error;
    ok &= check(DeviceTrust::parseSnapshot(QJsonDocument(fixture()).toJson(),parsed,error) && parsed.devices.size()==2 && !parsed.selfTrusted && parsed.canReplace(),"valid full-quota device list rejected");
    auto duplicate=fixture(); auto rows=duplicate.value("data").toArray(); rows.append(rows[0]); duplicate.insert("data",rows);
    ok &= check(!DeviceTrust::parseSnapshot(QJsonDocument(duplicate).toJson(),parsed,error) && parsed.devices.size()==2,"duplicate ID accepted or partial snapshot leaked");
    auto missing=fixture(); missing.remove("data");
    ok &= check(!DeviceTrust::parseSnapshot(QJsonDocument(missing).toJson(),parsed,error),"missing list treated as empty");
    auto empty=fixture(); empty.insert("data",QJsonArray{});
    ok &= check(DeviceTrust::parseSnapshot(QJsonDocument(empty).toJson(),parsed,error) && parsed.devices.isEmpty(),"empty list rejected");
    ok &= check(DeviceTrust::errorMessage("trustDevice failed with code 75500311: secret").contains("已达上限"),"quota error not explained");
    ok &= check(!DeviceTrust::errorMessage("login method is nil, but user is not logged in; secret").contains("secret"),"private error data leaked");
    QTemporaryDir temp;
    const QString file=temp.filePath("devices.json"); save(file,fixture());
    VpnProfile profile; profile.name="办公线路"; profile.server="office.example"; profile.protocolId="atrust";
    auto *client = new DeviceTrust::Client(profile.server,443,file,nullptr,app.applicationFilePath());
    DeviceTrustDialog dialog(profile,nullptr,client);
    dialog.show();
    auto *table=dialog.findChild<QTableWidget *>("deviceTrustTable");
    auto *bind=dialog.findChild<QPushButton *>("deviceTrustAddSelf");
    auto *unbind=dialog.findChild<QPushButton *>("deviceTrustUnbind");
    auto *status=dialog.findChild<QLabel *>("deviceTrustStatus");
    ok &= check(waitFor([&] { return table->rowCount()==2 && unbind->text()=="解绑并添加本机…"; }) && !bind->isEnabled(),"full quota replacement not offered");
    table->selectRow(0);
    const QString captures=qEnvironmentVariable("EZ4_DEVICE_CAPTURE_DIR");
    if(!captures.isEmpty())
    {
        QDir().mkpath(captures); QApplication::processEvents();
        ok &= dialog.grab().save(captures+"/devices.png");
        dialog.resize(680,400); QApplication::processEvents();
        ok &= dialog.grab().save(captures+"/devices-small.png");
        dialog.resize(850,450);
    }
    const auto answer = [&](QMessageBox::StandardButton choice, const QString &expectedId, bool replace)
    {
        QTimer::singleShot(0,[&,choice,expectedId,replace]()
        {
            auto *box=dialog.findChild<QMessageBox *>("deviceTrustConfirmation");
            if(!box) { ok=false; return; }
            ok &= check(box->informativeText().contains(expectedId),"confirmation omitted target ID");
            ok &= check(box->text().contains("并添加本机")==replace,"confirmation did not describe both replacement effects");
            if (!captures.isEmpty() && replace && choice==QMessageBox::Cancel)
                ok &= box->grab().save(captures+"/devices-confirm.png");
            box->button(choice)->click();
        });
        unbind->click();
    };
    answer(QMessageBox::Cancel,"retired-desktop",true);
    ok &= check(!QFileInfo::exists(file+".actions") && table->rowCount()==2,"cancel still removed device");
    answer(QMessageBox::Yes,"retired-desktop",true);
    ok &= check(waitFor([&] { return bind->text()=="本机已授信" && status->text().contains("换绑已完成"); }) && table->rowCount()==2,"replacement was not verified after refresh");
    ok &= check(read(file+".actions")=="replace-self retired-desktop\n","replacement fell back to separate mutations or targeted wrong ID");
    table->selectRow(0);
    answer(QMessageBox::Yes,"travel-notebook",false);
    ok &= check(waitFor([&] { return table->rowCount()==1 && status->text().contains("已解绑"); }),"trusted terminal unbind did not refresh list");
    auto available=fixture(); auto config=available.value("trustDeviceConfig").toObject();config.insert("limit",3);available.insert("trustDeviceConfig",config);
    save(file,available);client->refresh();
    ok &= check(waitFor([&] {return bind->isEnabled() && table->rowCount()==2;}),"available quota did not allow normal bind");
    bind->click();
    ok &= check(waitFor([&] { return table->rowCount()==3 && bind->text()=="本机已授信"; }) && !bind->isEnabled(),"bind-self did not reflect server state");
    ok &= check(read(file+".actions")=="replace-self retired-desktop\nuntrust travel-notebook\ntrust-self \n","unexpected mutation after bind");
    dialog.reject();

    for (const auto &mode : {"expired.example","malformed.example","large.example","slow.example"})
    {
        DeviceTrust::Client backend(mode,443,file,nullptr,app.applicationFilePath(),QString(mode)=="slow.example"?150:4000);
        QString failure;
        QObject::connect(&backend,&DeviceTrust::Client::failed,[&](const QString &message){failure=message;});
        backend.refresh(); backend.refresh();
        ok &= check(waitFor([&]{return !failure.isEmpty();}) && !backend.busy() && !failure.contains("private-cookie"),"error/timeout handling failed");
    }
    for (const auto &mode : {"unchanged.example", "partial.example", "replace-denied.example"})
    {
        save(file,fixture());
        DeviceTrustDialog replacement(profile,nullptr,new DeviceTrust::Client(mode,443,file,nullptr,app.applicationFilePath()));
        replacement.show();
        auto *rows=replacement.findChild<QTableWidget *>("deviceTrustTable");
        auto *button=replacement.findChild<QPushButton *>("deviceTrustUnbind");
        auto *message=replacement.findChild<QLabel *>("deviceTrustStatus");
        ok &= check(waitFor([&]{return rows->rowCount()==2;}),"replacement failure fixture not loaded");rows->selectRow(0);
        QTimer::singleShot(0,[&]{auto *box=replacement.findChild<QMessageBox *>("deviceTrustConfirmation");if(box)box->button(QMessageBox::Yes)->click();else ok=false;});
        button->click();
        const QString expected=QString(mode)=="partial.example"?"旧设备已移除":QString(mode)=="unchanged.example"?"尚未确认换绑完成":"75510008";
        ok &= check(waitFor([&]{return message->text().contains(expected);}) && !message->text().contains("换绑已完成"),"partial or rejected change claimed completion");
        replacement.reject();
    }
    save(file,available);
    DeviceTrustDialog limited(profile,nullptr,new DeviceTrust::Client("limit.example",443,file,nullptr,app.applicationFilePath()));
    limited.show();
    auto *limitedBind=limited.findChild<QPushButton *>("deviceTrustAddSelf");
    auto *limitedTable=limited.findChild<QTableWidget *>("deviceTrustTable");
    auto *limitedStatus=limited.findChild<QLabel *>("deviceTrustStatus");
    ok &= check(waitFor([&]{return limitedBind->isEnabled();}),"limit fixture load failed");
    limitedBind->click();
    ok &= check(waitFor([&]{return limitedStatus->text().contains("75500311") && limitedBind->isEnabled();})
        && limitedTable->rowCount()==2 && limitedBind->text()=="添加本机为授信设备", "rejected bind was treated as success or failed to refresh");
    if(!captures.isEmpty()) { QApplication::processEvents(); ok &= limited.grab().save(captures+"/devices-limit.png"); }
    // A cache removed between requests must stop at one failed refresh, not
    // retain the last mutation action and schedule refreshes indefinitely.
    QFile::remove(file);
    limitedBind->click();
    ok &= check(waitFor([&]{return limitedStatus->text().contains("列表刷新失败");}),"missing cache did not settle after refresh");
    ok &= check(!limitedBind->isEnabled(),"mutation remained available without login cache");
    limited.reject();
    std::printf("Device management parser, cancellation, selected unbind, self bind, refresh, rejection and timeout: %s\n",ok?"PASS":"FAIL");
    return ok ? 0 : 1;
}
