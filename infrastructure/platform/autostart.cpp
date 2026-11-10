#include "autostart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

#include "application/applicationconstants.h"
#include "application/commandlineoptions.h"
#include "infrastructure/storage/applicationpaths.h"

namespace
{
#if defined(Q_OS_WINDOWS) || defined(Q_OS_LINUX)
QString startupCommand()
{
    const QString program = QDir::toNativeSeparators(ApplicationPaths::launcherExecutable());
    QString command = '"' + program + "\" " + ApplicationConstants::AutoStartArgument;
    const QString profilesDir = CommandLineOptions::value(QCoreApplication::arguments(), "--profiles-dir");
    if (!profilesDir.isEmpty())
    {
        command += " --profiles-dir \"" + QFileInfo(profilesDir).absoluteFilePath() + '"';
    }
    return command;
}
#endif

#if defined(Q_OS_WINDOWS)
const QString runKey = QStringLiteral(R"(HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run)");
#elif defined(Q_OS_LINUX)
QString entryPath()
{
    return QDir(QStandardPaths::writableLocation(QStandardPaths::ConfigLocation))
        .filePath("autostart/" + QCoreApplication::applicationName() + ".desktop");
}

QByteArray entryContents()
{
    const QString name = QCoreApplication::applicationName();
    const QString command = startupCommand().replace('%', "%%");
    return ("[Desktop Entry]\nType=Application\nName=" + name
        + "\nExec=" + command + "\nX-GNOME-Autostart-enabled=true\n").toUtf8();
}
#endif
}

AutoStart::RegistrationStatus AutoStart::registrationStatus()
{
#if defined(Q_OS_WINDOWS)
    QSettings settings(runKey, QSettings::NativeFormat);
    const QString key = QCoreApplication::applicationName();
    if (!settings.contains(key))
    {
        return RegistrationStatus::Missing;
    }
    return settings.value(key).toString().compare(startupCommand(), Qt::CaseInsensitive) == 0
        ? RegistrationStatus::Registered : RegistrationStatus::NeedsRepair;
#elif defined(Q_OS_LINUX)
    QFile entry(entryPath());
    if (!entry.exists())
    {
        return RegistrationStatus::Missing;
    }
    return entry.open(QIODevice::ReadOnly) && entry.readAll() == entryContents()
        ? RegistrationStatus::Registered : RegistrationStatus::NeedsRepair;
#else
    return RegistrationStatus::Unknown;
#endif
}

bool AutoStart::setEnabled(bool enabled)
{
#if defined(Q_OS_WINDOWS)
    QSettings settings(runKey, QSettings::NativeFormat);
    const QString key = QCoreApplication::applicationName();
    if (enabled)
    {
        const QString command = startupCommand();
        if (command.size() > 260)
        {
            return false;
        }
        settings.setValue(key, command);
    }
    else
    {
        settings.remove(key);
    }
    settings.sync();
    return settings.status() == QSettings::NoError
        && registrationStatus() == (enabled ? RegistrationStatus::Registered : RegistrationStatus::Missing);
#elif defined(Q_OS_MACOS)
    const QString bundle = QDir::cleanPath(QCoreApplication::applicationDirPath() + "/../..");
    const QString name = QFileInfo(bundle).baseName();
    QString script = "tell application \"System Events\"\n"
        "delete (every login item whose name is \"" + name + "\")\n";
    if (enabled)
    {
        script += "make login item at end with properties {path:\"" + bundle + "\", hidden:false}\n";
    }
    script += "end tell";
    QProcess process;
    process.start("osascript", {"-e", script});
    return process.waitForFinished(5000) && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
#elif defined(Q_OS_LINUX)
    const QString path = entryPath();
    if (!enabled)
    {
        return !QFile::exists(path) || QFile::remove(path);
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
    {
        return false;
    }
    const QByteArray data = entryContents();
    QSaveFile entry(path);
    return entry.open(QIODevice::WriteOnly) && entry.write(data) == data.size() && entry.commit();
#else
    return !enabled;
#endif
}
