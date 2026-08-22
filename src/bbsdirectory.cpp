#include "bbsdirectory.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

namespace {
quint16 validPort(const QString &text, quint16 fallback = 23) {
    bool ok = false; int p = text.trimmed().toInt(&ok);
    return ok && p > 0 && p <= 65535 ? static_cast<quint16>(p) : fallback;
}
BbsDirectoryEntry fromFields(QStringList f) {
    for (QString &s : f) s = s.trimmed().remove(QRegularExpression(QStringLiteral("^\"|\"$")));
    BbsDirectoryEntry e;
    if (f.size() >= 3) { e.name = f[0]; e.host = f[1]; e.port = validPort(f[2]); if (f.size() > 3 && !f[3].isEmpty()) e.terminalType = f[3]; }
    else if (f.size() == 2) { e.name = f[0]; e.host = f[1]; }
    else if (f.size() == 1) {
        QString one = f[0];
        if (one.startsWith(QStringLiteral("telnet://"), Qt::CaseInsensitive)) one.remove(0, 9);
        const int colon = one.lastIndexOf(':');
        if (colon > 0 && !one.contains(']')) { e.host = one.left(colon); e.port = validPort(one.mid(colon + 1)); }
        else e.host = one;
        e.name = e.host;
    }
    if (e.name.isEmpty()) e.name = e.host;
    return e;
}
}

QVector<BbsDirectoryEntry> BbsDirectory::loadFile(const QString &path, QString *error)
{
    QVector<BbsDirectoryEntry> result;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { if (error) *error = file.errorString(); return result; }
    const QByteArray data = file.readAll();
    const QString ext = QFileInfo(path).suffix().toCaseFolded();
    if (ext == QStringLiteral("json") || data.trimmed().startsWith('[')) {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isArray()) { if (error) *error = pe.errorString(); return result; }
        for (const QJsonValue &v : doc.array()) {
            if (!v.isObject()) continue;
            const QJsonObject o = v.toObject();
            BbsDirectoryEntry e;
            e.name = o.value(QStringLiteral("name")).toString();
            e.host = o.value(QStringLiteral("host")).toString(o.value(QStringLiteral("server")).toString());
            e.port = static_cast<quint16>(std::clamp(o.value(QStringLiteral("port")).toInt(23), 1, 65535));
            e.terminalType = o.value(QStringLiteral("terminal")).toString(QStringLiteral("ANSI"));
            if (e.name.isEmpty()) e.name = e.host;
            if (!e.host.isEmpty()) result << e;
        }
        return result;
    }
    const QString text = QString::fromUtf8(data);
    for (QString line : text.split(QRegularExpression(QStringLiteral("[\r\n]+")), Qt::SkipEmptyParts)) {
        line = line.trimmed();
        if (line.isEmpty() || line.startsWith('#') || line.startsWith(';')) continue;
        const QChar sep = line.contains('\t') ? '\t' : (line.contains(',') ? ',' : (line.contains('|') ? '|' : QChar()));
        QStringList fields = sep.isNull() ? QStringList{line} : line.split(sep);
        if (!fields.isEmpty() && (fields[0].compare(QStringLiteral("name"), Qt::CaseInsensitive) == 0
            || fields[0].compare(QStringLiteral("host"), Qt::CaseInsensitive) == 0)) continue;
        BbsDirectoryEntry e = fromFields(fields);
        if (!e.host.isEmpty()) result << e;
    }
    return result;
}
