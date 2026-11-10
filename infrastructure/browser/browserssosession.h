#ifndef BROWSERSSOSESSION_H
#define BROWSERSSOSESSION_H

#include <QTcpServer>
#include <QTimer>
#include <QUrl>

class BrowserSsoSession : public QObject
{
    Q_OBJECT

public:
    explicit BrowserSsoSession(QObject *parent = nullptr);

    static bool isCallback(const QByteArray &url, const QUrl &gateway);
    static QString exportExtension(const QString &directory);
    QUrl prepare(const QUrl &loginUrl, const QUrl &gateway);
    QUrl entryUrl() const;
    void cancel();

signals:
    void callbackReceived(const QByteArray &url);
    void failed(const QString &message);

private:
    void acceptClient();
    void handleRequest(QTcpSocket *client, const QByteArray &method,
                       const QByteArray &target, const QByteArray &body, const QByteArray &origin);
    void fail(const QString &message);

    QTcpServer server;
    QTimer timeout;
    QUrl login;
    QUrl gateway;
    QByteArray sessionPath;
    QByteArray authority;
};

#endif // BROWSERSSOSESSION_H
