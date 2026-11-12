#include "connectionsession.h"

ConnectionSession::ConnectionSession(CoreProcess *coreProcess, QObject *parent)
    : QObject(parent),
      coreProcess(coreProcess)
{
    coreProcess->setParent(this);
    reconnectTimer.setSingleShot(true);

    connect(coreProcess, &CoreProcess::outputRead, this, &ConnectionSession::outputRead);
    connect(coreProcess, &CoreProcess::graphCaptcha, this, [this](const QString &file)
    {
        if (acceptsCoreEvents()) emit graphCaptcha(file);
    });
    connect(coreProcess, &CoreProcess::smsCode, this, [this](bool skip)
    {
        if (acceptsCoreEvents()) emit smsCode(skip);
    });
    connect(coreProcess, &CoreProcess::totpCode, this, [this]()
    {
        if (acceptsCoreEvents()) emit totpCode();
    });
    connect(coreProcess, &CoreProcess::randCode, this, [this]()
    {
        if (acceptsCoreEvents()) emit randCode();
    });
    connect(coreProcess, &CoreProcess::radiusCode, this, [this](bool skip)
    {
        if (acceptsCoreEvents()) emit radiusCode(skip);
    });
    connect(coreProcess, &CoreProcess::ssoAuth, this, [this](const QString &url)
    {
        if (acceptsCoreEvents()) emit ssoAuth(url);
    });
    connect(coreProcess, &CoreProcess::askSudoPass,
            this, &ConnectionSession::handleSudoPasswordRequest);

    connect(coreProcess, &CoreProcess::error, this, [this](ZJU_ERROR error, const QString &detail)
    {
        if (!acceptsCoreEvents()) return;
        if (sessionState.error() == ZJU_ERROR::NONE)
        {
            coreErrorDetail = detail;
        }
        sessionState.recordError(error);
    });
    connect(coreProcess, &CoreProcess::started, this, [this]()
    {
        emit stateChanged(sessionState.state());
    });
    connect(coreProcess, &CoreProcess::connectionEstablished, this, [this]()
    {
        if (!acceptsCoreEvents()) return;
        sessionState.connectionEstablished();
        emit stateChanged(sessionState.state());
    });
    connect(coreProcess, &CoreProcess::httpProxyReadyChanged, this, [this](bool ready)
    {
        if (state() == ConnectionState::Running && httpProxyReady != ready)
        {
            httpProxyReady = ready;
            emit httpProxyReadyChanged(ready);
        }
    });
    connect(coreProcess, &CoreProcess::finished,
            this, &ConnectionSession::handleCoreFinished);
    connect(&reconnectTimer, &QTimer::timeout, this, [this]()
    {
        if (sessionState.state() != ConnectionState::Reconnecting)
        {
            return;
        }
        sessionState.beginReconnect();
        emit stateChanged(sessionState.state());
        startCore();
    });
}

bool ConnectionSession::start(const ConnectionProfile &profile, const ReconnectPolicy &policy)
{
    if (!sessionState.requestStart(policy))
    {
        return false;
    }

    currentProfile = profile;
    emit stateChanged(sessionState.state());
    startCore();
    return true;
}

void ConnectionSession::stop()
{
    const ConnectionState previousState = sessionState.state();
    if (sessionState.isActive()) coreErrorDetail.clear();
    const bool processNeedsStop = sessionState.requestStop();
    if (previousState == ConnectionState::Reconnecting)
    {
        reconnectTimer.stop();
        emit stateChanged(sessionState.state());
        emit finished(sessionState.error());
        return;
    }

    if (processNeedsStop)
    {
        emit stateChanged(sessionState.state());
        coreProcess->stop();
    }
}

void ConnectionSession::submitInput(const QByteArray &data)
{
    if (acceptsCoreEvents()) coreProcess->writeInput(data);
}

void ConnectionSession::cancelInteractiveInput()
{
    submitInput("\r\n");
    stop();
}

void ConnectionSession::submitSudoPassword(const QString &password, bool remember)
{
    if (password.isEmpty())
    {
        stop();
        return;
    }

    if (remember)
    {
        savedSudoPassword = password;
    }
    sudoPasswordSubmitted = true;
    submitInput(password.toUtf8() + "\n");
}

ConnectionState ConnectionSession::state() const
{
    return sessionState.state();
}

bool ConnectionSession::isActive() const
{
    return sessionState.isActive();
}

bool ConnectionSession::isHttpProxyReady() const
{
    return httpProxyReady && state() == ConnectionState::Running;
}

QString ConnectionSession::errorDetail() const
{
    return coreErrorDetail;
}

bool ConnectionSession::acceptsCoreEvents() const
{
    return sessionState.wantsConnection()
        && (state() == ConnectionState::Starting || state() == ConnectionState::Running);
}

void ConnectionSession::startCore()
{
    sudoPasswordSubmitted = false;
    httpProxyReady = false;
    coreErrorDetail.clear();
    coreProcess->start(currentProfile);
}

void ConnectionSession::handleCoreFinished()
{
    if (sessionState.processFinished() == ProcessFinishAction::Reconnect)
    {
        emit stateChanged(sessionState.state());
        emit reconnectScheduled(sessionState.reconnectDelayMs());
        reconnectTimer.start(sessionState.reconnectDelayMs());
        return;
    }

    currentProfile = {};
    emit stateChanged(sessionState.state());
    emit finished(sessionState.error());
}

void ConnectionSession::handleSudoPasswordRequest()
{
    if (!acceptsCoreEvents()) return;
    if (savedSudoPassword.isEmpty())
    {
        emit askSudoPass();
        return;
    }

    if (sudoPasswordSubmitted)
    {
        savedSudoPassword.clear();
        emit savedSudoPasswordRejected();
        emit askSudoPass();
        return;
    }

    sudoPasswordSubmitted = true;
    submitInput(savedSudoPassword.toUtf8() + "\n");
}
