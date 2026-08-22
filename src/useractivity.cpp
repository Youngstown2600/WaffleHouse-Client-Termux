#include "useractivity.h"

#include <QDateTime>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>

namespace UserActivity {

qint64 idleMilliseconds(qint64 lastApplicationActivityMs)
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const qint64 fallback = std::max<qint64>(0, now - lastApplicationActivityMs);

    // Cache the external probe so the GUI/CLI never spawn a helper every tick.
    static qint64 nextProbeMs = 0;
    static qint64 cachedSystemIdleMs = -1;
    if (now < nextProbeMs) return cachedSystemIdleMs >= 0 ? cachedSystemIdleMs : fallback;
    nextProbeMs = now + 5000;

    const QString helper = QStandardPaths::findExecutable(QStringLiteral("xprintidle"));
    if (helper.isEmpty()) {
        cachedSystemIdleMs = -1;
        return fallback;
    }

    QProcess process;
    process.start(helper, {});
    if (!process.waitForStarted(150) || !process.waitForFinished(250)
        || process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        cachedSystemIdleMs = -1;
        return fallback;
    }

    bool ok = false;
    const qint64 value = QString::fromLatin1(process.readAllStandardOutput()).trimmed().toLongLong(&ok);
    if (!ok || value < 0) {
        cachedSystemIdleMs = -1;
        return fallback;
    }
    cachedSystemIdleMs = value;
    return cachedSystemIdleMs;
}

} // namespace UserActivity
