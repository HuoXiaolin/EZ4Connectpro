#ifndef COREEXECUTABLE_H
#define COREEXECUTABLE_H

#include <QString>

class QObject;

namespace CoreExecutable
{
QString path();
QString version(QObject *parent, int timeoutMs = 30000);
bool supportsIndependentTunnels();
}

#endif // COREEXECUTABLE_H
