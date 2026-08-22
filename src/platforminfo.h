#pragma once

#include <QString>

struct RuntimeEnvironment
{
    QString osName;
    QString sessionType;
    QString desktop;
    QString terminal;
    QString mode;
    bool graphicalSession = false;
    bool ttyAttached = false;

    QString summary() const;
    static RuntimeEnvironment detect();
};
