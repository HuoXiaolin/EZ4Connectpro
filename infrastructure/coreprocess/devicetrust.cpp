#include "devicetrust.h"
#include "infrastructure/coreprocess/coreexecutable.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSet>

namespace DeviceTrust
{
namespace
{
QString field(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString().simplified().left(512);
}
}
bool parseSnapshot(const QByteArray &json, Snapshot &snapshot, QString &error)
{
    QJsonParseError parseError;
    const auto document = QJsonDocument::fromJson(json, &parseError);
    const auto root = document.object();
    if (parseError.error != QJsonParseError::NoError || !document.isObject()
        || !root.value("data").isArray() || !root.value("selfId").isString()
        || root.value("selfId").toString().isEmpty() || !root.value("deviceTrusted").isBool())
    {
        error = "服务器返回的设备列表格式不受支持，未执行设备变更。";
        return false;
    }
    Snapshot next{root.value("selfId").toString(), root.value("deviceTrusted").toBool(), {}};
    const auto config = root.value("trustDeviceConfig").toObject();
    next.trustedCount = root.value("currentTrustDeviceCount").toInt(-1);
    next.limit = config.value("limit").toInt(-1);
    next.selfBindAllowed = config.value("enable").toBool() && config.value("enableSelfBind").toInt() == 1;
    QSet<QString> ids;
    const auto records = root.value("data").toArray();
    if (records.size() > 5000) { error = "设备列表过大，请联系管理员处理。"; return false; }
    for (const auto &value : records)
    {
        const auto row = value.toObject();
        const QString id = row.value("id").toString();
        if (!value.isObject() || id.trimmed().isEmpty() || id.size() > 1024 || ids.contains(id))
        {
            error = "设备列表包含缺失或重复的标识，请刷新后重试。";
            return false;
        }
        ids.insert(id);
        Device device;
        device.id = id;
        device.name = field(row, "deviceName");
        if (device.name.isEmpty()) device.name = "未命名设备";
        device.system = (field(row, "os") + ' ' + field(row, "osVersion")).trimmed();
        device.lastLogin = field(row, "lastLoginTime");
        device.address = field(row, "lastLoginIp");
        device.onlineKnown = row.value("onlineStatus").isBool();
        device.online = row.value("onlineStatus").toBool();
        device.self = id == next.selfId;
        next.devices.append(device);
    }
    snapshot = next;
    error.clear();
    return true;
}

QString errorMessage(const QString &detail)
{
    const auto code = QRegularExpression("(?:code\\s+)([0-9]+)").match(detail).captured(1);
    if (code == "75500311") return "授信设备数量已达上限（75500311）。请解绑不再使用的设备，再添加本机；也可联系管理员调整限额。";
    if (code == "75500312") return "本设备已被其他账号绑定（75500312），请联系管理员核对绑定关系。";
    if (code == "75500501") return "授信申请已提交，等待管理员审批后再刷新（75500501）。";
    if (code == "75500502") return "服务器尚未记录本机终端信息，请成功连接一次后重试（75500502）。";
    if (code == "75510008")
        return "网关拒绝本次设备管理请求（75510008）。请刷新后核对本机授信状态和可用名额。";
    if (detail.contains("current device is not trusted"))
        return "服务器不允许本次单独解绑。本机尚未授信且名额已满时，请选择旧设备，使用「解绑并添加本机」。";
    if (code == "73700001" || detail.contains("additional authentication required"))
        return "网关要求额外身份认证，尚未确认授信完成。当前窗口暂不支持继续该认证，请使用已有授信终端或联系管理员处理。";
    if (detail.contains("replacement conditions changed"))
        return "换绑条件已变化，请刷新设备列表后重试。本次未提交设备变更。";
    if (detail.contains("not logged in") || detail.contains("saved login") || detail.contains("HTTP 401")
        || code == "75500200" || code == "5114")
        return "登录状态已失效或尚未登录。请关闭此窗口，连接该线路并完成认证后重试；不要清理已有登录缓存。";
    if (detail.contains("device record changed")) return "所选设备已变化或已解绑，请刷新列表后重新选择。";
    if (detail.contains("HTTP 403")) return "服务器不允许当前账号自助管理授信设备，请联系管理员。";
    if (detail.contains("unsupported device list") || detail.contains("invalid device") || detail.contains("missing device response"))
        return "服务器返回的设备信息不受支持，未执行设备变更。";
    if (detail.contains("timeout", Qt::CaseInsensitive) || detail.contains("deadline", Qt::CaseInsensitive))
        return "服务器响应超时，请刷新列表确认结果后再操作。";
    if (detail.contains("unknown flag")) return "当前连接核心不支持设备管理，请使用本次更新的完整单文件版。";
    // Gateways can include private login data in their errors. Never show raw output.
    return code.isEmpty() ? "设备管理请求失败，请检查网络和登录状态后刷新；若仍失败，请联系管理员。"
        : "服务器拒绝设备管理请求（" + code + "）。请联系管理员核对自助绑定、解绑权限及认证策略。";
}

Client::Client(const QString &server, int port, const QString &cacheFile, QObject *parent,
               const QString &executable, int timeoutMs)
    : QObject(parent), server(server), port(port), cacheFile(cacheFile),
      executable(executable.isEmpty() ? CoreExecutable::path() : executable)
{
    timer.setSingleShot(true);
    timer.setInterval(timeoutMs);
    connect(&timer, &QTimer::timeout, this, [this]() { fail("请求超时，结果尚未确认。请刷新列表后再操作。"); });
    connect(&process, &QProcess::readyReadStandardOutput, this, &Client::readOutput);
    connect(&process, &QProcess::readyReadStandardError, this, &Client::readOutput);
    connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error)
    {
        if (pending && error == QProcess::FailedToStart) fail("无法启动内置连接核心，请重新打开完整单文件版。");
    });
    connect(&process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
        [this](int exitCode, QProcess::ExitStatus status)
    {
        if (!pending) return;
        readOutput();
        if (!pending) return;
        pending = false;
        timer.stop();
        // Keep the completed buffers local: a slot may start the next request.
        const auto completedOutput = std::move(output);
        const auto completedErrors = std::move(errors);
        const auto completedAction = currentAction;
        emit busyChanged(false);
        if (exitCode != 0 || status != QProcess::NormalExit)
        {
            emit failed(errorMessage(QString::fromUtf8(completedErrors)));
            return;
        }
        if (completedAction == Action::List)
        {
            Snapshot snapshot;
            QString error;
            if (parseSnapshot(completedOutput, snapshot, error)) emit loaded(snapshot);
            else emit failed(error);
        }
        else
        {
            const auto result = QJsonDocument::fromJson(completedOutput).object();
            if (!result.value("changed").isBool()) emit failed("未能确认服务器的操作结果，请刷新列表后核对。");
            else emit changed();
        }
    });
}

Client::~Client() { cancel(); }
bool Client::busy() const { return pending || process.state() != QProcess::NotRunning; }
Client::Action Client::action() const { return currentAction; }
void Client::refresh() { start(Action::List); }
void Client::trustSelf() { start(Action::TrustSelf); }
void Client::untrust(const QString &id) { if (!id.trimmed().isEmpty()) start(Action::Untrust, id); }
void Client::replaceSelf(const QString &id) { if (!id.trimmed().isEmpty()) start(Action::ReplaceSelf, id); }

void Client::start(Action action, const QString &id)
{
    if (busy()) return;
    currentAction = action;
    if (server.isEmpty() || port <= 0 || port > 65535 || !QFileInfo::exists(cacheFile))
    {
        emit failed("请先配置网关，并成功连接一次该线路，再管理授信设备。");
        return;
    }
    output.clear(); errors.clear();
    QStringList args{"-protocol", "atrust", "-server", server, "-port", QString::number(port),
                     "-client-data-file", cacheFile, "-device-action",
                     action == Action::List ? "list" : action == Action::TrustSelf ? "trust-self"
                         : action == Action::ReplaceSelf ? "replace-self" : "untrust"};
    if (action == Action::Untrust || action == Action::ReplaceSelf) args << "-device-record-id" << id;
    auto environment = QProcessEnvironment::systemEnvironment();
    for (const auto &key : environment.keys()) if (key.startsWith("ZJU_CONNECT_", Qt::CaseInsensitive)) environment.remove(key);
    process.setProcessEnvironment(environment);
    pending = true;
    emit busyChanged(true);
    timer.start();
    process.start(executable, args);
}

void Client::readOutput()
{
    output += process.readAllStandardOutput();
    errors += process.readAllStandardError();
    if (pending && (output.size() > 2*1024*1024 || errors.size() > 64*1024))
        fail("服务器返回的信息过大，已停止读取。请刷新列表确认结果。");
}

void Client::cancel()
{
    pending = false;
    timer.stop();
    if (process.state() != QProcess::NotRunning) { process.kill(); process.waitForFinished(1000); }
    output.clear(); errors.clear();
}

void Client::fail(const QString &message)
{
    cancel();
    emit busyChanged(false);
    emit failed(message);
}
}
