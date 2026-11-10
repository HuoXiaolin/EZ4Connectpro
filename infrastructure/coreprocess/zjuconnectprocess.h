#ifndef ZJUCONNECTPROCESS_H
#define ZJUCONNECTPROCESS_H

#include <QtCore>
#include <QTcpSocket>

#include "application/coreprocess.h"
#include "infrastructure/coreprocess/coreoutputbuffer.h"
#include "infrastructure/platform/platformtunreservation.h"

class ZjuConnectProcess : public CoreProcess
{
Q_OBJECT

public:
    explicit ZjuConnectProcess(QObject *parent = nullptr);

    ~ZjuConnectProcess() override;

    void start(const ConnectionProfile &profile) override;

    void stop() override;

    void writeInput(const QByteArray &data) override;

private:
    QString copyCoreForAppImage(const QString &programPath);

    void processOutput(CoreOutputBuffer &buffer, const QByteArray &data, bool flushPending = false);

    void processOutputLines(const QList<QByteArray> &lines);

    QProcess *zjuConnectProcess;

    QTemporaryDir *tempDir = nullptr;

    CoreOutputBuffer standardOutputBuffer;
    CoreOutputBuffer standardErrorBuffer;
    QString ssoLoginUrl;
    QTimer stopTimer;
    bool stopRequested = false;
    QString httpBind;
    QTcpSocket httpProbe;
    QTimer httpProbeTimer;
    int httpProbeAttempts = 0;
    bool waitForTunnel = false;
    PlatformTunReservation tunReservation;

};

#endif // ZJUCONNECTPROCESS_H
