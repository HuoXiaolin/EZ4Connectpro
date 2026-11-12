#include <QCoreApplication>
#include <QDebug>
#include <QEventLoop>

#include "application/connectionsession.h"

namespace
{
class FakeCoreProcess : public CoreProcess
{
public:
    int startCalls = 0;
    int stopCalls = 0;
    QByteArray lastInput;

    void start(const ConnectionProfile &) override
    {
        ++startCalls;
        emit started();
    }

    void stop() override
    {
        ++stopCalls;
    }

    void writeInput(const QByteArray &data) override
    {
        lastInput = data;
    }

    void complete()
    {
        emit finished();
    }

    void establishConnection()
    {
        emit connectionEstablished();
    }

    void requestSudoPassword()
    {
        emit askSudoPass();
    }
};

bool delegatesProcessLifecycleThroughPort()
{
    auto *coreProcess = new FakeCoreProcess();
    ConnectionSession session(coreProcess);
    ConnectionProfile profile;
    int finishedCalls = 0;
    QObject::connect(
        &session,
        &ConnectionSession::finished,
        [&](ZJU_ERROR) { ++finishedCalls; }
    );

    if (!session.start(profile, {})
        || coreProcess->startCalls != 1
        || session.state() != ConnectionState::Starting
        || !session.isActive()
        || session.start(profile, {}))
    {
        qCritical() << "start delegation failed";
        return false;
    }

    coreProcess->establishConnection();
    if (session.state() != ConnectionState::Running)
    {
        qCritical() << "connection establishment was not reflected";
        return false;
    }

    session.submitInput("input");
    session.stop();
    if (coreProcess->lastInput != "input"
        || coreProcess->stopCalls != 1
        || session.state() != ConnectionState::Stopping)
    {
        qCritical() << "input or stop delegation failed";
        return false;
    }

    coreProcess->complete();
    if (finishedCalls != 1
        || session.state() != ConnectionState::Disconnected
        || session.isActive())
    {
        qCritical() << "completion delegation failed";
        return false;
    }
    return true;
}

bool distinguishesInterruptedConnectionFromStartupFailure()
{
    auto *coreProcess = new FakeCoreProcess();
    ConnectionSession session(coreProcess);
    ConnectionProfile profile;

    session.start(profile, {});
    coreProcess->establishConnection();
    emit coreProcess->error(ZJU_ERROR::OTHER);
    coreProcess->complete();

    if (session.state() != ConnectionState::Interrupted)
    {
        qCritical() << "established connection was reported as startup failure";
        return false;
    }
    return true;
}

bool emptySudoPasswordStopsTheSession()
{
    auto *coreProcess = new FakeCoreProcess();
    ConnectionSession session(coreProcess);
    ConnectionProfile profile;

    if (!session.start(profile, {}))
    {
        qCritical() << "failed to start session for sudo cancellation";
        return false;
    }

    coreProcess->requestSudoPassword();
    session.submitSudoPassword({}, false);
    if (coreProcess->stopCalls != 1
        || session.state() != ConnectionState::Stopping)
    {
        qCritical() << "empty sudo password did not stop the session";
        return false;
    }
    return true;
}

bool cancelledInteractiveInputSubmitsNewlineBeforeStopping()
{
    auto *coreProcess = new FakeCoreProcess();
    ConnectionSession session(coreProcess);
    ConnectionProfile profile;

    if (!session.start(profile, {}))
    {
        qCritical() << "failed to start session for interactive input cancellation";
        return false;
    }

    session.cancelInteractiveInput();
    if (coreProcess->lastInput != "\r\n"
        || coreProcess->stopCalls != 1
        || session.state() != ConnectionState::Stopping)
    {
        qCritical() << "interactive input cancellation did not submit a newline and stop";
        return false;
    }
    return true;
}

bool isolatesParallelSessions()
{
    auto *firstCore = new FakeCoreProcess;
    auto *secondCore = new FakeCoreProcess;
    ConnectionSession first(firstCore);
    ConnectionSession second(secondCore);
    ConnectionProfile office;
    office.profileId = "office";
    ConnectionProfile lab;
    lab.profileId = "lab";
    if (!first.start(office, {}) || !second.start(lab, {}))
    {
        return false;
    }
    first.submitInput("office-ticket\n");
    second.submitInput("lab-ticket\n");
    firstCore->establishConnection();
    secondCore->establishConnection();
    if (first.isHttpProxyReady() || second.isHttpProxyReady())
    {
        return false;
    }
    emit firstCore->httpProxyReadyChanged(true);
    if (!first.isHttpProxyReady() || second.isHttpProxyReady())
    {
        return false;
    }
    first.stop();
    if (first.isHttpProxyReady())
    {
        return false;
    }
    firstCore->complete();
    return firstCore->lastInput == "office-ticket\n" && secondCore->lastInput == "lab-ticket\n"
        && first.state() == ConnectionState::Disconnected && second.state() == ConnectionState::Running
        && firstCore->stopCalls == 1 && secondCore->stopCalls == 0;
}

bool intentionalStopIgnoresLateAuthenticationButKeepsLogs()
{
    auto *core = new FakeCoreProcess;
    ConnectionSession session(core);
    int prompts = 0;
    int completions = 0;
    ZJU_ERROR completion = ZJU_ERROR::OTHER;
    QString output;
    QObject::connect(&session, &ConnectionSession::ssoAuth, [&](const QString &) { ++prompts; });
    QObject::connect(&session, &ConnectionSession::graphCaptcha, [&](const QString &) { ++prompts; });
    QObject::connect(&session, &ConnectionSession::smsCode, [&](bool) { ++prompts; });
    QObject::connect(&session, &ConnectionSession::radiusCode, [&](bool) { ++prompts; });
    QObject::connect(&session, &ConnectionSession::totpCode, [&]() { ++prompts; });
    QObject::connect(&session, &ConnectionSession::randCode, [&]() { ++prompts; });
    QObject::connect(&session, &ConnectionSession::askSudoPass, [&]() { ++prompts; });
    QObject::connect(&session, &ConnectionSession::outputRead, [&](const QString &text) { output = text; });
    QObject::connect(&session, &ConnectionSession::finished, [&](ZJU_ERROR error) { ++completions; completion = error; });
    session.start({}, {true, 1});
    emit core->smsCode(false);
    if (prompts != 1) return false;
    core->establishConnection();
    emit core->error(ZJU_ERROR::AUTH_EXPIRED, "in-flight expiry before user stop");
    session.stop();
    emit core->error(ZJU_ERROR::AUTH_EXPIRED, "invalid SID during shutdown");
    emit core->outputRead("invalid SID during shutdown");
    emit core->ssoAuth("https://office.example/login");
    emit core->graphCaptcha("unused.png");
    emit core->smsCode(false);
    emit core->radiusCode(false);
    emit core->totpCode();
    emit core->randCode();
    emit core->askSudoPass();
    core->establishConnection();
    emit core->httpProxyReadyChanged(true);
    if (session.state() != ConnectionState::Stopping || session.isHttpProxyReady()) return false;
    core->complete();
    emit core->smsCode(false);
    if (prompts != 1 || completions != 1 || completion != ZJU_ERROR::NONE
        || !session.errorDetail().isEmpty() || session.state() != ConnectionState::Disconnected
        || output != "invalid SID during shutdown")
    {
        qCritical() << "manual disconnect accepted a late auth prompt/error or dropped its diagnostic log";
        return false;
    }
    session.start({}, {});
    emit core->error(ZJU_ERROR::AUTH_EXPIRED, "real login expiry");
    core->complete();
    return completions == 2 && completion == ZJU_ERROR::AUTH_EXPIRED
        && session.state() == ConnectionState::Failed && session.errorDetail() == "real login expiry";
}

bool cancellingReconnectDoesNotReportOldExpiryOrRestart()
{
    auto *core = new FakeCoreProcess;
    ConnectionSession session(core);
    ZJU_ERROR completion = ZJU_ERROR::OTHER;
    QObject::connect(&session, &ConnectionSession::finished, [&](ZJU_ERROR error) { completion = error; });
    session.start({}, {true, 20});
    core->establishConnection();
    emit core->error(ZJU_ERROR::AUTH_EXPIRED, "expiry before reconnect");
    core->complete();
    if (session.state() != ConnectionState::Reconnecting) return false;
    session.stop();
    QEventLoop loop;
    QTimer::singleShot(60, &loop, &QEventLoop::quit);
    loop.exec();
    return completion == ZJU_ERROR::NONE && session.state() == ConnectionState::Disconnected
        && session.errorDetail().isEmpty() && core->startCalls == 1;
}
}

int main(int argc, char *argv[])
{
    QCoreApplication application(argc, argv);
    return delegatesProcessLifecycleThroughPort()
        && distinguishesInterruptedConnectionFromStartupFailure()
        && emptySudoPasswordStopsTheSession()
        && isolatesParallelSessions()
        && intentionalStopIgnoresLateAuthenticationButKeepsLogs()
        && cancellingReconnectDoesNotReportOldExpiryOrRestart()
        && cancelledInteractiveInputSubmitsNewlineBeforeStopping() ? 0 : 1;
}
