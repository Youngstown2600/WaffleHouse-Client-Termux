#pragma once

#include <QList>
#include <QString>

struct UnifiedContactEndpoint {
    QString protocol;
    QString address;
    QString accountId;
    QString label;
};

struct UnifiedContact {
    QString id;
    QString displayName;
    QString notes;
    QList<UnifiedContactEndpoint> endpoints;
};

class ContactStore {
public:
    QList<UnifiedContact> contacts() const;
    UnifiedContact findByName(const QString &name, bool *ok = nullptr) const;
    UnifiedContact findByEndpoint(const QString &protocol, const QString &address, bool *ok = nullptr) const;
    bool upsert(const UnifiedContact &contact, QString *error = nullptr);
    bool remove(const QString &nameOrId, QString *error = nullptr);
    bool addEndpoint(const QString &contactName, const UnifiedContactEndpoint &endpoint,
                     QString *error = nullptr);
    bool setNotes(const QString &contactName, const QString &notes, QString *error = nullptr);

    static QString normalizedProtocol(QString protocol);
};
