#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTcpSocket>
#include <cstdio>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

#include "infrastructure/browser/browserssosession.h"
#include "infrastructure/browser/temporarybrowserssosession.h"
#include "infrastructure/browser/browserlogincache.h"
#include "infrastructure/storage/applicationpaths.h"

namespace
{
#ifdef Q_OS_WIN
HWND fixtureWindow(const QString &title)
{
    struct Search { QString title; HWND window; } search{title, nullptr};
    EnumDesktopWindows(GetThreadDesktop(GetCurrentThreadId()), [](HWND window, LPARAM value) -> BOOL
    {
        auto *search = reinterpret_cast<Search *>(value);
        wchar_t text[256]{};
        if (IsWindowVisible(window) && GetWindowTextW(window, text, 256)
            && QString::fromWCharArray(text).startsWith(search->title))
        {
            search->window = window;
            return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.window;
}
#endif
bool writeGatewayFixture(const QString &file, const QUrl &gateway)
{
    const auto cookie = [&](const QString &host, const QString &name)
    { return QJsonObject{{"host", host}, {"scheme", gateway.scheme()}, {"name", name}, {"value", "resource-fixture"}}; };
    const QJsonArray cookies{cookie(gateway.authority(), "sid"), cookie(gateway.authority(), "sid.sig"),
        cookie("other.example", "foreign"), cookie(gateway.host() + ":444", "wrong-port"),
        cookie(gateway.authority(), "bad\r\nname")};
    QFile output(file);
    return output.open(QIODevice::WriteOnly)
        && output.write(QJsonDocument(QJsonObject{{"cookies", cookies}}).toJson()) > 0;
}

// Only the Chromium launched by this fixture uses its private temporary root.
bool closeFixturePortalTab(const QTemporaryDir &root, const QUrl &portal)
{
    const auto dirs = QDir(root.path()).entryList({"EZ4Connectpro-login-*"}, QDir::Dirs | QDir::NoDotAndDotDot);
    if (dirs.size() != 1) return false;
    QFile endpoint(root.filePath(dirs.first() + "/DevToolsActivePort"));
    if (!endpoint.open(QIODevice::ReadOnly)) return false;
    bool ok = false;
    const auto port = endpoint.readLine(8).trimmed().toUShort(&ok);
    if (!ok || !port) return false;
    QNetworkAccessManager controller;
    controller.setProxy(QNetworkProxy::NoProxy);
    const auto request = [&](const QString &action, bool create = false)
    {
        QNetworkRequest command(QUrl(QString("http://127.0.0.1:%1/json/").arg(port) + action));
        command.setTransferTimeout(2000);
        auto *reply = create ? controller.put(command, QByteArray{}) : controller.get(command);
        QEventLoop loop;
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        return reply->error() == QNetworkReply::NoError ? reply->readAll() : QByteArray{};
    };
    const auto tabs = QJsonDocument::fromJson(request("list")).array();
    for (const auto &entry : tabs)
    {
        const auto tab = entry.toObject();
        if (tab.value("url").toString() != portal.toString() || tab.value("type") != "page") continue;
        if (QJsonDocument::fromJson(request("new?about:blank", true)).object().value("id").toString().isEmpty()) return false;
        return !request("close/" + tab.value("id").toString()).isEmpty();
    }
    return false;
}

enum class PortalClose { None, Window, Tab };

int runPortalFixture(QCoreApplication &app, const QString &program, bool direct = false, PortalClose close = PortalClose::None)
{
    QTemporaryDir root;
    if (close == PortalClose::Tab)
    {
        const QByteArray temp = QFile::encodeName(QDir::toNativeSeparators(root.path()));
        qputenv("TMP", temp);
        qputenv("TEMP", temp);
    }
    QTcpServer server;
    server.setProxy(QNetworkProxy::NoProxy);
    if (!server.listen(QHostAddress::LocalHost)) return 1;
    const QUrl gateway("http://127.0.0.1:" + QString::number(server.serverPort()));
    const QString cache = root.filePath("client-data.json");
    if (!writeGatewayFixture(cache, gateway)) return 1;
    TemporaryBrowserSsoSession browser;
    int callbacks = 0;
    int portalRequests = 0;
    const QString title = "EZ4 portal fixture " + QString::number(server.serverPort());
    bool portalVerified = false;
    bool loginRequested = false;
    bool callbackConsumed = false;
    QObject::connect(&browser, &TemporaryBrowserSsoSession::failed, &app, [&](const QString &reason)
    {
        qCritical().noquote() << reason;
        app.exit(1);
    });
    QObject::connect(&browser, &TemporaryBrowserSsoSession::callbackReceived, &app, [&](const QByteArray &)
    {
        ++callbacks;
        QTimer::singleShot(150, &browser, [&]() { browser.openResources(cache); });
    });
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]()
    {
        auto *socket = server.nextPendingConnection();
        QObject::connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [&, socket, bytes = QByteArray{}]() mutable
        {
            bytes += socket->readAll();
            if (!bytes.contains("\r\n\r\n")) return;
            const auto path = bytes.split(' ').value(1);
            if (path == "/login")
            {
                loginRequested = true;
                socket->write("HTTP/1.1 302 Found\r\nLocation: " + gateway.toEncoded()
                    + "/passport/v1/auth/cas?ticket=ST-resource\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
            else if (path == "/portal/")
            {
                ++portalRequests;
                const bool ok = callbacks == (direct ? 0 : 1) && loginRequested == !direct && !callbackConsumed && bytes.contains("sid=resource-fixture")
                    && !bytes.contains("foreign=") && !bytes.contains("wrong-port=");
                const QByteArray html = "<!doctype html><title>" + title.toUtf8() + "</title>Resources are ready.";
                socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(html.size())
                              + "\r\nConnection: close\r\n\r\n" + html);
                qInfo() << "Same-browser resources verified:" << ok << "callbacks" << callbacks
                        << "ticketConsumedByBrowser" << callbackConsumed << "sessionCookie" << bytes.contains("sid=resource-fixture")
                        << "foreignCookie" << bytes.contains("foreign=") << "wrongPortCookie" << bytes.contains("wrong-port=");
                if (ok && close == PortalClose::Tab && portalRequests == 1)
                    portalVerified = true;
                else if (ok && direct && portalRequests == 1)
                    QTimer::singleShot(300, &browser, [&]() { browser.openResources(cache); });
                else if (ok && close == PortalClose::Window) portalVerified = true;
                else QTimer::singleShot(300, &app, [&, ok]() { app.exit(ok ? 0 : 1); });
            }
            else
            {
                callbackConsumed |= path.startsWith("/passport/v1/auth/");
                socket->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
            socket->disconnectFromHost();
        });
    });
#ifdef Q_OS_WIN
    QTimer windowPoll;
    bool closeSent = false;
    bool reopening = false;
    QObject::connect(&windowPoll, &QTimer::timeout, &app, [&]()
    {
        if (!portalVerified) return;
        if (close == PortalClose::Tab)
        {
            if (!closeSent && fixtureWindow(title))
            {
                closeSent = true;
                windowPoll.stop();
                if (!closeFixturePortalTab(root, gateway.resolved(QUrl("/portal/")))) { app.exit(1); return; }
                windowPoll.start();
            }
            else if (closeSent && !browser.isActive()) app.exit(1);
            else if (closeSent && !reopening && !browser.hasResourceTab())
            {
                reopening = true;
                browser.openResources(cache);
            }
        }
        else if (!closeSent)
        {
            if (const auto window = fixtureWindow(title)) closeSent = PostMessageW(window, WM_CLOSE, 0, 0);
        }
        else if (!browser.isActive() && !fixtureWindow(title))
        {
            qInfo() << "Resource window closed without a login failure; callbacks" << callbacks;
            app.exit(0);
        }
    });
    if (close != PortalClose::None) windowPoll.start(40);
#endif
    QTimer::singleShot(0, &app, [&]()
    {
        if (direct) browser.startResources(gateway, cache, program);
        else browser.start(gateway.resolved(QUrl("/login")), gateway, program, {}, true);
    });
    QTimer::singleShot(25000, &app, [&]() { app.exit(2); });
    if (close == PortalClose::None) return app.exec();
    static bool closeWarning = false;
    const auto previous = qInstallMessageHandler([](QtMsgType type, const QMessageLogContext &, const QString &message)
    {
        std::fprintf(stderr, "%s\n", message.toUtf8().constData());
        if (type == QtWarningMsg && (message.contains("QSocketNotifier") || message.startsWith("临时浏览器")))
            closeWarning = true;
    });
    const int result = app.exec();
    qInstallMessageHandler(previous);
    return closeWarning ? 1 : result;
}

bool checksProtectedLoginCache()
{
#ifdef Q_OS_WIN
    QTemporaryDir root;
    const QString path = root.filePath("login.dat");
    const QUrl gateway("https://vpn.example:443");
    const auto cookie = [](const QString &domain, double expires)
    {
        return QJsonObject{{"name", "CAS-session"}, {"value", "private-session-fixture"},
            {"domain", domain}, {"path", "/cas"}, {"secure", true}, {"httpOnly", true},
            {"sameSite", "Lax"}, {"expires", expires}};
    };
    QJsonArray data{cookie("login.example", -1), cookie(".example", QDateTime::currentSecsSinceEpoch() + 3600),
                    cookie("expired.example", QDateTime::currentSecsSinceEpoch() - 1)};
    auto partitioned = cookie("isolated.example", -1);
    partitioned.insert("partitionKey", QJsonObject{{"topLevelSite", "https://example"}});
    data.append(partitioned);
    if (!BrowserLoginCache::save(path, gateway, data)) return false;
    const auto restored = BrowserLoginCache::load(path, QUrl("https://vpn.example"));
    if (restored.size() != 2 || restored[0].toObject().contains("domain")
        || restored[0].toObject().value("url") != "https://login.example/cas"
        || restored[0].toObject().contains("expires") || !restored[0].toObject().value("httpOnly").toBool()
        || restored[1].toObject().value("domain") != ".example"
        || !BrowserLoginCache::load(path, QUrl("https://other.example")).isEmpty()) return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return false;
    const QByteArray bytes = file.readAll();
    file.close();
    if (bytes.contains("private-session-fixture") || bytes.contains("CAS-session")) return false;
    const QString other = root.filePath("other-profile.dat");
    if (!file.copy(other) || !BrowserLoginCache::load(other, gateway).isEmpty()) return false;
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    file.write(bytes.left(bytes.size() / 2));
    file.close();
    if (!BrowserLoginCache::load(path, gateway).isEmpty()) return false;
    ApplicationPaths::setProfileRoot(root.path());
    const QString defaultClient = ApplicationPaths::clientDataFile("");
    const QString defaultLogin = ApplicationPaths::browserLoginDataFile("");
    const QString otherLogin = ApplicationPaths::browserLoginDataFile("other");
    const bool cleared = BrowserLoginCache::save(defaultLogin, gateway, data)
        && BrowserLoginCache::save(otherLogin, gateway, data)
        && ApplicationPaths::clearClientData("") && !QFile::exists(defaultClient) && !QFile::exists(defaultLogin)
        && !BrowserLoginCache::load(otherLogin, gateway).isEmpty();
    ApplicationPaths::setProfileRoot({});
    if (!cleared) return false;
#endif
    return true;
}

// Each child is a fresh application process and owns a fresh temporary browser.
// Only the DPAPI-protected cookie file crosses the process boundary.
int runRememberChild(QCoreApplication &app, const QStringList &args)
{
    const QUrl gateway(args[3]);
    const QString phase = args[5];
    const QUrl login = gateway.resolved(QUrl("/" + phase));
    TemporaryBrowserSsoSession browser;
    int silent = 0, interactive = 0;
    QElapsedTimer elapsed;
    qint64 visibleMs = -1;
#ifdef Q_OS_WIN
    QTimer windowPoll;
    QObject::connect(&windowPoll, &QTimer::timeout, &app, [&]()
    {
        if (fixtureWindow("EZ4 manual fixture " + QString::number(gateway.port())))
        {
            visibleMs = elapsed.elapsed();
            windowPoll.stop();
        }
    });
    if (phase == "manual") windowPoll.start(25);
#endif
    QObject::connect(&browser, &TemporaryBrowserSsoSession::restoringLogin, &app, [&]() { ++silent; });
    QObject::connect(&browser, &TemporaryBrowserSsoSession::interactionRequired, &app, [&]() { ++interactive; });
    QObject::connect(&browser, &TemporaryBrowserSsoSession::failed, &app, [&](const QString &message)
    {
        qCritical().noquote() << message;
        app.exit(1);
    });
    QObject::connect(&browser, &TemporaryBrowserSsoSession::callbackReceived, &app, [&](const QByteArray &callback)
    {
        const int expectedSilent = phase == "seed" || phase == "manual" ? 0 : 1;
        const int expectedInteractive = phase == "resume" ? 0 : 1;
        const QByteArray expected = gateway.resolved(QUrl("/passport/v1/auth/cas?ticket=ST-" + phase)).toEncoded();
        const bool ok = callback == expected && silent == expectedSilent && interactive == expectedInteractive
            && (phase != "manual" || (visibleMs >= 0 && visibleMs < 10000));
        qInfo() << "Login phase" << phase << "silent" << silent << "interactive" << interactive
                << "visible ms" << visibleMs << "verified" << ok;
        app.exit(ok ? 0 : 1);
    });
    QTimer::singleShot(0, &app, [&]()
    {
        elapsed.start();
        using LaunchMode = TemporaryBrowserSsoSession::LaunchMode;
        browser.start(login, gateway, args[2], args[4], false,
                      phase == "manual" ? LaunchMode::ShowWindow : LaunchMode::RestoreInBackground);
    });
    QTimer::singleShot(26000, &app, [&]() { app.exit(2); });
    return app.exec();
}

int runRememberFixture(QCoreApplication &app, const QString &program, const QString &phaseName)
{
    QTemporaryDir root;
    QTcpServer server;
    server.setProxy(QNetworkProxy::NoProxy);
    if (!server.listen(QHostAddress::LocalHost)) return 1;
    const QByteArray gateway = "http://127.0.0.1:" + QByteArray::number(server.serverPort());
    bool gatewayHit = false;
    int restores = 0;
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&]()
    {
        auto *client = server.nextPendingConnection();
        QObject::connect(client, &QTcpSocket::disconnected, client, &QObject::deleteLater);
        QObject::connect(client, &QTcpSocket::readyRead, client, [&, client, request = QByteArray{}]() mutable
        {
            request += client->readAll();
            if (!request.contains("\r\n\r\n")) return;
            const auto target = request.split(' ').value(1);
            if (target == "/manual" && request.contains("CAS-fixture=remembered"))
            {
                ++restores;
                const QByteArray body = "<!doctype html><title>EZ4 manual fixture " + QByteArray::number(server.serverPort())
                    + "</title><script>setTimeout(() => location.replace('/passport/v1/auth/cas?ticket=ST-manual'), 1200)</script>";
                client->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(body.size())
                              + "\r\nConnection: close\r\n\r\n" + body);
            }
            else if (target == "/seed" || target == "/resume" || target == "/expired")
            {
                const bool hasCookie = request.contains("CAS-fixture=remembered");
                const bool restored = target != "/seed" && hasCookie;
                if (restored) ++restores;
                if (target == "/seed" || (restored && (target == "/resume" || restores >= 2)))
                {
                    client->write("HTTP/1.1 302 Found\r\nSet-Cookie: CAS-fixture=remembered; Path=/; HttpOnly; SameSite=Lax\r\nLocation: "
                        + gateway + "/passport/v1/auth/cas?ticket=ST-" + target.mid(1)
                        + "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
                }
                else
                {
                    const QByteArray body("<html><body>Authentication required<form><input name='username'></form></body></html>");
                    client->write("HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: " + QByteArray::number(body.size())
                                  + "\r\nConnection: close\r\n\r\n" + body);
                }
            }
            else
            {
                gatewayHit |= target.startsWith("/passport/v1/auth/");
                client->write("HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            }
            client->disconnectFromHost();
        });
    });
    QProcess child;
    child.setProcessChannelMode(QProcess::ForwardedChannels);
    int phase = 0;
    const auto launch = [&](const QString &name)
    {
        child.start(QCoreApplication::applicationFilePath(), {"--remember-child", program, QString::fromLatin1(gateway), root.filePath("login.dat"), name});
    };
    QObject::connect(&child, &QProcess::finished, &app, [&](int code, QProcess::ExitStatus status)
    {
        if (code != 0 || status != QProcess::NormalExit || gatewayHit) { app.exit(1); return; }
        if (phase++ == 0) launch(phaseName);
        else app.exit(restores == (phaseName == "expired" ? 2 : 1) ? 0 : 1);
    });
    QTimer::singleShot(0, &app, [&]() { launch("seed"); });
    QTimer::singleShot(29000, &app, [&]() { app.exit(2); });
    return app.exec();
}

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

bool checksStandaloneResourceBridge()
{
    QTemporaryDir root;
    const QString file = root.filePath("client-data.json");
    const QUrl gateway("https://vpn.example:443");
    if (!writeGatewayFixture(file, gateway)) return false;
    BrowserSsoSession bridge;
    int callbacks = 0;
    QObject::connect(&bridge, &BrowserSsoSession::callbackReceived, [&]() { ++callbacks; });
    if (!bridge.prepareResources(QUrl("https://other.example"), file).isEmpty()) return false;
    const QUrl entry = bridge.prepareResources(gateway, file);
    if (entry.isEmpty()) return false;
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    const auto get = [&](const QString &path)
    {
        QNetworkRequest request(entry.resolved(QUrl(path)));
        request.setTransferTimeout(2000);
        QEventLoop loop;
        auto *reply = network.get(request);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QByteArray bytes = reply->readAll();
        delete reply;
        return bytes;
    };
    const auto metadata = QJsonDocument::fromJson(get("metadata")).object();
    if (!metadata.value("resourceOnly").toBool() || !get("return").contains("dataset.ez4Callback='ready'")) return false;
    if (post(network, entry.resolved(QUrl("return")), "http://" + entry.authority().toLatin1(),
             "https://vpn.example/passport/v1/auth/cas?ticket=must-not-be-consumed") != 400) return false;
    const auto payload = QJsonDocument::fromJson(get("resources")).object();
    return callbacks == 0 && payload.value("cookies").toArray().size() == 2
        && QUrl(payload.value("url").toString()).path() == "/portal/" && bridge.entryUrl().isEmpty();
}

bool checksResourceBridge()
{
    QTemporaryDir root;
    const QString file = root.filePath("client-data.json");
    const QUrl gateway("https://vpn.example");
    if (!writeGatewayFixture(file, gateway)) return false;
    const auto cookies = BrowserLoginCache::gatewaySessionCookies(file, gateway);
    if (cookies.size() != 2 || !BrowserLoginCache::gatewaySessionCookies(file, QUrl("https://other.example")).isEmpty()) return false;
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    BrowserSsoSession bridge;
    const auto entry = bridge.prepare(gateway.resolved(QUrl("/login")), gateway, true);
    const QByteArray origin = "http://" + entry.authority().toLatin1();
    const auto returned = entry.resolved(QUrl("return"));
    const QByteArray callback("https://vpn.example/passport/v1/auth/cas?ticket=fixture");
    if (post(network, returned, origin, callback) != 204 || post(network, returned, origin, callback) != 400) return false;
    const auto get = [&]()
    {
        QNetworkRequest request(entry.resolved(QUrl("resources")));
        request.setTransferTimeout(2000);
        QEventLoop loop;
        auto *reply = network.get(request);
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const auto response = qMakePair(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt(), reply->readAll());
        delete reply;
        return response;
    };
    if (get().first != 202) return false;
    bridge.openResources(file);
    const auto response = get();
    const auto data = QJsonDocument::fromJson(response.second).object();
    return response.first == 200 && data.value("url") == "https://vpn.example/portal/"
        && data.value("cookies").toArray().size() == 2 && bridge.entryUrl().isEmpty();
}

bool checksClosedBridgeTimeouts()
{
    QTemporaryDir root;
    const QUrl gateway("https://vpn.example");
    const QString cache = root.filePath("client-data.json");
    if (!writeGatewayFixture(cache, gateway)) return false;
    BrowserSsoSession bridge;
    int failures = 0;
    QObject::connect(&bridge, &BrowserSsoSession::failed, [&]() { ++failures; });
    const auto expire = [&]() { return QMetaObject::invokeMethod(&bridge, "handleTimeout") && bridge.entryUrl().isEmpty(); };
    if (bridge.prepareResources(gateway, cache).isEmpty() || !expire() || failures) return false;
    const auto begin = bridge.prepare(gateway.resolved(QUrl("/login")), gateway, true);
    QNetworkAccessManager network;
    network.setProxy(QNetworkProxy::NoProxy);
    const int accepted = post(network, begin.resolved(QUrl("return")), ("http://" + begin.authority()).toUtf8(),
                              "https://vpn.example/passport/v1/auth/cas?ticket=fixture");
    if (accepted != 204 || !expire() || failures) return false;
    return !bridge.prepare(gateway.resolved(QUrl("/login")), gateway).isEmpty() && expire() && failures == 1;
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
    if (arguments.size() == 3 && arguments[1] == "--portal") return runPortalFixture(app, arguments[2]);
    if (arguments.size() == 3 && arguments[1] == "--resources") return runPortalFixture(app, arguments[2], true);
#ifdef Q_OS_WIN
    if (arguments.size() == 3 && arguments[1] == "--portal-close") return runPortalFixture(app, arguments[2], false, PortalClose::Window);
    if (arguments.size() == 3 && arguments[1] == "--resources-close") return runPortalFixture(app, arguments[2], true, PortalClose::Window);
    if (arguments.size() == 3 && arguments[1] == "--resource-tab") return runPortalFixture(app, arguments[2], true, PortalClose::Tab);
    if (arguments.size() == 3 && arguments[1] == "--manual-cache") return runRememberFixture(app, arguments[2], "manual");
#endif
    if (arguments.size() == 6 && arguments[1] == "--remember-child") return runRememberChild(app, arguments);
    if (arguments.size() == 3 && (arguments[1] == "--remember" || arguments[1] == "--expired"))
        return runRememberFixture(app, arguments[2], arguments[1] == "--expired" ? "expired" : "resume");
    if (arguments.size() == 3 && arguments.at(1) == "--serve")
    {
        return runBrowserFixture(app, arguments.at(2));
    }
    if (arguments.size() == 3 && arguments.at(1) == "--temporary")
    {
        return runBrowserFixture(app, {}, arguments.at(2));
    }
    if (!checksGatewayAndTicket() || !checksIndependentSessions() || !checksProtectedLoginCache()
        || !checksResourceBridge() || !checksStandaloneResourceBridge() || !checksClosedBridgeTimeouts())
    {
        qCritical() << "SSO callback validation failed";
        return 1;
    }
    return 0;
}
