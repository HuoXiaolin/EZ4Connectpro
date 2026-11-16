#ifndef DEVICETRUST_H
#define DEVICETRUST_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QList>

namespace DeviceTrust
{
struct Device
{
    QString id, name, system, lastLogin, address;
    bool online = false;
    bool onlineKnown = false;
    bool self = false;
};
struct Snapshot
{
    QString selfId;
    bool selfTrusted = false;
    QList<Device> devices;
    int trustedCount = -1;
    int limit = -1;
    bool selfBindAllowed = false;
    bool quotaFull() const { return limit > 0 && trustedCount >= limit; }
    bool canReplace() const { return !selfTrusted && selfBindAllowed && limit > 0 && trustedCount == limit; }
};
bool parseSnapshot(const QByteArray &json, Snapshot &snapshot, QString &error);
QString errorMessage(const QString &detail);

// One instance is bound to one immutable profile for the lifetime of its dialog.
class Client : public QObject
{
    Q_OBJECT
public:
    enum class Action { List, TrustSelf, Untrust, ReplaceSelf };
    Client(const QString &server, int port, const QString &cacheFile, QObject *parent = nullptr,
           const QString &executable = {}, int timeoutMs = 60000);
    ~Client() override;
    bool busy() const;
    Action action() const;
    void refresh();
    void trustSelf();
    void untrust(const QString &recordId);
    void replaceSelf(const QString &recordId);
    void cancel();
signals:
    void busyChanged(bool busy);
    void loaded(const DeviceTrust::Snapshot &snapshot);
    void changed();
    void failed(const QString &message);
private:
    void start(Action action, const QString &recordId = {});
    void fail(const QString &message);
    void readOutput();
    QString server;
    int port;
    QString cacheFile, executable;
    QProcess process;
    QTimer timer;
    QByteArray output, errors;
    bool pending = false;
    Action currentAction = Action::List;
};
}
#endif
