#pragma once

#include <QString>
#include <QVector>
#include <QtGlobal>

struct BbsDirectoryEntry {
    QString name;
    QString host;
    quint16 port = 23;
    QString terminalType = QStringLiteral("ANSI");
};

class BbsDirectory {
public:
    static QVector<BbsDirectoryEntry> loadFile(const QString &path, QString *error = nullptr);
};
