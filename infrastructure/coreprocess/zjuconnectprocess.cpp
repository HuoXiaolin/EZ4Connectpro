#include "zjuconnectprocess.h"
#include "infrastructure/coreprocess/corecommandbuilder.h"
#include "infrastructure/coreprocess/consoleoutputdecoder.h"
#include "infrastructure/coreprocess/coreoutputparser.h"
#include "infrastructure/platform/privileges.h"
#include "infrastructure/storage/applicationpaths.h"
#include <qcontainerfwd.h>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QStandardPaths>
#include <utility>

ZjuConnectProcess::ZjuConnectProcess(QObject *parent) : CoreProcess(parent)
{
    zjuConnectProcess = new QProcess(this);
    stopTimer.setSingleShot(true);
    connect(&stopTimer, &QTimer::timeout, zjuConnectProcess, &QProcess::kill);

    httpProbe.setProxy(QNetworkProxy::NoProxy);
    httpProbeTimer.setInterval(150);
    connect(&httpProbeTimer, &QTimer::timeout, this, [this]()
    {
        if (++httpProbeAttempts > 30)
        {
            httpProbeTimer.stop();
            httpProbe.abort();
            qWarning().noquote() << "本机 HTTP 代理端口未就绪";
            if (waitForTunnel)
            {
                emit error(ZJU_ERROR::LISTEN_FAILED);
                stop();
            }
            return;
        }
        httpProbe.abort();
        httpProbe.connectToHost(QHostAddress::LocalHost, httpBind.section(':', -1).toUShort());
    });
    connect(&httpProbe, &QTcpSocket::connected, this, [this]()
    {
        httpProbeTimer.stop();
        httpProbe.abort();
        if (waitForTunnel)
        {
            emit connectionEstablished();
        }
        emit httpProxyReadyChanged(true);
    });

    connect(zjuConnectProcess, &QProcess::readyReadStandardOutput, this, [this]()
    {
        processOutput(standardOutputBuffer, zjuConnectProcess->readAllStandardOutput());
    });

    connect(zjuConnectProcess, &QProcess::readyReadStandardError, this, [this]()
    {
        processOutput(standardErrorBuffer, zjuConnectProcess->readAllStandardError());
    });

    connect(zjuConnectProcess, &QProcess::errorOccurred, this, [&](QProcess::ProcessError err)
    {
        if (stopRequested)
        {
            return;
        }
        QString errorString = zjuConnectProcess->errorString();
        qWarning().noquote() << "退出原因：" + errorString;

        if (errorString.contains("No such file or directory") || errorString.contains("not found") || errorString.contains("找不到"))
        {
            qWarning().noquote() << "核心路径：" + zjuConnectProcess->program();
            emit error(ZJU_ERROR::PROGRAM_NOT_FOUND);
        }
    });

    connect(zjuConnectProcess, &QProcess::finished, this, [&]()
    {
        stopTimer.stop();
        httpProbeTimer.stop();
        httpProbe.abort();
        processOutput(standardOutputBuffer, zjuConnectProcess->readAllStandardOutput(), true);
        processOutput(standardErrorBuffer, zjuConnectProcess->readAllStandardError(), true);
        stopRequested = false;
        tunReservation.release();
        qInfo().noquote() << "退出原因：进程已结束";
        emit finished();
    });
}

void ZjuConnectProcess::processOutput(CoreOutputBuffer &buffer, const QByteArray &data, bool flushPending)
{
    QList<QByteArray> lines = buffer.append(data);

    if (buffer.hasPendingData())
    {
        if (flushPending || CoreOutputParser::hasInteractivePrompt(buffer.pendingData()))
        {
            lines.append(buffer.takePendingData());
        }
    }

    processOutputLines(lines);
}

void ZjuConnectProcess::processOutputLines(const QList<QByteArray> &lines)
{
    if (lines.isEmpty())
    {
        return;
    }

    QStringList outputLines;
    outputLines.reserve(lines.size());
    for (const QByteArray &line : lines)
    {
        const QString decoded = ConsoleOutputDecoder::decode(line);
        const QString loginUrl = CoreOutputParser::ssoLoginUrl(decoded);
        if (!loginUrl.isEmpty())
        {
            ssoLoginUrl = loginUrl;
            outputLines.append(QStringLiteral("已获取浏览器登录地址"));
        }
        else
        {
            outputLines.append(decoded);
        }
    }

    const QString output = outputLines.join('\n');
    emit outputRead(output);

    for (const QString &line : outputLines)
    {
        switch (CoreOutputParser::parse(line))
        {
        case CoreOutputEvent::AskSudoPassword:
            emit askSudoPass();
            break;
        case CoreOutputEvent::GraphCaptcha:
            emit graphCaptcha(CoreOutputParser::graphCaptchaFile(line));
            break;
        case CoreOutputEvent::SmsCodeWithSkipOption:
            emit smsCode(true);
            break;
        case CoreOutputEvent::SmsCode:
            emit smsCode(false);
            break;
        case CoreOutputEvent::TotpCode:
            emit totpCode();
            break;
        case CoreOutputEvent::RandCode:
            emit randCode();
            break;
        case CoreOutputEvent::RadiusCodeWithSkipOption:
            emit radiusCode(true);
            break;
        case CoreOutputEvent::SsoCallback:
            if (!stopRequested && zjuConnectProcess->state() == QProcess::Running)
            {
                emit ssoAuth(std::exchange(ssoLoginUrl, QString{}));
            }
            break;
        case CoreOutputEvent::ClientStarted:
            if (!waitForTunnel)
            {
                emit connectionEstablished();
            }
            break;
        case CoreOutputEvent::HttpProxyStarting:
            if (!stopRequested && zjuConnectProcess->state() == QProcess::Running
                && !httpBind.isEmpty() && line.trimmed().endsWith("HTTP server listening on " + httpBind))
            {
                // The core logs the address before it binds the listener.
                httpProbeTimer.start();
            }
            break;
        case CoreOutputEvent::HttpProxyFailed:
            httpProbeTimer.stop();
            httpProbe.abort();
            emit httpProxyReadyChanged(false);
            emit error(ZJU_ERROR::LISTEN_FAILED);
            if (waitForTunnel)
            {
                stop();
            }
            break;
        case CoreOutputEvent::CaptchaFailed:
            emit error(ZJU_ERROR::CAPTCHA_FAILED);
            break;
        case CoreOutputEvent::AccessDenied:
            emit error(ZJU_ERROR::ACCESS_DENIED);
            break;
        case CoreOutputEvent::ListenFailed:
            emit error(ZJU_ERROR::LISTEN_FAILED);
            break;
        case CoreOutputEvent::TunSetupFailed:
            emit error(ZJU_ERROR::TUN_SETUP_FAILED, CoreOutputParser::tunSetupError(line));
            break;
        case CoreOutputEvent::TunDeviceConflict:
        {
            constexpr auto prefix = "TUN conflict: ";
            const QString detail = CoreOutputParser::tunSetupError(line);
            emit error(ZJU_ERROR::TUN_CONFLICT, detail.startsWith(prefix)
                ? detail.mid(qstrlen(prefix))
                : QStringLiteral("Windows 返回网卡实例已存在（0x800700B7）。请先断开占用该网卡的连接。"));
            break;
        }
        case CoreOutputEvent::InvalidCredentials:
            emit error(ZJU_ERROR::INVALID_DETAIL);
            break;
        case CoreOutputEvent::BruteForceBlocked:
            emit error(ZJU_ERROR::BRUTE_FORCE);
            break;
        case CoreOutputEvent::LoginFailed:
            emit error(ZJU_ERROR::OTHER_LOGIN_FAILED);
            break;
        case CoreOutputEvent::InteractiveError:
            emit error(ZJU_ERROR::INTERACTIVE_ERROR);
            break;
        case CoreOutputEvent::AuthNotAvailable:
            emit error(ZJU_ERROR::AUTH_NOT_AVAILABLE);
            break;
        case CoreOutputEvent::AuthExpired:
            emit error(ZJU_ERROR::AUTH_EXPIRED);
            break;
        case CoreOutputEvent::ClientFailed:
            emit error(ZJU_ERROR::CLIENT_FAILED);
            break;
        case CoreOutputEvent::CorePanic:
            emit error(ZJU_ERROR::OTHER);
            break;
        case CoreOutputEvent::None:
            break;
        }
    }
}

QString ZjuConnectProcess::copyCoreForAppImage(const QString &programPath)
{
#if defined(Q_OS_UNIX)
    static QString cachedSourcePath;
    static QString cachedTempPath;

    if (!qEnvironmentVariableIsSet("APPIMAGE")) {
        return programPath;
    }

    if (cachedSourcePath == programPath && !cachedTempPath.isEmpty() && QFileInfo::exists(cachedTempPath)) {
        return cachedTempPath;
    }

    const QFileInfo sourceInfo(programPath);
    if (!sourceInfo.exists() || !sourceInfo.isFile()) {
        return programPath;
    }

    const QString tempRoot = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                             + "/EZ4Connect-" + QString::number(QCoreApplication::applicationPid());
    QDir().mkpath(tempRoot);

    const QString tempPath = tempRoot + "/" + sourceInfo.fileName();
    if (QFileInfo::exists(tempPath)) {
        QFile::remove(tempPath);
    }

    if (!QFile::copy(programPath, tempPath)) {
        return programPath;
    }

    QFile::setPermissions(tempPath,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                          QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                          QFileDevice::ReadOther | QFileDevice::ExeOther);
    cachedSourcePath = programPath;
    cachedTempPath = tempPath;
    return tempPath;
#else
    return programPath;
#endif
}

void ZjuConnectProcess::start(const ConnectionProfile &profile)
{
    if (profile.tunnel.tunMode)
    {
        const QString conflict = tunReservation.acquire(profile.tunnel.adapterId);
        if (!conflict.isEmpty())
        {
            emit error(ZJU_ERROR::TUN_CONFLICT, conflict);
            emit finished();
            return;
        }
    }
    stopTimer.stop();
    httpBind = profile.proxy.httpBind;
    httpProbeAttempts = 0;
    waitForTunnel = profile.tunnel.tunMode;
    ssoLoginUrl.clear();
    CoreRuntimePaths runtimePaths;

    // Both protocols can require a graph captcha. The UI chooses the response
    // format according to the active protocol.
    if (tempDir == nullptr)
    {
        tempDir = new QTemporaryDir;
        tempDir->setAutoRemove(true);
    }
    runtimePaths.graphCodeFile = tempDir->filePath("graph.jpg");

    if (profile.endpoint.protocol == "atrust")
    {
        runtimePaths.clientDataFile = ApplicationPaths::clientDataFile(profile.profileId);
    }

    const ApplicationPaths::DebugArtifactPaths debugPaths =
        ApplicationPaths::createDebugArtifactFiles(
            profile.profileId,
            profile.debug.capturePcap,
            profile.debug.exportTlsKeys
        );
    runtimePaths.debugPcapFile = debugPaths.pcapFile;
    runtimePaths.debugTlsLogFile = debugPaths.tlsLogFile;

    const CoreCommand command = CoreCommandBuilder::build(profile, runtimePaths);
    if (profile.debug.detailedOutput)
    {
        qInfo().noquote() << "VPN 核心参数 " + command.loggableCommandLine();
    }

    if (!profile.credentials.totpSecret.isEmpty())
    {
        qInfo().noquote() << "使用了 TOTP";
    }
    if (profile.endpoint.protocol == "easyconnect"
        && !profile.credentials.certFile.isEmpty())
    {
        qInfo().noquote() << "使用了证书文件";
    }

    QString programToStart = profile.program;
    QStringList finalArgs = command.arguments;
    QProcessEnvironment processEnvironment = QProcessEnvironment::systemEnvironment();
    processEnvironment.remove("EZ4CONNECTPRO_TUN_GUID");
    if (profile.tunnel.tunMode && !profile.tunnel.adapterId.isEmpty())
    {
        processEnvironment.insert("EZ4CONNECTPRO_TUN_GUID", profile.tunnel.adapterId);
    }
    for (const QString &name : command.clearedEnvironmentVariables)
    {
        processEnvironment.remove(name);
    }
    for (auto iterator = command.environmentVariables.cbegin();
         iterator != command.environmentVariables.cend(); ++iterator)
    {
        processEnvironment.insert(iterator.key(), iterator.value());
    }

#if defined(Q_OS_UNIX)
    if (profile.tunnel.tunMode && !Privileges::isElevated())
    {
        programToStart = copyCoreForAppImage(programToStart);

        QStringList sudoArgs;
        sudoArgs << "-p" << "SUDO_ASK_PASS";
        sudoArgs << "-S";
        if (!command.environmentVariables.isEmpty())
        {
            sudoArgs << "--preserve-env=" + command.environmentVariables.keys().join(',');
        }
        sudoArgs << programToStart << finalArgs;
        programToStart = "sudo";
        finalArgs = sudoArgs;
    }
#endif

    zjuConnectProcess->setProcessEnvironment(processEnvironment);
    zjuConnectProcess->start(programToStart, finalArgs);
    if (!zjuConnectProcess->waitForStarted())
    {
        tunReservation.release();
        emit finished();
    }
    else if (zjuConnectProcess->state() != QProcess::NotRunning)
    {
        emit started();
    }
}

void ZjuConnectProcess::stop()
{
    httpProbeTimer.stop();
    httpProbe.abort();
    if (zjuConnectProcess->state() == QProcess::NotRunning)
    {
        return;
    }

    if (!stopRequested)
    {
        stopRequested = true;
        zjuConnectProcess->terminate();
        if (zjuConnectProcess->state() != QProcess::NotRunning)
        {
            stopTimer.start(1500);
        }
    }
    else
    {
        zjuConnectProcess->kill();
    }
}

void ZjuConnectProcess::writeInput(const QByteArray &data)
{
    zjuConnectProcess->write(data);
}

ZjuConnectProcess::~ZjuConnectProcess()
{
    disconnect(zjuConnectProcess, nullptr, this, nullptr);

    if (zjuConnectProcess->state() == QProcess::NotRunning)
    {
        return;
    }

    zjuConnectProcess->terminate();
    if (!zjuConnectProcess->waitForFinished(3000))
    {
        zjuConnectProcess->kill();
        zjuConnectProcess->waitForFinished();
    }
}
