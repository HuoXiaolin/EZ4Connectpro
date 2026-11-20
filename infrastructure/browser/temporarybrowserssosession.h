#ifndef TEMPORARYBROWSERSSOSESSION_H
#define TEMPORARYBROWSERSSOSESSION_H

#include <QHash>
#include <QJsonArray>
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
    enum class LaunchMode { ShowWindow, RestoreInBackground };

    explicit TemporaryBrowserSsoSession(QObject *parent = nullptr);
    ~TemporaryBrowserSsoSession() override;

    void start(const QUrl &loginUrl, const QUrl &gateway, const QString &browser = {}, const QString &loginCache = {},
               bool keepForResources = false, LaunchMode mode = LaunchMode::RestoreInBackground);
    void openResources(const QString &clientDataFile);
    void startResources(const QUrl &gateway, const QString &clientDataFile, const QString &browser = {});
    bool isActive() const { return active; }
    bool hasResourceTab() const { return active && callbackDelivered && !resourceTabClosed; }
    void cancel();

signals:
    void callbackReceived(const QByteArray &url);
    void failed(const QString &message);
    void restoringLogin();
    void interactionRequired();
    void resourcesOpened();

private:
    using Reply = std::function<void(const QJsonObject &)>;

    void readBrowserOutput();
    void findChromiumEndpoint();
    void openEndpoint(const QString &host, int port, const QString &path);
    void findLoginPage();
    void attachChromiumPage(const QString &target);
    void enableInterception();
    void navigate();
    void restoreCookiesAndNavigate();
    void launchBrowser();
    void stopBrowser();
    void showInteractiveBrowser();
    void saveCookiesAndComplete();
    void complete();
    void send(const QString &method, const QJsonObject &params = {}, Reply reply = {}, bool required = true);
    void handleMessage(const QString &message);
    void handleRequest(const QJsonObject &event);
    void resourceTabWasClosed();
    void browserClosed(const QString &loginError);
    void fail(const QString &message);

    std::unique_ptr<QTemporaryDir> profile;
    QProcess process;
    QWebSocket socket;
    QTimer poll;
    QTimer pageRetry;
    QTimer timeout;
    QTimer silentTimeout;
    QTimer saveTimeout;
    QUrl loginUrl;
    QUrl gateway;
    QByteArray browserOutput;
    QByteArray callback;
    QHash<int, Reply> replies;
    QString sessionId;
    QString contextId;
    QString interceptId;
    QString browserProgram;
    QString cachePath;
    QString resourceDataFile;
    QJsonArray savedCookies;
    int nextCommand = 0;
    bool gecko = false;
    bool active = false;
    bool silent = false;
    bool keepForResources = false;
    bool callbackDelivered = false;
    bool resourceTabClosed = false;
    bool portalOpened = false;
    bool resourceOnly = false;
};

#endif // TEMPORARYBROWSERSSOSESSION_H
