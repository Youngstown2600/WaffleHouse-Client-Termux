#pragma once

#include "platforminfo.h"

#include <QList>
#include <QString>
#include <QStringList>

struct ClientCapability {
    QString key;
    QString label;
    bool available = false;
    QString detail;
};

class CapabilityRegistry {
public:
    static QList<ClientCapability> detect(const RuntimeEnvironment &runtime = RuntimeEnvironment::detect());
    static ClientCapability capability(const QString &key,
                                       const RuntimeEnvironment &runtime = RuntimeEnvironment::detect());
    static QStringList displayLines(const RuntimeEnvironment &runtime = RuntimeEnvironment::detect());
};
