#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>

#include "infrastructure/browser/browserssosession.h"
#include "infrastructure/browser/temporarybrowserssosession.h"

namespace
{
bool checksGatewayAndTicket()
{
    const QUrl gateway("https://vpn.example:443");
    const QList<QByteArray> accepted{
        "https://vpn.example/passport/v1/auth/cas?sfDomain=test&ticket=ST-a%2Bb",
        "https://vpn.example:443/passport/v1/auth/httpsOauth2?code=c%2Fd&state=123",
    };
    const QList<QByteArray> rejected{
        "https://other.example/passport/v1/auth/cas?ticket=ST-a",
        "http://vpn.example/passport/v1/auth/cas?ticket=ST-a",
        "https://vpn.example:8443/passport/v1/auth/cas?ticket=ST-a",
        "https://user@vpn.example/passport/v1/auth/cas?ticket=ST-a",
        "https://vpn.example/passport/v1/public/casLogin?ticket=ST-a",
        "https://vpn.example/passport/v1/auth/cas?code=ST-a",
        "https://vpn.example/passport/v1/auth/cas?ticket=",
        "https://vpn.example/passport/v1/auth/cas?ticket=a&ticket=b",
        "https://vpn.example/passport/v1/auth/cas?ticket=%0A",
        "https://vpn.example/passport/v1/auth/cas?ticket=ST-a#fragment",
    };
    for (const auto &url : accepted)
    {
        if (!BrowserSsoSession::isCallback(url, gateway))
        {
            qCritical() << "Rejected valid fixture callback" << url;
            return false;
        }
    }
    for (const auto &url : rejected)
    {
        if (BrowserSsoSession::isCallback(url, gateway))
        {
            qCritical() << "Accepted invalid fixture callback" << url;
            return false;
        }
    }
    return true;
}

int post(QNetworkAccessManager &network, const QUrl &url, const QByteArray &origin, const QByteArray &body)
{
    QNetworkRequest request(url);
    request.setTransferTimeout(3000);
    request.setRawHeader("Origin", origin);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
    QEventLoop loop;
    auto *reply = network.post(request, body);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    delete reply;
    return code;
}

bool checksIndependentSessions()
{
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    BrowserSsoSession first;
    BrowserSsoSession second;
    const QUrl gateway("https://vpn.example");
    const QUrl a = first.prepare(QUrl("https://login.example/login"), gateway);
    const QUrl b = second.prepare(QUrl("https://login.example/login"), gateway);
    QByteArray firstTicket;
    QByteArray secondTicket;
    QObject::connect(&first, &BrowserSsoSession::callbackReceived, &first, [&](const QByteArray &url) { firstTicket = url; });
    QObject::connect(&second, &BrowserSsoSession::callbackReceived, &second, [&](const QByteArray &url) { secondTicket = url; });

    const auto origin = [](const QUrl &url) { return "http://" + url.authority().toLatin1(); };
    const auto returned = [](const QUrl &url) { return url.resolved(QUrl("return")); };
    QUrl foreignPath = returned(a);
    foreignPath.setPath(returned(b).path());
    const QByteArray aTicket = "https://vpn.example/passport/v1/auth/cas?ticket=ST-a%2B1&sfDomain=test";
    const QByteArray bTicket = "https://vpn.example/passport/v1/auth/httpsOauth2?code=b%26c&state=9";
    return !a.isEmpty() && !b.isEmpty() && a != b
        && first.entryUrl() == a && second.entryUrl() == b
        && post(network, foreignPath, origin(a), aTicket) == 404
        && post(network, returned(a), "https://other.example", aTicket) == 400
        && post(network, returned(a), origin(a), "https://wrong.example/passport/v1/auth/cas?ticket=ST-a") == 400
        && post(network, returned(a), origin(a), aTicket) == 204
        && firstTicket == aTicket && first.entryUrl().isEmpty() && secondTicket.isEmpty()
        && post(network, returned(b), origin(b), bTicket) == 204
        && secondTicket == bTicket
        && post(network, returned(a), origin(a), bTicket) == 0;
}

// The isolated-desktop check uses a fake gateway for both browser login modes.
int runBrowserFixture(QCoreApplication &app, const QString &urlFile, const QString &temporaryProgram = {})
{
    QTcpServer server;
    server.setProxy(QNetworkProxy::NoProxy);
    if (!server.listen(QHostAddress::LocalHost))
    {
        return 1;
    }
    const QByteArray gateway = "http://127.0.0.1:" + QByteArray::number(server.serverPort());
    const QByteArray a = gateway + "/passport/v1/auth/cas?sfDomain=test&ticket=ST-a%2B1";
    const QByteArray b = gateway + "/passport/v1/auth/httpsOauth2?code=b%26c&state=9";
    int cookiesSeen = 0;
    bool gatewayHit = false;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]()
    {
        auto *client = server.nextPendingConnection();
        QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        QObject::connect(client, &QTcpSocket::readyRead, client, [&, client, request = QByteArray{}]() mutable
        {
            request += client->readAll();
            if (!request.contains("\r\n\r\n"))
            {
                return;
            }
            const QByteArray target = request.split(' ').value(1);
            if (target == "/cas" || target == "/oauth")
            {
                cookiesSeen += request.toLower().contains("cookie: fixture-existing-profile=1");
                client->write("HTTP/1.1 302 Found\r\nLocation: " + (target == "/cas" ? a : b)
                              + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
            else
            {
                gatewayHit |= target.startsWith("/passport/v1/auth/");
                client->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
            client->disconnectFromHost();
        });
    });
    BrowserSsoSession first;
    BrowserSsoSession second;
    TemporaryBrowserSsoSession temporaryFirst;
    TemporaryBrowserSsoSession temporarySecond;
    const QUrl host = QUrl::fromEncoded(gateway);
    const QUrl casLogin = QUrl::fromEncoded(gateway + "/cas");
    const QUrl oauthLogin = QUrl::fromEncoded(gateway + "/oauth");
    const bool temporary = !temporaryProgram.isEmpty();
    int verified = 0;
    const auto received = [&](const QByteArray &url, const QByteArray &expected)
    {
        if (url != expected)
        {
            app.exit(1);
        }
        else if (++verified == 2)
        {
            qInfo() << "Both original callbacks received. Gateway reached:" << gatewayHit << "Existing cookies:" << cookiesSeen;
            app.exit(!gatewayHit && cookiesSeen == (temporary ? 0 : 2) ? 0 : 1);
        }
    };
    if (temporary)
    {
        QObject::connect(&temporaryFirst, &TemporaryBrowserSsoSession::callbackReceived, &app,
                         [&](const QByteArray &url) { received(url, a); });
        QObject::connect(&temporarySecond, &TemporaryBrowserSsoSession::callbackReceived, &app,
                         [&](const QByteArray &url) { received(url, b); });
        const auto failed = [&](const QString &reason)
        {
            qCritical().noquote() << reason;
            app.exit(1);
        };
        QObject::connect(&temporaryFirst, &TemporaryBrowserSsoSession::failed, &app, failed);
        QObject::connect(&temporarySecond, &TemporaryBrowserSsoSession::failed, &app, failed);
        QTimer::singleShot(0, &app, [&]()
        {
            temporaryFirst.start(casLogin, host, temporaryProgram);
            temporarySecond.start(oauthLogin, host, temporaryProgram);
        });
    }
    else
    {
        const QUrl beginA = first.prepare(casLogin, host);
        const QUrl beginB = second.prepare(oauthLogin, host);
        QFile file(urlFile);
        if (!file.open(QIODevice::WriteOnly))
        {
            return 1;
        }
        file.write(gateway + '\n' + beginA.toEncoded() + '\n' + beginB.toEncoded() + '\n');
        QObject::connect(&first, &BrowserSsoSession::callbackReceived, &app, [&](const QByteArray &url) { received(url, a); });
        QObject::connect(&second, &BrowserSsoSession::callbackReceived, &app, [&](const QByteArray &url) { received(url, b); });
    }
    QTimer::singleShot(24000, &app, [&]() { app.exit(2); });
    return app.exec();
}
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const QStringList arguments = app.arguments();
    if (arguments.size() == 3 && arguments.at(1) == "--serve")
    {
        return runBrowserFixture(app, arguments.at(2));
    }
    if (arguments.size() == 3 && arguments.at(1) == "--temporary")
    {
        return runBrowserFixture(app, {}, arguments.at(2));
    }
    if (!checksGatewayAndTicket() || !checksIndependentSessions())
    {
        qCritical() << "SSO callback validation failed";
        return 1;
    }
    return 0;
}
