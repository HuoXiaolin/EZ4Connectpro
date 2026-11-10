#ifndef APPLICATIONCONSTANTS_H
#define APPLICATIONCONSTANTS_H

#include <QString>

namespace ApplicationConstants
{
const inline QString RepositoryName("HuoXiaolin/EZ4Connectpro");
const inline QString ProjectUrl = "https://github.com/" + RepositoryName;
const inline QString LatestReleaseUrl = ProjectUrl + "/releases/latest";
const inline QString ApplicationName("EZ4Connectpro");
const inline QString AutoStartArgument("--autostart");
const inline QString SsoBrowserModeKey("ZJUConnect/SsoBrowserMode");
constexpr inline int ConfigVersion = 8;
}

#endif // APPLICATIONCONSTANTS_H
