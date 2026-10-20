#include "tiktok/auth-profile.h"

#include "tiktok/constants.h"

#include <QDir>
#include <QStandardPaths>

namespace TikTokAuthProfile {

QString storagePath()
{
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    if (base.isEmpty())
        base = QDir::tempPath();
    return base + QStringLiteral("/obs-studio/plugin_config/obs-multi-rtmp/") +
           QString::fromLatin1(TikTokConstants::kWebViewDirName);
}

bool clearStorage()
{
    const QString path = storagePath();
    QDir dir(path);
    if (!dir.exists())
        return true;
    return dir.removeRecursively();
}

} // namespace TikTokAuthProfile
