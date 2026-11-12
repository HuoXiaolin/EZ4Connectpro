#include <QCoreApplication>
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
    const auto address = [&args](const QString &option)
    {
        const int index = args.indexOf(option);
        return index > 0 && index + 1 < args.size() ? args[index + 1] : QString{};
    };
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
