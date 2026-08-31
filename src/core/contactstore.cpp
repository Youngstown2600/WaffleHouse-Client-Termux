#include "core/contactstore.h"

#include <QSettings>
#include <QUuid>

#include <algorithm>

namespace {
QString normalizedAddress(QString value, const QString &protocol)
{
    value = value.trimmed();
    if (protocol == QStringLiteral("aim") || protocol == QStringLiteral("oscar")) {
        value = value.toCaseFolded();
        value.remove(QLatin1Char(' '));
    } else if (protocol == QStringLiteral("irc") || protocol == QStringLiteral("sip")) {
        value = value.toCaseFolded();
    }
    return value;
}
}

QString ContactStore::normalizedProtocol(QString protocol)
{
    protocol = protocol.trimmed().toCaseFolded();
    if (protocol == QStringLiteral("oscar")) protocol = QStringLiteral("aim");
    if (protocol == QStringLiteral("voip") || protocol == QStringLiteral("phone")) protocol = QStringLiteral("sip");
    return protocol;
}

QList<UnifiedContact> ContactStore::contacts() const
{
    QSettings settings;
    QList<UnifiedContact> out;
    const int count = settings.beginReadArray(QStringLiteral("v5/unifiedContacts"));
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        UnifiedContact c;
        c.id = settings.value(QStringLiteral("id")).toString();
        c.displayName = settings.value(QStringLiteral("displayName")).toString();
        c.notes = settings.value(QStringLiteral("notes")).toString();
        const int endpointCount = settings.beginReadArray(QStringLiteral("endpoints"));
        for (int j = 0; j < endpointCount; ++j) {
            settings.setArrayIndex(j);
            c.endpoints.push_back({
                settings.value(QStringLiteral("protocol")).toString(),
                settings.value(QStringLiteral("address")).toString(),
                settings.value(QStringLiteral("accountId")).toString(),
                settings.value(QStringLiteral("label")).toString()
            });
        }
        settings.endArray();
        if (!c.displayName.trimmed().isEmpty()) out.push_back(c);
    }
    settings.endArray();
    return out;
}

UnifiedContact ContactStore::findByName(const QString &name, bool *ok) const
{
    if (ok) *ok = false;
    const QString wanted = name.trimmed().toCaseFolded();
    for (const UnifiedContact &contact : contacts()) {
        if (contact.displayName.trimmed().toCaseFolded() == wanted
            || contact.id.trimmed().toCaseFolded() == wanted) {
            if (ok) *ok = true;
            return contact;
        }
    }
    return {};
}

UnifiedContact ContactStore::findByEndpoint(const QString &protocol, const QString &address, bool *ok) const
{
    if (ok) *ok = false;
    const QString p = normalizedProtocol(protocol);
    const QString a = normalizedAddress(address, p);
    for (const UnifiedContact &contact : contacts()) {
        for (const UnifiedContactEndpoint &endpoint : contact.endpoints) {
            const QString ep = normalizedProtocol(endpoint.protocol);
            if (ep == p && normalizedAddress(endpoint.address, ep) == a) {
                if (ok) *ok = true;
                return contact;
            }
        }
    }
    return {};
}

bool ContactStore::upsert(const UnifiedContact &input, QString *error)
{
    UnifiedContact contact = input;
    contact.displayName = contact.displayName.trimmed();
    if (contact.displayName.isEmpty()) {
        if (error) *error = QStringLiteral("Contact name cannot be empty.");
        return false;
    }
    if (contact.id.trimmed().isEmpty()) contact.id = QUuid::createUuid().toString(QUuid::WithoutBraces);

    QList<UnifiedContact> all = contacts();
    int replaceIndex = -1;
    for (int i = 0; i < all.size(); ++i) {
        if (all.at(i).id == contact.id
            || all.at(i).displayName.compare(contact.displayName, Qt::CaseInsensitive) == 0) {
            replaceIndex = i;
            if (contact.id.trimmed().isEmpty()) contact.id = all.at(i).id;
            break;
        }
    }
    if (replaceIndex >= 0) all[replaceIndex] = contact;
    else all.push_back(contact);

    QSettings settings;
    settings.remove(QStringLiteral("v5/unifiedContacts"));
    settings.beginWriteArray(QStringLiteral("v5/unifiedContacts"));
    for (int i = 0; i < all.size(); ++i) {
        settings.setArrayIndex(i);
        const UnifiedContact &c = all.at(i);
        settings.setValue(QStringLiteral("id"), c.id);
        settings.setValue(QStringLiteral("displayName"), c.displayName);
        settings.setValue(QStringLiteral("notes"), c.notes);
        settings.beginWriteArray(QStringLiteral("endpoints"));
        for (int j = 0; j < c.endpoints.size(); ++j) {
            settings.setArrayIndex(j);
            const auto &ep = c.endpoints.at(j);
            settings.setValue(QStringLiteral("protocol"), normalizedProtocol(ep.protocol));
            settings.setValue(QStringLiteral("address"), ep.address.trimmed());
            settings.setValue(QStringLiteral("accountId"), ep.accountId.trimmed());
            settings.setValue(QStringLiteral("label"), ep.label.trimmed());
        }
        settings.endArray();
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool ContactStore::remove(const QString &nameOrId, QString *error)
{
    QList<UnifiedContact> all = contacts();
    const QString wanted = nameOrId.trimmed();
    const int before = all.size();
    all.erase(std::remove_if(all.begin(), all.end(), [&](const UnifiedContact &c) {
        return c.id == wanted || c.displayName.compare(wanted, Qt::CaseInsensitive) == 0;
    }), all.end());
    if (all.size() == before) {
        if (error) *error = QStringLiteral("Contact not found: %1").arg(nameOrId);
        return false;
    }
    QSettings settings;
    settings.remove(QStringLiteral("v5/unifiedContacts"));
    settings.beginWriteArray(QStringLiteral("v5/unifiedContacts"));
    for (int i = 0; i < all.size(); ++i) {
        settings.setArrayIndex(i);
        const auto &c = all.at(i);
        settings.setValue(QStringLiteral("id"), c.id);
        settings.setValue(QStringLiteral("displayName"), c.displayName);
        settings.setValue(QStringLiteral("notes"), c.notes);
        settings.beginWriteArray(QStringLiteral("endpoints"));
        for (int j = 0; j < c.endpoints.size(); ++j) {
            settings.setArrayIndex(j);
            const auto &ep = c.endpoints.at(j);
            settings.setValue(QStringLiteral("protocol"), normalizedProtocol(ep.protocol));
            settings.setValue(QStringLiteral("address"), ep.address.trimmed());
            settings.setValue(QStringLiteral("accountId"), ep.accountId.trimmed());
            settings.setValue(QStringLiteral("label"), ep.label.trimmed());
        }
        settings.endArray();
    }
    settings.endArray();
    settings.sync();
    return settings.status() == QSettings::NoError;
}

bool ContactStore::addEndpoint(const QString &contactName, const UnifiedContactEndpoint &input, QString *error)
{
    bool ok = false;
    UnifiedContact contact = findByName(contactName, &ok);
    if (!ok) {
        contact.displayName = contactName.trimmed();
        if (contact.displayName.isEmpty()) {
            if (error) *error = QStringLiteral("Contact name cannot be empty.");
            return false;
        }
    }
    UnifiedContactEndpoint endpoint = input;
    endpoint.protocol = normalizedProtocol(endpoint.protocol);
    endpoint.address = endpoint.address.trimmed();
    if (endpoint.protocol.isEmpty() || endpoint.address.isEmpty()) {
        if (error) *error = QStringLiteral("Endpoint protocol and address are required.");
        return false;
    }
    const QString normalized = normalizedAddress(endpoint.address, endpoint.protocol);
    for (auto &existing : contact.endpoints) {
        const QString epProtocol = normalizedProtocol(existing.protocol);
        if (epProtocol == endpoint.protocol && normalizedAddress(existing.address, epProtocol) == normalized) {
            existing = endpoint;
            return upsert(contact, error);
        }
    }
    contact.endpoints.push_back(endpoint);
    return upsert(contact, error);
}

bool ContactStore::setNotes(const QString &contactName, const QString &notes, QString *error)
{
    bool ok = false;
    UnifiedContact contact = findByName(contactName, &ok);
    if (!ok) {
        if (error) *error = QStringLiteral("Contact not found: %1").arg(contactName);
        return false;
    }
    contact.notes = notes;
    return upsert(contact, error);
}
