#include "platforminfo.h"

#ifdef WAFFLEHOUSE_TERMUX
#define WAFFLEHOUSE_TERMUX_PLATFORM 1
#endif

#include <QProcessEnvironment>
#include <QSysInfo>
#include <QStringList>

#include <unistd.h>

namespace {
QString firstEnvironmentValue(const QProcessEnvironment &env,
                              std::initializer_list<const char *> names)
{
    for (const char *name : names) {
        const QString value = env.value(QString::fromLatin1(name)).trimmed();
        if (!value.isEmpty()) return value;
    }
    return {};
}
}

RuntimeEnvironment RuntimeEnvironment::detect()
{
    RuntimeEnvironment info;
#if defined(WAFFLEHOUSE_TERMUX)
    info.osName = QStringLiteral("Android / Termux");
#elif defined(Q_OS_FREEBSD)
    info.osName = QStringLiteral("FreeBSD");
#elif defined(Q_OS_LINUX)
    info.osName = QStringLiteral("Linux");
#else
    info.osName = QSysInfo::prettyProductName();
    if (info.osName.isEmpty()) info.osName = QSysInfo::kernelType();
#endif

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString display = env.value(QStringLiteral("DISPLAY")).trimmed();
    const QString wayland = env.value(QStringLiteral("WAYLAND_DISPLAY")).trimmed();
    const QString xdgSession = env.value(QStringLiteral("XDG_SESSION_TYPE")).trimmed().toCaseFolded();

    info.graphicalSession = !display.isEmpty() || !wayland.isEmpty()
        || xdgSession == QStringLiteral("x11") || xdgSession == QStringLiteral("wayland");
    info.ttyAttached = ::isatty(STDIN_FILENO) || ::isatty(STDOUT_FILENO);

    if (!xdgSession.isEmpty() && xdgSession != QStringLiteral("tty")) {
        info.sessionType = xdgSession;
    } else if (!wayland.isEmpty()) {
        info.sessionType = QStringLiteral("wayland");
    } else if (!display.isEmpty()) {
        info.sessionType = QStringLiteral("x11");
    } else if (info.ttyAttached) {
        info.sessionType = QStringLiteral("tty");
    } else {
        info.sessionType = QStringLiteral("headless");
    }

    info.desktop = firstEnvironmentValue(env, {
        "XDG_CURRENT_DESKTOP", "XDG_SESSION_DESKTOP", "DESKTOP_SESSION", "GDMSESSION"
    });
    if (info.desktop.isEmpty() && env.contains(QStringLiteral("I3SOCK"))) {
        info.desktop = QStringLiteral("i3");
    }
    if (info.desktop.isEmpty() && env.contains(QStringLiteral("SWAYSOCK"))) {
        info.desktop = QStringLiteral("sway");
    }
    if (info.desktop.isEmpty() && env.contains(QStringLiteral("MATE_DESKTOP_SESSION_ID"))) {
        info.desktop = QStringLiteral("MATE");
    }
    if (info.desktop.isEmpty() && env.contains(QStringLiteral("GNOME_DESKTOP_SESSION_ID"))) {
        info.desktop = QStringLiteral("GNOME");
    }

    info.terminal = firstEnvironmentValue(env, {
        "TERM_PROGRAM", "TERMINAL_EMULATOR"
    });
    if (info.terminal.isEmpty() && env.contains(QStringLiteral("KONSOLE_VERSION"))) {
        info.terminal = QStringLiteral("Konsole");
    }
    if (info.terminal.isEmpty() && env.contains(QStringLiteral("VTE_VERSION"))) {
        info.terminal = QStringLiteral("VTE terminal");
    }
    if (info.terminal.isEmpty() && info.ttyAttached) {
        info.terminal = env.value(QStringLiteral("TERM")).trimmed();
    }

#if defined(WAFFLEHOUSE_TERMUX)
    info.mode = info.ttyAttached ? QStringLiteral("Termux terminal") : QStringLiteral("Termux non-interactive");
#else
    if (info.graphicalSession && info.ttyAttached) {
        info.mode = QStringLiteral("graphical terminal emulator");
    } else if (info.graphicalSession) {
        info.mode = QStringLiteral("graphical desktop session");
    } else if (info.ttyAttached) {
        info.mode = QStringLiteral("console/TTY (no GUI session)");
    } else {
        info.mode = QStringLiteral("headless/non-interactive");
    }
#endif

    return info;
}

QString RuntimeEnvironment::summary() const
{
    QStringList parts;
    parts << osName << mode;
    if (!sessionType.isEmpty()) parts << QStringLiteral("session=%1").arg(sessionType);
    if (!desktop.isEmpty()) parts << QStringLiteral("desktop=%1").arg(desktop);
    if (!terminal.isEmpty()) parts << QStringLiteral("terminal=%1").arg(terminal);
    return parts.join(QStringLiteral(" | "));
}
