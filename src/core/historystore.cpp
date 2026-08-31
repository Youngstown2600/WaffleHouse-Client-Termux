#include "core/historystore.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>

namespace {
QMutex &historyMutex()
{
    static QMutex mutex;
    return mutex;
}

QJsonObject toJson(const HistoryRecord &record)
{
    return {
        {QStringLiteral("timestamp"), record.timestamp.toUTC().toString(Qt::ISODateWithMs)},
        {QStringLiteral("protocol"), record.protocol},
        {QStringLiteral("accountId"), record.accountId},
        {QStringLiteral("kind"), record.kind},
        {QStringLiteral("target"), record.target},
        {QStringLiteral("direction"), record.direction},
        {QStringLiteral("text"), record.text}
    };
}

HistoryRecord fromJson(const QJsonObject &object)
{
    HistoryRecord r;
    r.timestamp = QDateTime::fromString(object.value(QStringLiteral("timestamp")).toString(), Qt::ISODateWithMs);
    if (!r.timestamp.isValid()) r.timestamp = QDateTime::fromString(object.value(QStringLiteral("timestamp")).toString(), Qt::ISODate);
    r.protocol = object.value(QStringLiteral("protocol")).toString();
    r.accountId = object.value(QStringLiteral("accountId")).toString();
    r.kind = object.value(QStringLiteral("kind")).toString();
    r.target = object.value(QStringLiteral("target")).toString();
    r.direction = object.value(QStringLiteral("direction")).toString();
    r.text = object.value(QStringLiteral("text")).toString();
    return r;
}
}

QString HistoryStore::path()
{
    QString root = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (root.isEmpty()) root = QDir::homePath() + QStringLiteral("/.wafflehouse-client");
    QDir().mkpath(root);
    return root + QStringLiteral("/history-v5.jsonl");
}

bool HistoryStore::append(const HistoryRecord &input, QString *error)
{
    HistoryRecord record = input;
    if (!record.timestamp.isValid()) record.timestamp = QDateTime::currentDateTimeUtc();
    QMutexLocker lock(&historyMutex());
    QFile file(path());
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = file.errorString();
        return false;
    }
    const QByteArray line = QJsonDocument(toJson(record)).toJson(QJsonDocument::Compact) + '\n';
    if (file.write(line) != line.size()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QList<HistoryRecord> HistoryStore::search(const QString &query, int limit,
                                          const QString &protocol, const QString &target)
{
    QList<HistoryRecord> matches;
    limit = qBound(1, limit, 2000);
    QMutexLocker lock(&historyMutex());
    QFile file(path());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return matches;
    const QString q = query.trimmed();
    const QString p = protocol.trimmed();
    const QString t = target.trimmed();
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonParseError parseError{};
        const QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) continue;
        const HistoryRecord r = fromJson(doc.object());
        if (!p.isEmpty() && r.protocol.compare(p, Qt::CaseInsensitive) != 0) continue;
        if (!t.isEmpty() && !r.target.contains(t, Qt::CaseInsensitive)) continue;
        if (!q.isEmpty()) {
            const QString haystack = r.protocol + QLatin1Char(' ') + r.target + QLatin1Char(' ')
                + r.kind + QLatin1Char(' ') + r.direction + QLatin1Char(' ') + r.text;
            if (!haystack.contains(q, Qt::CaseInsensitive)) continue;
        }
        matches.push_back(r);
        while (matches.size() > limit) matches.removeFirst();
    }
    return matches;
}

bool HistoryStore::clear(QString *error)
{
    QMutexLocker lock(&historyMutex());
    QFile file(path());
    if (!file.exists()) return true;
    if (!file.remove()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QStringList HistoryStore::displayLines(const QList<HistoryRecord> &records)
{
    QStringList lines;
    for (const HistoryRecord &r : records) {
        const QString when = r.timestamp.isValid()
            ? r.timestamp.toLocalTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
            : QStringLiteral("unknown-time");
        lines << QStringLiteral("%1  %-5s %-8s %-4s %-20s %2")
                     .arg(when, r.text)
                     .replace(QStringLiteral("%-5s"), r.protocol.leftJustified(5, QLatin1Char(' ')))
                     .replace(QStringLiteral("%-8s"), r.kind.leftJustified(8, QLatin1Char(' ')))
                     .replace(QStringLiteral("%-4s"), r.direction.leftJustified(4, QLatin1Char(' ')))
                     .replace(QStringLiteral("%-20s"), r.target.leftJustified(20, QLatin1Char(' ')));
    }
    return lines;
}
