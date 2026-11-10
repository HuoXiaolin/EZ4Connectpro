#ifndef TEMPORARYBROWSERSSOSESSION_H
#define TEMPORARYBROWSERSSOSESSION_H

#include <QHash>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include <functional>
#include <memory>

class TemporaryBrowserSsoSession : public QObject
{
    Q_OBJECT

public:
    explicit TemporaryBrowserSsoSession(QObject *parent = nullptr);
    ~TemporaryBrowserSsoSession() override;

    void start(const QUrl &loginUrl, const QUrl &gateway, const QString &browser = {});
    void cancel();

signals:
    void callbackReceived(const QByteArray &url);
    void failed(const QString &message);

private:
    using Reply = std::function<void(const QJsonObject &)>;

    void readBrowserOutput();
    void findChromiumEndpoint();
    void openEndpoint(const QString &host, int port, const QString &path);
    void findLoginPage();
    void enableInterception();
    void navigate();
    void send(const QString &method, const QJsonObject &params = {}, Reply reply = {});
    void handleMessage(const QString &message);
    void handleRequest(const QJsonObject &event);
    void fail(const QString &message);

    std::unique_ptr<QTemporaryDir> profile;
    QProcess process;
    QWebSocket socket;
    QTimer poll;
    QTimer pageRetry;
    QTimer timeout;
    QUrl loginUrl;
    QUrl gateway;
    QByteArray browserOutput;
    QByteArray callback;
    QHash<int, Reply> replies;
    QString sessionId;
    QString contextId;
    int nextCommand = 0;
    bool gecko = false;
    bool active = false;
};

#endif // TEMPORARYBROWSERSSOSESSION_H
