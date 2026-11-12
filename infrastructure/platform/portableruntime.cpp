#include "portableruntime.h"

#include <QCoreApplication>
#include <QDebug>
#include <QFileInfo>
#include <QProcess>
#include "infrastructure/storage/applicationpaths.h"

void PortableRuntime::startCleanupWorker()
{
#if defined(Q_OS_WIN)
    const QString runtime = qEnvironmentVariable("EZ4CONNECTPRO_RUNTIME");
    const QString launcher = ApplicationPaths::portableLauncher();
    if (runtime.isEmpty() || launcher.isEmpty()) return;
    if (QFileInfo(runtime) != QFileInfo(QCoreApplication::applicationDirPath()))
    {
        qWarning() << "Portable cleanup directory does not match the application directory";
        return;
    }
    // Keep the cleanup worker beneath the window-owning process in the process tree.
    // The bootstrap stays alive as a fallback until this worker acknowledges ownership.
    if (!QProcess::startDetached(launcher, {"--ez4-cleanup-runtime",
            QString::number(QCoreApplication::applicationPid()), runtime}))
    {
        qWarning() << "Portable cleanup worker unavailable; bootstrap will retain cleanup ownership";
    }
#endif
}
