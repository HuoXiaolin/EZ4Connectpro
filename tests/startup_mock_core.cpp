#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <cstdio>

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    const auto args = app.arguments();
    if (args.contains("-version"))
    {
        std::puts("ZJU Connect v1.3.1-ez4tun1");
        return 0;
    }
    if (qEnvironmentVariableIsSet("EZ4_QA_SIGNATURE_FAILURE"))
    {
        std::puts("reportEnv failed with code 10000008: interface sig verify failed");
        std::puts("Login error: reportEnv failed with code 10000008");
        std::puts("VPN client setup error: reportEnv failed with code 10000008");
        return 1;
    }
    const auto address = [&args](const QString &option)
    {
        const int index = args.indexOf(option);
        return index > 0 && index + 1 < args.size() ? args[index + 1] : QString{};
    };
    if (args.contains("-device-action"))
    {
        QFile cache(address("-client-data-file"));
        if (!cache.open(QIODevice::ReadOnly)) return 3;
        auto data=QJsonDocument::fromJson(cache.readAll()).object();cache.close();
        const auto action=address("-device-action");
        if (action=="list") {std::puts(QJsonDocument(data).toJson(QJsonDocument::Compact).constData());return 0;}
        if (action!="replace-self" || address("-device-record-id")!="retired-desktop") return 4;
        auto rows=data.value("data").toArray();
        for (int i=rows.size()-1;i>=0;--i) if(rows[i].toObject().value("id").toString()=="retired-desktop")rows.removeAt(i);
        rows.append(QJsonObject{{"id","this-computer"},{"deviceName","Current device"}});
        data.insert("data",rows);data.insert("deviceTrusted",true);
        if(!cache.open(QIODevice::WriteOnly|QIODevice::Truncate))return 5;
        cache.write(QJsonDocument(data).toJson(QJsonDocument::Compact));cache.close();
        std::puts("{\"changed\":true}");return 0;
    }
    QTcpServer socks;
    QTcpServer http;
    const QString socksAddress = address("-socks-bind");
    const QString httpAddress = address("-http-bind");
    if (!socksAddress.startsWith("127.0.0.1:") || !httpAddress.startsWith("127.0.0.1:")
        || !socks.listen(QHostAddress::LocalHost, socksAddress.section(':', -1).toUShort())
        || !http.listen(QHostAddress::LocalHost, httpAddress.section(':', -1).toUShort())) return 2;
    for (auto *server : {&socks, &http})
    {
        QObject::connect(server, &QTcpServer::newConnection, server, [server]()
        {
            while (server->hasPendingConnections())
            {
                auto *socket = server->nextPendingConnection();
                socket->abort();
                socket->deleteLater();
            }
        });
    }
    std::puts("VPN client started");
    std::fflush(stdout);
    QTimer::singleShot(150, [&]()
    {
        std::printf("HTTP server listening on %s\n", httpAddress.toUtf8().constData());
        std::fflush(stdout);
    });
    return app.exec();
}
