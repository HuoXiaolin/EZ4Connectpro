#ifndef PRIVILEGES_H
#define PRIVILEGES_H

class QString;

namespace Privileges
{
bool isElevated();
bool relaunchElevated(const QString &activeConfigPath, bool silent);
void waitForPreviousInstance(int argc, char *argv[]);
}

#endif // PRIVILEGES_H
