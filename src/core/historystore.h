#pragma once

#include <QDateTime>
#include <QList>
#include <QString>

struct HistoryRecord {
    QDateTime timestamp;
    QString protocol;
    QString accountId;
    QString kind;
    QString target;
    QString direction;
    QString text;
};

class HistoryStore {
public:
    static QString path();
    static bool append(const HistoryRecord &record, QString *error = nullptr);
    static QList<HistoryRecord> search(const QString &query = {}, int limit = 100,
                                       const QString &protocol = {},
                                       const QString &target = {});
    static bool clear(QString *error = nullptr);
    static QStringList displayLines(const QList<HistoryRecord> &records);
};
