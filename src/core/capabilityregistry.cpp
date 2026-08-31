#include "core/capabilityregistry.h"

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QtGlobal>

namespace {
void add(QList<ClientCapability> &out, const QString &key, const QString &label,
         bool available, const QString &detail = {})
{
    out.push_back({key, label, available, detail});
}

QString findExternalExecutable(const QString &name)
{
    const QString onPath = QStandardPaths::findExecutable(name);
    if (!onPath.isEmpty()) return onPath;
#ifdef Q_OS_MACOS
    const QStringList dirs = {
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
        QStringLiteral("/opt/local/bin"),
        QStringLiteral("/sw/bin")
    };
    for (const QString &dir : dirs) {
        const QString candidate = QDir(dir).filePath(name);
        const QFileInfo info(candidate);
        if (info.exists() && info.isFile() && info.isExecutable()) return candidate;
    }
    if (name == QStringLiteral("mpv")) {
        const QString appBinary = QStringLiteral("/Applications/mpv.app/Contents/MacOS/mpv");
        const QFileInfo info(appBinary);
        if (info.exists() && info.isFile() && info.isExecutable()) return appBinary;
    }
#endif
    return {};
}

bool commandAvailable(const QString &name)
{
    return !findExternalExecutable(name).isEmpty();
}
}

QList<ClientCapability> CapabilityRegistry::detect(const RuntimeEnvironment &runtime)
{
    QList<ClientCapability> out;
#ifdef WAFFLEHOUSE_TERMUX
    const bool termux = true;
#else
    const bool termux = qEnvironmentVariableIsSet("TERMUX_VERSION")
        || qEnvironmentVariable("PREFIX").contains(QStringLiteral("com.termux"));
#endif
#ifdef WAFFLEHOUSE_GUI
    const bool guiBuilt = true;
#else
    const bool guiBuilt = false;
#endif
#ifdef WAFFLEHOUSE_OSCAR_VOICE
    const bool oscarVoice = true;
#else
    const bool oscarVoice = false;
#endif

    add(out, QStringLiteral("cli"), QStringLiteral("CLI / TUI"), true,
        QStringLiteral("ncurses shared frontend"));
    add(out, QStringLiteral("gui"), QStringLiteral("Qt GUI"), guiBuilt,
        guiBuilt ? QStringLiteral("Qt Widgets frontend compiled")
                 : QStringLiteral("not compiled for this target"));
    add(out, QStringLiteral("oscar"), QStringLiteral("AIM / OSCAR"), true,
        QStringLiteral("capability-driven OSCAR feature surface"));
    add(out, QStringLiteral("irc"), QStringLiteral("IRC"), true,
        QStringLiteral("IRC commands, rooms, PMs, WATCH/ISON buddy presence"));
    add(out, QStringLiteral("sip"), QStringLiteral("SIP / PJSIP"), true,
        QStringLiteral("PJSIP 2.17 multi-account softphone"));
    add(out, QStringLiteral("telnet"), QStringLiteral("Telnet / BBS"), true,
        QStringLiteral("ANSI terminal and saved BBS sessions"));
    add(out, QStringLiteral("secure"), QStringLiteral("Encrypted DM / Secure Rooms"), true,
        QStringLiteral("CPX3 secure channel and secure room support"));
    add(out, QStringLiteral("file-transfer"), QStringLiteral("File Transfer"), true,
        QStringLiteral("secure relay and encrypted direct transfer"));
    add(out, QStringLiteral("history"), QStringLiteral("Searchable Local History"), true,
        QStringLiteral("shared GUI/CLI JSONL history store"));
    add(out, QStringLiteral("unified-contacts"), QStringLiteral("Unified Contacts"), true,
        QStringLiteral("cross-protocol identities and SIP call targets"));
    add(out, QStringLiteral("sip-quality"), QStringLiteral("Live SIP Quality"), true,
        QStringLiteral("codec, RTP, loss, jitter, RTT, R-factor and MOS"));
    add(out, QStringLiteral("sip-transfer"), QStringLiteral("SIP Transfer"), true,
        QStringLiteral("blind and attended transfer"));
    add(out, QStringLiteral("command-palette"), QStringLiteral("Command Palette"), guiBuilt,
        guiBuilt ? QStringLiteral("Ctrl+Shift+P") : QStringLiteral("CLI slash commands provide the same command surface"));
    add(out, QStringLiteral("oscar-voice"), QStringLiteral("OSCAR Voice"), oscarVoice,
        oscarVoice ? QStringLiteral("Qt Multimedia UDP voice path compiled")
                   : QStringLiteral("audio backend unavailable on this target"));
    add(out, QStringLiteral("media"), QStringLiteral("WaffleRadio / Media"),
        commandAvailable(QStringLiteral("mpv")),
        commandAvailable(QStringLiteral("mpv")) ? QStringLiteral("mpv detected")
                                                 : QStringLiteral("mpv not detected; controls remain available but playback is disabled"));
    add(out, QStringLiteral("youtube"), QStringLiteral("YouTube Resolution"),
        commandAvailable(QStringLiteral("yt-dlp")),
        commandAvailable(QStringLiteral("yt-dlp")) ? QStringLiteral("yt-dlp detected")
                                                    : QStringLiteral("yt-dlp not detected"));
    add(out, QStringLiteral("desktop-notifications"), QStringLiteral("Desktop Notifications"),
        !termux && runtime.graphicalSession,
        !termux && runtime.graphicalSession ? QStringLiteral("desktop session detected")
                                            : QStringLiteral("terminal notification fallback is used"));
    add(out, QStringLiteral("termux"), QStringLiteral("Termux Integration"), termux,
        termux ? QStringLiteral("Android/Termux target") : QStringLiteral("not a Termux runtime"));
    add(out, QStringLiteral("termux-microphone"), QStringLiteral("Termux Microphone Preflight"),
        termux && commandAvailable(QStringLiteral("termux-microphone-record")),
        termux ? (commandAvailable(QStringLiteral("termux-microphone-record"))
                      ? QStringLiteral("Termux:API microphone helper detected")
                      : QStringLiteral("install Termux:API for Android microphone preflight"))
               : QStringLiteral("not applicable"));
    add(out, QStringLiteral("ssh-remote-audio"), QStringLiteral("SSH Remote Audio Companion"),
        qEnvironmentVariableIsSet("WAFFLEHOUSE_REMOTE_MEDIA_SOCKET")
            || qEnvironmentVariableIsSet("WAFFLEHOUSE_REMOTE_SIP_AUDIO"),
        QStringLiteral("activated automatically when the SSH companion exports its audio sockets"));
    return out;
}

ClientCapability CapabilityRegistry::capability(const QString &key, const RuntimeEnvironment &runtime)
{
    const QString wanted = key.trimmed().toCaseFolded();
    for (const ClientCapability &item : detect(runtime)) {
        if (item.key.toCaseFolded() == wanted) return item;
    }
    return {key, key, false, QStringLiteral("unknown capability")};
}

QStringList CapabilityRegistry::displayLines(const RuntimeEnvironment &runtime)
{
    QStringList lines;
    lines << QStringLiteral("Runtime: %1").arg(runtime.summary()) << QString();
    for (const ClientCapability &item : detect(runtime)) {
        lines << QStringLiteral("%-24s %1  %2")
                     .arg(item.available ? QStringLiteral("AVAILABLE") : QStringLiteral("UNAVAILABLE"),
                          item.detail)
                     .replace(QStringLiteral("%-24s"), item.label.leftJustified(24, QLatin1Char(' ')));
    }
    return lines;
}
