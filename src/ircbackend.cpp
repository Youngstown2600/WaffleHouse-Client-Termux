#include "ircbackend.h"
#include "appbranding.h"

#include <QAbstractSocket>
#include <QDateTime>
#include <QHostAddress>
#include <QHostInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QSslSocket>
#include <QTcpSocket>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

IrcBackend::IrcBackend(ConnectionSettings settings, QObject *parent)
    : ChatBackend(std::move(settings), parent),
      m_nickname(m_settings.username)
{
    for (const QString &name : m_settings.ircBuddies) {
        if (!name.trimmed().isEmpty()) m_watchBuddies.insert(name.trimmed());
    }
}

IrcBackend::~IrcBackend()
{
    m_stopRequested = true;
    if (m_thread && m_thread != QThread::currentThread()) {
        m_thread->wait();
    }
}

void IrcBackend::setConnectionSettings(const ConnectionSettings &settings)
{
    ChatBackend::setConnectionSettings(settings);
    m_nickname = m_settings.username;
    m_watchBuddies.clear();
    m_onlineWatchBuddies.clear();
    for (const QString &name : m_settings.ircBuddies) {
        if (!name.trimmed().isEmpty()) m_watchBuddies.insert(name.trimmed());
    }
}

void IrcBackend::start()
{
    if (m_thread) {
        return;
    }
    m_stopRequested = false;
    m_thread = QThread::create([this] { run(); });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this, [this] { m_thread = nullptr; }, Qt::QueuedConnection);
    m_thread->start();
}

void IrcBackend::stop()
{
    m_stopRequested = true;
    QThread *thread = m_thread;
    if (thread && thread != QThread::currentThread()) {
        thread->wait(15000);
    }
}

void IrcBackend::enqueue(Command command)
{
    QMutexLocker locker(&m_commandMutex);
    m_commands.enqueue(std::move(command));
}

QList<IrcBackend::Command> IrcBackend::takeCommands()
{
    QList<Command> result;
    QMutexLocker locker(&m_commandMutex);
    while (!m_commands.isEmpty()) {
        result.push_back(m_commands.dequeue());
    }
    return result;
}

void IrcBackend::sendPrivateMessage(const QString &target, const QString &message)
{
    enqueue({CommandType::SendIm, target, message});
}

void IrcBackend::requestClientVersion(const QString &target)
{
    const QString clean = target.trimmed();
    if (clean.isEmpty()) return;
    sendRaw(QStringLiteral("PRIVMSG %1 :\x01VERSION\x01").arg(clean));
}

void IrcBackend::requestWhois(const QString &target)
{
    const QString clean = target.trimmed();
    if (!clean.isEmpty()) sendRaw(QStringLiteral("WHOIS %1").arg(clean));
}

void IrcBackend::refreshServerCapabilities()
{
    sendRaw(QStringLiteral("CAP LS 302"));
}

void IrcBackend::joinRoom(const QString &room, bool)
{
    enqueue({CommandType::Join, room, {}});
}

QStringList IrcBackend::slashCommands()
{
    return {
        QStringLiteral("/ban"), QStringLiteral("/deop"), QStringLiteral("/devoice"),
        QStringLiteral("/invite"), QStringLiteral("/ison"), QStringLiteral("/j"),
        QStringLiteral("/join"), QStringLiteral("/kick"), QStringLiteral("/list"),
        QStringLiteral("/me"), QStringLiteral("/mode"), QStringLiteral("/motd"),
        QStringLiteral("/msg"), QStringLiteral("/names"), QStringLiteral("/nick"),
        QStringLiteral("/notice"), QStringLiteral("/op"), QStringLiteral("/part"),
        QStringLiteral("/query"), QStringLiteral("/quote"), QStringLiteral("/raw"),
        QStringLiteral("/say"), QStringLiteral("/topic"), QStringLiteral("/unban"),
        QStringLiteral("/voice"), QStringLiteral("/who"), QStringLiteral("/whois"),
        QStringLiteral("/whowas")
    };
}

bool IrcBackend::handleSlashCommand(const QString &contextTarget, const QString &input)
{
    QString line = input.trimmed();
    if (!line.startsWith(QLatin1Char('/'))) return false;

    line.remove(0, 1);
    line = line.trimmed();
    if (line.isEmpty()) return false;

    QString rest = line;
    auto takeArg = [](QString &value) {
        value = value.trimmed();
        if (value.isEmpty()) return QString();
        const int split = value.indexOf(QRegularExpression(QStringLiteral("\\s")));
        if (split < 0) {
            const QString result = value;
            value.clear();
            return result;
        }
        const QString result = value.left(split);
        value = value.mid(split + 1).trimmed();
        return result;
    };

    const QString command = takeArg(rest).toCaseFolded();
    const QString currentRoom = isChannel(contextTarget) ? contextTarget.trimmed() : QString();

    auto usage = [this, &currentRoom](const QString &text) {
        if (!currentRoom.isEmpty()) {
            emit eventReceived(QStringLiteral("chat"), currentRoom,
                               QStringLiteral("*** %1").arg(text));
        } else {
            emit eventReceived(QStringLiteral("status"), QString(), text);
        }
        return true;
    };
    auto requireRoom = [&]() {
        if (!currentRoom.isEmpty()) return true;
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("That IRC command requires an active channel."));
        return false;
    };
    auto sendModeForTargets = [&](QChar sign, QChar mode) {
        if (!requireRoom()) return true;
        const QStringList targets = rest.split(QRegularExpression(QStringLiteral("\\s+")),
                                               Qt::SkipEmptyParts);
        if (targets.isEmpty()) {
            return usage(QStringLiteral("Usage: /%1 NICK [NICK ...]").arg(command));
        }
        for (const QString &target : targets) {
            sendRaw(QStringLiteral("MODE %1 %2%3 %4")
                        .arg(currentRoom, QString(sign), QString(mode), target));
        }
        return true;
    };

    if (command == QStringLiteral("nick")) {
        const QString nick = takeArg(rest);
        if (nick.isEmpty()) return usage(QStringLiteral("Usage: /nick NEWNICK"));
        changeNickname(nick);
        return true;
    }

    if (command == QStringLiteral("join") || command == QStringLiteral("j")) {
        QString room = takeArg(rest);
        if (room.isEmpty()) return usage(QStringLiteral("Usage: /join #channel [key]"));
        room = canonicalChannel(room);
        const QString key = takeArg(rest);
        if (key.isEmpty()) joinRoom(room);
        else sendRaw(QStringLiteral("JOIN %1 %2").arg(room, key));
        return true;
    }

    if (command == QStringLiteral("part")) {
        QString room = currentRoom;
        QString reason = rest;
        QString probe = rest;
        const QString first = takeArg(probe);
        if (isChannel(first)) {
            room = first;
            reason = probe;
        }
        if (room.isEmpty()) return usage(QStringLiteral("Usage: /part [#channel] [reason]"));
        if (reason.isEmpty()) leaveRoom(room);
        else sendRaw(QStringLiteral("PART %1 :%2").arg(room, reason));
        return true;
    }

    if (command == QStringLiteral("me")) {
        if (!requireRoom()) return true;
        if (rest.isEmpty()) return usage(QStringLiteral("Usage: /me ACTION"));
        const QString payload = QString(QChar(0x01)) + QStringLiteral("ACTION ")
            + rest + QChar(0x01);
        sendRaw(QStringLiteral("PRIVMSG %1 :%2").arg(currentRoom, payload));
        emit eventReceived(QStringLiteral("chat"), currentRoom,
                           QStringLiteral("* %1 %2").arg(m_nickname, rest));
        return true;
    }

    if (command == QStringLiteral("notice")) {
        QString args = rest;
        const QString target = takeArg(args);
        if (target.isEmpty() || args.isEmpty()) {
            return usage(QStringLiteral("Usage: /notice TARGET MESSAGE"));
        }
        sendRaw(QStringLiteral("NOTICE %1 :%2").arg(target, args));
        return true;
    }

    if (command == QStringLiteral("op")) return sendModeForTargets(QLatin1Char('+'), QLatin1Char('o'));
    if (command == QStringLiteral("deop")) return sendModeForTargets(QLatin1Char('-'), QLatin1Char('o'));
    if (command == QStringLiteral("voice")) return sendModeForTargets(QLatin1Char('+'), QLatin1Char('v'));
    if (command == QStringLiteral("devoice")) return sendModeForTargets(QLatin1Char('-'), QLatin1Char('v'));

    if (command == QStringLiteral("ban") || command == QStringLiteral("unban")) {
        if (!requireRoom()) return true;
        QString mask = takeArg(rest);
        if (mask.isEmpty()) return usage(QStringLiteral("Usage: /%1 NICK|MASK").arg(command));
        if (!mask.contains(QLatin1Char('!')) && !mask.contains(QLatin1Char('@'))) {
            mask += QStringLiteral("!*@*");
        }
        sendRaw(QStringLiteral("MODE %1 %2b %3")
                    .arg(currentRoom,
                         command == QStringLiteral("ban") ? QStringLiteral("+") : QStringLiteral("-"),
                         mask));
        return true;
    }

    if (command == QStringLiteral("kick")) {
        QString args = rest;
        QString room = currentRoom;
        QString nick = takeArg(args);
        if (isChannel(nick)) {
            room = nick;
            nick = takeArg(args);
        }
        if (room.isEmpty() || nick.isEmpty()) {
            return usage(QStringLiteral("Usage: /kick [#channel] NICK [reason]"));
        }
        sendRaw(args.isEmpty()
                    ? QStringLiteral("KICK %1 %2").arg(room, nick)
                    : QStringLiteral("KICK %1 %2 :%3").arg(room, nick, args));
        return true;
    }

    if (command == QStringLiteral("topic")) {
        QString args = rest;
        QString room = currentRoom;
        QString first = takeArg(args);
        if (isChannel(first)) room = first;
        else args = rest;
        if (room.isEmpty()) return usage(QStringLiteral("Usage: /topic [#channel] [topic]"));
        sendRaw(args.isEmpty()
                    ? QStringLiteral("TOPIC %1").arg(room)
                    : QStringLiteral("TOPIC %1 :%2").arg(room, args));
        return true;
    }

    if (command == QStringLiteral("mode")) {
        QString args = rest;
        QString room = currentRoom;
        QString first = takeArg(args);
        if (isChannel(first)) room = first;
        else args = rest;
        if (room.isEmpty()) return usage(QStringLiteral("Usage: /mode [#channel] [modes [args]]"));
        sendRaw(args.isEmpty()
                    ? QStringLiteral("MODE %1").arg(room)
                    : QStringLiteral("MODE %1 %2").arg(room, args));
        return true;
    }

    if (command == QStringLiteral("names")) {
        QString room = takeArg(rest);
        if (room.isEmpty()) room = currentRoom;
        if (room.isEmpty()) return usage(QStringLiteral("Usage: /names [#channel]"));
        sendRaw(QStringLiteral("NAMES %1").arg(room));
        return true;
    }

    if (command == QStringLiteral("invite")) {
        QString args = rest;
        const QString nick = takeArg(args);
        QString room = takeArg(args);
        if (room.isEmpty()) room = currentRoom;
        if (nick.isEmpty() || room.isEmpty()) {
            return usage(QStringLiteral("Usage: /invite NICK [#channel]"));
        }
        sendRaw(QStringLiteral("INVITE %1 %2").arg(nick, room));
        return true;
    }

    if (command == QStringLiteral("whois") || command == QStringLiteral("whowas")) {
        const QString nick = takeArg(rest);
        if (nick.isEmpty()) return usage(QStringLiteral("Usage: /%1 NICK").arg(command));
        sendRaw(QStringLiteral("%1 %2").arg(command.toUpper(), nick));
        return true;
    }

    if (command == QStringLiteral("who")) {
        QString target = takeArg(rest);
        if (target.isEmpty()) target = currentRoom;
        if (target.isEmpty()) return usage(QStringLiteral("Usage: /who TARGET"));
        sendRaw(QStringLiteral("WHO %1").arg(target));
        return true;
    }

    if (command == QStringLiteral("ison")) {
        const QString targets = rest.trimmed();
        if (targets.isEmpty()) return usage(QStringLiteral("Usage: /ison NICK [NICK ...]"));
        sendRaw(QStringLiteral("ISON %1").arg(targets));
        return true;
    }

    if (command == QStringLiteral("list")) {
        sendRaw(rest.isEmpty() ? QStringLiteral("LIST") : QStringLiteral("LIST %1").arg(rest));
        return true;
    }

    if (command == QStringLiteral("motd")) {
        sendRaw(rest.isEmpty() ? QStringLiteral("MOTD") : QStringLiteral("MOTD %1").arg(rest));
        return true;
    }

    if (command == QStringLiteral("raw") || command == QStringLiteral("quote")) {
        if (rest.isEmpty()) return usage(QStringLiteral("Usage: /%1 IRC-COMMAND").arg(command));
        sendRaw(rest);
        return true;
    }

    return false;
}

void IrcBackend::sendRoomMessage(const QString &room, const QString &message)
{
    enqueue({CommandType::SendRoom, room, message});
}

void IrcBackend::leaveRoom(const QString &room)
{
    enqueue({CommandType::Part, room, {}});
}

void IrcBackend::sendRaw(const QString &line, const QString &, const QString &)
{
    // IRC's wire protocol does not use the leading slash that interactive
    // clients use for commands.  Accept either spelling so GUI/CLI users can
    // type "/PART #channel" or "PART #channel" without accidentally
    // transmitting an invalid literal slash to the server.
    QString clean = line.trimmed();
    if (clean.startsWith(QLatin1Char('/'))) {
        clean.remove(0, 1);
        clean = clean.trimmed();
    }

    // IRC commands are conventionally uppercase.  Only normalize the command
    // token; preserve all arguments/trailing text exactly as entered.
    int commandEnd = 0;
    while (commandEnd < clean.size() && !clean.at(commandEnd).isSpace()) {
        ++commandEnd;
    }
    if (commandEnd > 0) {
        clean.replace(0, commandEnd, clean.left(commandEnd).toUpper());
    }

    enqueue({CommandType::Raw, clean, {}});
}

void IrcBackend::changeNickname(const QString &newNick)
{
    enqueue({CommandType::Nick, newNick, {}});
}

void IrcBackend::addBuddy(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return;
    bool found = false;
    for (const QString &existing : m_settings.ircBuddies) {
        if (existing.compare(clean, Qt::CaseInsensitive) == 0) { found = true; break; }
    }
    if (!found) m_settings.ircBuddies.append(clean);
    m_settings.ircBuddies.sort(Qt::CaseInsensitive);
    emit buddyListChanged(m_settings.ircBuddies);
    enqueue({CommandType::WatchAdd, clean, {}});
}

void IrcBackend::removeBuddy(const QString &name)
{
    const QString clean = name.trimmed();
    if (clean.isEmpty()) return;
    for (auto it = m_settings.ircBuddies.begin(); it != m_settings.ircBuddies.end();) {
        if (it->compare(clean, Qt::CaseInsensitive) == 0) it = m_settings.ircBuddies.erase(it);
        else ++it;
    }
    emit buddyListChanged(m_settings.ircBuddies);
    enqueue({CommandType::WatchRemove, clean, {}});
}

bool IrcBackend::isChannel(const QString &target)
{
    if (target.isEmpty()) {
        return false;
    }
    return QStringLiteral("#&+!").contains(target.front());
}

QString IrcBackend::canonicalChannel(QString room)
{
    room = room.trimmed();
    if (!room.isEmpty() && !isChannel(room)) {
        room.prepend(QLatin1Char('#'));
    }
    return room;
}

QString IrcBackend::nickFromPrefix(const QString &prefix)
{
    return prefix.section(QLatin1Char('!'), 0, 0);
}

QString IrcBackend::stripMemberPrefix(QString nick)
{
    while (!nick.isEmpty() && QStringLiteral("~&@%+").contains(nick.front())) {
        nick.remove(0, 1);
    }
    return nick;
}


QString IrcBackend::stripFormatting(const QString &text)
{
    // IRC formatting controls are transport-level decoration.  The TUI and
    // GUI share this backend, so normalize them here instead of allowing raw
    // C0 bytes to appear as ^B/^O/etc. in topics or conversation text.
    QString out;
    out.reserve(text.size());

    for (qsizetype i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        const ushort u = ch.unicode();

        // Common IRC toggles: bold, reset, monospace, reverse, italic,
        // strikethrough and underline.
        if (u == 0x02 || u == 0x0F || u == 0x11 || u == 0x16
            || u == 0x1D || u == 0x1E || u == 0x1F) {
            continue;
        }

        // mIRC numeric colors: ^C[fg][,bg], with one or two decimal digits.
        if (u == 0x03) {
            int digits = 0;
            while (i + 1 < text.size() && digits < 2 && text.at(i + 1).isDigit()) {
                ++i;
                ++digits;
            }
            if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char(',')) {
                const qsizetype comma = i + 1;
                qsizetype j = comma + 1;
                int bgDigits = 0;
                while (j < text.size() && bgDigits < 2 && text.at(j).isDigit()) {
                    ++j;
                    ++bgDigits;
                }
                if (bgDigits > 0) i = j - 1;
            }
            continue;
        }

        // IRCv3 hex colors: ^Drrggbb[,rrggbb].  Only consume a color when a
        // complete six-digit hex value follows so ordinary text is preserved.
        if (u == 0x04) {
            auto isHex = [](QChar c) {
                const ushort v = c.unicode();
                return (v >= '0' && v <= '9') || (v >= 'a' && v <= 'f')
                    || (v >= 'A' && v <= 'F');
            };
            qsizetype j = i + 1;
            bool foreground = j + 6 <= text.size();
            for (qsizetype k = 0; foreground && k < 6; ++k) foreground = isHex(text.at(j + k));
            if (foreground) {
                i = j + 5;
                if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char(',')) {
                    j = i + 2;
                    bool background = j + 6 <= text.size();
                    for (qsizetype k = 0; background && k < 6; ++k) background = isHex(text.at(j + k));
                    if (background) i = j + 5;
                }
            }
            continue;
        }

        // Do not allow embedded CR/LF or other C0 controls to leak into the
        // curses/Qt renderer. Preserve TAB for readable server text.
        if ((u < 0x20 && u != 0x09) || u == 0x7F) {
            continue;
        }

        out.append(ch);
    }
    return out;
}

IrcBackend::ParsedLine IrcBackend::parseLine(const QString &line)
{
    ParsedLine parsed;
    QString rest = line;

    if (rest.startsWith(QLatin1Char(':'))) {
        const int space = rest.indexOf(QLatin1Char(' '));
        if (space < 0) {
            return parsed;
        }
        parsed.prefix = rest.mid(1, space - 1);
        rest = rest.mid(space + 1);
    }

    QString trailing;
    const int trailingPos = rest.indexOf(QStringLiteral(" :"));
    if (trailingPos >= 0) {
        trailing = rest.mid(trailingPos + 2);
        rest = rest.left(trailingPos);
    }

    QStringList parts = rest.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        return parsed;
    }

    parsed.command = parts.takeFirst().toUpper();
    parsed.params = parts;
    if (trailingPos >= 0) {
        parsed.params.push_back(trailing);
    }
    return parsed;
}

void IrcBackend::sendLine(QAbstractSocket &socket, const QString &line)
{
    QString clean = line;
    clean.remove(QLatin1Char('\r'));
    clean.remove(QLatin1Char('\n'));

    if (m_settings.debug) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC =>] %1").arg(clean));
    }

    const QByteArray bytes = clean.toUtf8() + "\r\n";
    qint64 offset = 0;
    while (offset < bytes.size()) {
        const qint64 written = socket.write(bytes.constData() + offset, bytes.size() - offset);
        if (written < 0) {
            throw std::runtime_error(
                QStringLiteral("IRC write failed: %1").arg(socket.errorString()).toStdString());
        }
        offset += written;
        if (socket.bytesToWrite() > 0 && !socket.waitForBytesWritten(5000)) {
            throw std::runtime_error(
                QStringLiteral("IRC write timed out: %1").arg(socket.errorString()).toStdString());
        }
    }
}

QString IrcBackend::readLine(QAbstractSocket &socket, int timeoutMs)
{
    while (!socket.canReadLine()) {
        if (!socket.waitForReadyRead(timeoutMs)) {
            if (socket.state() != QAbstractSocket::ConnectedState) {
                throw std::runtime_error("IRC server closed the connection");
            }
            return {};
        }
    }
    QString line = QString::fromUtf8(socket.readLine());
    while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) {
        line.chop(1);
    }
    return line;
}

void IrcBackend::addMembers(const QString &room, const QStringList &names)
{
    const QString key = room.toCaseFolded();
    m_roomNames[key] = room;
    auto &members = m_members[key];
    for (const QString &name : names) {
        if (!name.isEmpty()) {
            members.insert(name);
        }
    }
    emit membersChanged(room, QStringLiteral("add"), names);
}

void IrcBackend::removeMembers(const QString &room, const QStringList &names)
{
    const QString key = room.toCaseFolded();
    auto it = m_members.find(key);
    if (it != m_members.end()) {
        for (const QString &name : names) {
            for (auto memberIt = it->begin(); memberIt != it->end();) {
                if (memberIt->compare(name, Qt::CaseInsensitive) == 0) {
                    memberIt = it.value().erase(memberIt);
                } else {
                    ++memberIt;
                }
            }
        }
    }
    emit membersChanged(m_roomNames.value(key, room), QStringLiteral("remove"), names);
}

void IrcBackend::replaceMembers(const QString &room, const QStringList &names)
{
    const QString key = room.toCaseFolded();
    m_roomNames[key] = room;
    QSet<QString> replacement;
    for (const QString &name : names) {
        if (!name.isEmpty()) {
            replacement.insert(name);
        }
    }
    m_members[key] = replacement;
    emit membersChanged(room, QStringLiteral("replace"), names);
}

void IrcBackend::emitServerCapabilities()
{
    QStringList caps = m_ircv3Capabilities.values();
    caps.sort(Qt::CaseInsensitive);

    QStringList isupport;
    QStringList keys = m_isupportTokens.keys();
    keys.sort(Qt::CaseInsensitive);
    for (const QString &key : keys) {
        const QString value = m_isupportTokens.value(key);
        isupport.append(value.isEmpty() ? key : QStringLiteral("%1=%2").arg(key, value));
    }
    emit serverCapabilitiesChanged(caps, isupport);
}

void IrcBackend::processLine(const QString &line)
{
    if (m_settings.debug) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC <=] %1").arg(line));
    }

    const ParsedLine parsed = parseLine(line);
    const QString nick = nickFromPrefix(parsed.prefix);

    if (parsed.command == QStringLiteral("CAP") && parsed.params.size() >= 2) {
        const QString subcommand = parsed.params.at(1).toUpper();
        if (subcommand == QStringLiteral("LS") && !parsed.params.isEmpty()) {
            const QString capabilityText = parsed.params.back();
            for (const QString &raw : capabilityText.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const QString capability = raw.trimmed();
                if (!capability.isEmpty()) m_ircv3Capabilities.insert(capability);
            }
            emitServerCapabilities();
        } else if ((subcommand == QStringLiteral("NEW") || subcommand == QStringLiteral("DEL"))
                   && !parsed.params.isEmpty()) {
            const bool add = subcommand == QStringLiteral("NEW");
            for (const QString &raw : parsed.params.back().split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                const QString capability = raw.trimmed();
                const QString capabilityName = capability.section(QLatin1Char('='), 0, 0);
                if (capabilityName.isEmpty()) continue;
                if (add) {
                    for (auto it = m_ircv3Capabilities.begin(); it != m_ircv3Capabilities.end(); ) {
                        if (it->section(QLatin1Char('='), 0, 0) == capabilityName) it = m_ircv3Capabilities.erase(it);
                        else ++it;
                    }
                    m_ircv3Capabilities.insert(capability);
                } else {
                    for (auto it = m_ircv3Capabilities.begin(); it != m_ircv3Capabilities.end(); ) {
                        if (it->section(QLatin1Char('='), 0, 0) == capabilityName) it = m_ircv3Capabilities.erase(it);
                        else ++it;
                    }
                }
            }
            emitServerCapabilities();
        }
        return;
    }

    if (parsed.command == QStringLiteral("005") && parsed.params.size() >= 2) {
        // RPL_ISUPPORT: first parameter is our nick; the final trailing parameter
        // is descriptive prose. Everything in between is a server feature token.
        for (int i = 1; i < parsed.params.size(); ++i) {
            const QString token = parsed.params.at(i).trimmed();
            if (token.isEmpty() || token.contains(QLatin1Char(' '))) continue;
            if (token.startsWith(QLatin1Char('-'))) {
                m_isupportTokens.remove(token.mid(1).section(QLatin1Char('='), 0, 0).toUpper());
                continue;
            }
            const QString key = token.section(QLatin1Char('='), 0, 0).toUpper();
            const QString value = token.contains(QLatin1Char('='))
                ? token.section(QLatin1Char('='), 1) : QString();
            if (!key.isEmpty()) m_isupportTokens.insert(key, value);
        }
        emitServerCapabilities();
        return;
    }

    if (parsed.command == QStringLiteral("PRIVMSG") && parsed.params.size() >= 2) {
        const QString target = parsed.params[0];
        const QString rawText = parsed.params[1];
        if (!isChannel(target)
            && rawText == QString(QChar(0x01)) + QStringLiteral("VERSION") + QChar(0x01)) {
            emit eventReceived(QStringLiteral("version-request"), nick, QString());
            return;
        }
        const QString text = stripFormatting(rawText);
        if (isChannel(target)) {
            emit eventReceived(QStringLiteral("chat"), target,
                               QStringLiteral("<%1> %2").arg(nick, text));
        } else {
            emit eventReceived(QStringLiteral("im"), nick,
                               QStringLiteral("<%1> %2").arg(nick, text));
        }
        return;
    }

    if (parsed.command == QStringLiteral("NOTICE") && parsed.params.size() >= 2) {
        const QString target = parsed.params[0];
        const QString sender = nick.isEmpty() ? parsed.prefix : nick;
        const QString rawText = parsed.params[1];
        const QString ctcpPrefix = QString(QChar(0x01)) + QStringLiteral("VERSION ");
        if (!isChannel(target) && rawText.startsWith(ctcpPrefix) && rawText.endsWith(QChar(0x01))) {
            const QString reported = rawText.mid(ctcpPrefix.size(), rawText.size() - ctcpPrefix.size() - 1).trimmed();
            emit eventReceived(QStringLiteral("version"), sender, reported);
            return;
        }
        if (isChannel(target)) {
            emit eventReceived(QStringLiteral("chat"), target,
                               QStringLiteral("-%1- %2").arg(sender, stripFormatting(rawText)));
        } else {
            emit eventReceived(QStringLiteral("status"), QString(),
                               QStringLiteral("-%1- %2").arg(sender, stripFormatting(rawText)));
        }
        return;
    }

    if (parsed.command == QStringLiteral("JOIN") && !parsed.params.isEmpty()) {
        const QString room = parsed.params.back();
        addMembers(room, {nick});
        emit eventReceived(QStringLiteral("chat"), room,
                           QStringLiteral("*** %1 joined").arg(nick));
        return;
    }

    if (parsed.command == QStringLiteral("PART") && !parsed.params.isEmpty()) {
        const QString room = parsed.params[0];
        removeMembers(room, {nick});
        const QString reason = parsed.params.size() >= 2 ? stripFormatting(parsed.params[1]) : QString();
        emit eventReceived(QStringLiteral("chat"), room,
                           QStringLiteral("*** %1 left%2")
                               .arg(nick,
                                    reason.isEmpty() ? QString()
                                                     : QStringLiteral(" (%1)").arg(reason)));
        return;
    }

    if (parsed.command == QStringLiteral("QUIT")) {
        const QString reason = parsed.params.isEmpty() ? QString() : stripFormatting(parsed.params[0]);
        for (auto it = m_members.begin(); it != m_members.end(); ++it) {
            QString actual;
            for (const QString &member : it.value()) {
                if (member.compare(nick, Qt::CaseInsensitive) == 0) {
                    actual = member;
                    break;
                }
            }
            if (!actual.isEmpty()) {
                const QString room = m_roomNames.value(it.key(), it.key());
                removeMembers(room, {actual});
                emit eventReceived(QStringLiteral("chat"), room,
                                   QStringLiteral("*** %1 quit%2")
                                       .arg(actual,
                                            reason.isEmpty() ? QString()
                                                             : QStringLiteral(" (%1)").arg(reason)));
            }
        }
        return;
    }

    if (parsed.command == QStringLiteral("NICK") && !parsed.params.isEmpty()) {
        const QString newNick = parsed.params.back();
        const QString oldNick = nick;
        if (oldNick.compare(m_nickname, Qt::CaseInsensitive) == 0) {
            m_nickname = newNick;
        }

        for (auto it = m_members.begin(); it != m_members.end(); ++it) {
            QString actual;
            for (const QString &member : it.value()) {
                if (member.compare(oldNick, Qt::CaseInsensitive) == 0) {
                    actual = member;
                    break;
                }
            }
            if (!actual.isEmpty()) {
                it.value().remove(actual);
                it.value().insert(newNick);
                const QString room = m_roomNames.value(it.key(), it.key());
                emit membersChanged(room, QStringLiteral("remove"), {actual});
                emit membersChanged(room, QStringLiteral("add"), {newNick});
                emit eventReceived(QStringLiteral("chat"), room,
                                   QStringLiteral("*** %1 is now known as %2")
                                       .arg(actual, newNick));
            }
        }
        return;
    }

    // RPL_NAMREPLY: <me> <symbol> <channel> :names...
    if (parsed.command == QStringLiteral("353") && parsed.params.size() >= 4) {
        const QString room = parsed.params[2];
        const QString key = room.toCaseFolded();
        m_roomNames[key] = room;
        auto &pending = m_pendingNames[key];
        for (QString member : parsed.params[3].split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
            member = stripMemberPrefix(member);
            if (!member.isEmpty()) {
                pending.insert(member);
            }
        }
        return;
    }

    // RPL_ENDOFNAMES
    if (parsed.command == QStringLiteral("366") && parsed.params.size() >= 2) {
        const QString room = parsed.params[1];
        const QString key = room.toCaseFolded();
        const QSet<QString> pending = m_pendingNames.take(key);
        QStringList names = pending.values();
        names.sort(Qt::CaseInsensitive);
        replaceMembers(room, names);
        return;
    }

    if (parsed.command == QStringLiteral("KICK") && parsed.params.size() >= 2) {
        const QString room = parsed.params[0];
        const QString victim = parsed.params[1];
        const QString reason = parsed.params.size() >= 3 ? stripFormatting(parsed.params[2]) : QString();
        removeMembers(room, {victim});
        emit eventReceived(QStringLiteral("chat"), room,
                           QStringLiteral("*** %1 was kicked by %2%3")
                               .arg(victim,
                                    nick,
                                    reason.isEmpty() ? QString()
                                                     : QStringLiteral(" (%1)").arg(reason)));
        return;
    }

    if (parsed.command == QStringLiteral("MODE") && parsed.params.size() >= 2) {
        const QString target = parsed.params[0];
        const QString details = parsed.params.mid(1).join(QLatin1Char(' '));
        if (isChannel(target)) {
            emit eventReceived(QStringLiteral("chat"), target,
                               QStringLiteral("*** %1 sets mode %2")
                                   .arg(nick.isEmpty() ? QStringLiteral("server") : nick, details));
        } else {
            emit eventReceived(QStringLiteral("status"), QString(),
                               QStringLiteral("*** MODE %1 %2").arg(target, details));
        }
        return;
    }

    if (parsed.command == QStringLiteral("TOPIC") && parsed.params.size() >= 2) {
        emit eventReceived(QStringLiteral("chat"), parsed.params[0],
                           QStringLiteral("*** topic set by %1: %2")
                               .arg(nick, stripFormatting(parsed.params[1])));
        return;
    }

    if (parsed.command == QStringLiteral("332") && parsed.params.size() >= 3) {
        emit eventReceived(QStringLiteral("chat"), parsed.params[1],
                           QStringLiteral("*** topic: %1").arg(stripFormatting(parsed.params[2])));
        return;
    }

    // RPL_TOPICWHOTIME: <me> <channel> <setter> <unix-time>
    if (parsed.command == QStringLiteral("333") && parsed.params.size() >= 4) {
        bool ok = false;
        const qint64 epoch = parsed.params[3].toLongLong(&ok);
        QString when = parsed.params[3];
        if (ok) {
            when = QDateTime::fromSecsSinceEpoch(epoch).toLocalTime()
                       .toString(QStringLiteral("ddd MMM d HH:mm:ss yyyy"));
        }
        emit eventReceived(QStringLiteral("chat"), parsed.params[1],
                           QStringLiteral("*** topic set by %1 [%2]")
                               .arg(stripFormatting(parsed.params[2]), when));
        return;
    }

    // RPL_ISON: <me> :nick1 nick2 ...
    // The list is used for the local per-profile IRC buddy/watch feature.
    if (parsed.command == QStringLiteral("303") && parsed.params.size() >= 2) {
        QSet<QString> online;
        for (const QString &name : parsed.params.back().split(QLatin1Char(' '), Qt::SkipEmptyParts))
            online.insert(name.toCaseFolded());
        for (const QString &watched : m_watchBuddies) {
            const QString folded = watched.toCaseFolded();
            const bool isOnline = online.contains(folded);
            const bool wasOnline = m_onlineWatchBuddies.contains(folded);
            if (isOnline != wasOnline) emit buddyPresenceChanged(watched, isOnline);
        }
        m_onlineWatchBuddies = online;
        return;
    }

    static const QSet<QString> interestingErrors = {
        QStringLiteral("401"), QStringLiteral("403"), QStringLiteral("404"),
        QStringLiteral("405"), QStringLiteral("441"), QStringLiteral("442"),
        QStringLiteral("443"), QStringLiteral("461"), QStringLiteral("467"),
        QStringLiteral("471"), QStringLiteral("472"), QStringLiteral("473"),
        QStringLiteral("474"), QStringLiteral("475"), QStringLiteral("478"),
        QStringLiteral("482"), QStringLiteral("501"), QStringLiteral("502")
    };
    if (interestingErrors.contains(parsed.command)) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC %1] %2")
                               .arg(parsed.command, parsed.params.join(QLatin1Char(' '))));
        return;
    }

    static const QSet<QString> whoisReplies = {
        QStringLiteral("301"), QStringLiteral("311"), QStringLiteral("312"),
        QStringLiteral("313"), QStringLiteral("317"), QStringLiteral("318"),
        QStringLiteral("319"), QStringLiteral("330"), QStringLiteral("338"),
        QStringLiteral("671")
    };
    if (whoisReplies.contains(parsed.command) && parsed.params.size() >= 2) {
        const QString whoisNick = parsed.params.at(1);
        QString human;
        const QStringList p = parsed.params;
        if (parsed.command == QStringLiteral("311") && p.size() >= 6) {
            human = QStringLiteral("User: %1@%2 | Real name: %3")
                        .arg(p.at(2), p.at(3), p.mid(5).join(QLatin1Char(' ')));
        } else if (parsed.command == QStringLiteral("312") && p.size() >= 4) {
            human = QStringLiteral("Server: %1 — %2").arg(p.at(2), p.mid(3).join(QLatin1Char(' ')));
        } else if (parsed.command == QStringLiteral("313")) {
            human = QStringLiteral("IRC operator: yes");
        } else if (parsed.command == QStringLiteral("317") && p.size() >= 4) {
            bool okIdle = false;
            bool okSignon = false;
            const qint64 idle = p.at(2).toLongLong(&okIdle);
            const qint64 signon = p.at(3).toLongLong(&okSignon);
            human = QStringLiteral("Idle: %1 sec | Signed on: %2")
                        .arg(okIdle ? QString::number(idle) : p.at(2),
                             okSignon ? QDateTime::fromSecsSinceEpoch(signon).toLocalTime().toString(Qt::ISODate) : p.at(3));
        } else if (parsed.command == QStringLiteral("319") && p.size() >= 3) {
            human = QStringLiteral("Channels: %1").arg(p.mid(2).join(QLatin1Char(' ')));
        } else if (parsed.command == QStringLiteral("330") && p.size() >= 3) {
            human = QStringLiteral("Services account: %1").arg(p.at(2));
        } else if (parsed.command == QStringLiteral("338") && p.size() >= 3) {
            human = QStringLiteral("Actual host/IP: %1").arg(p.at(2));
        } else if (parsed.command == QStringLiteral("301") && p.size() >= 3) {
            human = QStringLiteral("Away: %1").arg(p.mid(2).join(QLatin1Char(' ')));
        } else if (parsed.command == QStringLiteral("671")) {
            human = QStringLiteral("Secure connection: yes (TLS)");
        } else if (parsed.command == QStringLiteral("318")) {
            human = QStringLiteral("Last Updated: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate));
        } else {
            human = QStringLiteral("[%1] %2")
                        .arg(parsed.command, parsed.params.mid(1).join(QLatin1Char(' ')));
        }
        emit whoisReply(whoisNick, human, parsed.command == QStringLiteral("318"));
        emit eventReceived(QStringLiteral("status"), QString(), QStringLiteral("[IRC %1] %2")
                               .arg(parsed.command, parsed.params.join(QLatin1Char(' '))));
        return;
    }

    // Surface the standard numeric replies produced by interactive IRC
    // commands such as WHO/WHOIS/LIST/MODE/INVITE/MOTD and ban-list queries.
    // Without this, the command can succeed on the wire while appearing to do
    // nothing in both frontends.
    static const QSet<QString> commandReplies = {
        QStringLiteral("301"), QStringLiteral("311"), QStringLiteral("312"),
        QStringLiteral("313"), QStringLiteral("315"), QStringLiteral("317"),
        QStringLiteral("318"), QStringLiteral("319"), QStringLiteral("321"),
        QStringLiteral("322"), QStringLiteral("323"), QStringLiteral("324"),
        QStringLiteral("329"), QStringLiteral("330"), QStringLiteral("338"),
        QStringLiteral("341"), QStringLiteral("346"), QStringLiteral("347"),
        QStringLiteral("348"), QStringLiteral("349"), QStringLiteral("352"),
        QStringLiteral("354"), QStringLiteral("367"), QStringLiteral("368"),
        QStringLiteral("372"), QStringLiteral("375"), QStringLiteral("376"),
        QStringLiteral("671")
    };
    if (commandReplies.contains(parsed.command)) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC %1] %2")
                               .arg(parsed.command, parsed.params.join(QLatin1Char(' '))));
        return;
    }
}

void IrcBackend::run()
{
    QString disconnectReason = QStringLiteral("signed off");

    try {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC] Resolving %1…").arg(m_settings.server));

        QList<QHostAddress> addresses;
        QHostAddress literalAddress;
        if (literalAddress.setAddress(m_settings.server)) {
            addresses.append(literalAddress);
        } else {
            const QHostInfo info = QHostInfo::fromName(m_settings.server);
            if (info.error() != QHostInfo::NoError) {
                throw std::runtime_error(
                    QStringLiteral("IRC DNS lookup failed for %1: %2")
                        .arg(m_settings.server, info.errorString()).toStdString());
            }
            addresses = info.addresses();
        }

        if (addresses.isEmpty()) {
            throw std::runtime_error(
                QStringLiteral("IRC DNS lookup returned no addresses for %1")
                    .arg(m_settings.server).toStdString());
        }

        // Prefer IPv4 first. A surprisingly common failure mode is a hostname
        // advertising IPv6 while the local network has no working IPv6 route.
        std::stable_sort(addresses.begin(), addresses.end(), [](const QHostAddress &a,
                                                                 const QHostAddress &b) {
            const bool aV4 = a.protocol() == QAbstractSocket::IPv4Protocol;
            const bool bV4 = b.protocol() == QAbstractSocket::IPv4Protocol;
            return aV4 && !bV4;
        });

        QStringList addressText;
        for (const QHostAddress &address : addresses) {
            if (!addressText.contains(address.toString())) {
                addressText.append(address.toString());
            }
        }
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC] Resolved %1 → %2")
                               .arg(m_settings.server, addressText.join(QStringLiteral(", "))));

        std::unique_ptr<QAbstractSocket> socket;
        QStringList connectionErrors;

        for (const QHostAddress &address : addresses) {
            if (m_stopRequested) {
                break;
            }

            const QString mode = m_settings.tls ? QStringLiteral("TLS")
                                                : QStringLiteral("plain TCP");
            emit eventReceived(
                QStringLiteral("status"), QString(),
                QStringLiteral("[IRC] Trying %1:%2 using %3…")
                    .arg(address.toString())
                    .arg(m_settings.port)
                    .arg(mode));

            if (m_settings.tls) {
                auto ssl = std::make_unique<QSslSocket>();
                ssl->setPeerVerifyName(m_settings.server);
                ssl->connectToHost(address, m_settings.port);

                if (!ssl->waitForConnected(6000)) {
                    connectionErrors.append(
                        QStringLiteral("%1: TCP %2")
                            .arg(address.toString(), ssl->errorString()));
                    ssl->abort();
                    continue;
                }

                emit eventReceived(QStringLiteral("status"), QString(),
                                   QStringLiteral("[IRC] TCP connected to %1; starting TLS…")
                                       .arg(address.toString()));

                ssl->startClientEncryption();
                if (!ssl->waitForEncrypted(8000)) {
                    connectionErrors.append(
                        QStringLiteral("%1: TLS %2")
                            .arg(address.toString(), ssl->errorString()));
                    ssl->abort();
                    continue;
                }

                emit eventReceived(QStringLiteral("status"), QString(),
                                   QStringLiteral("[IRC] TLS established with %1.")
                                       .arg(address.toString()));
                socket = std::move(ssl);
                break;
            }

            auto tcp = std::make_unique<QTcpSocket>();
            tcp->connectToHost(address, m_settings.port);
            if (!tcp->waitForConnected(6000)) {
                connectionErrors.append(
                    QStringLiteral("%1: %2")
                        .arg(address.toString(), tcp->errorString()));
                tcp->abort();
                continue;
            }

            emit eventReceived(QStringLiteral("status"), QString(),
                               QStringLiteral("[IRC] TCP connected to %1.")
                                   .arg(address.toString()));
            socket = std::move(tcp);
            break;
        }

        if (!socket) {
            if (m_stopRequested) {
                throw std::runtime_error("IRC connection cancelled");
            }
            throw std::runtime_error(
                QStringLiteral("IRC could not connect to any resolved address: %1")
                    .arg(connectionErrors.join(QStringLiteral("; "))).toStdString());
        }

        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[IRC] Registering nickname %1…").arg(m_nickname));

        m_ircv3Capabilities.clear();
        m_isupportTokens.clear();

        if (!m_settings.password.isEmpty()) {
            sendLine(*socket, QStringLiteral("PASS %1").arg(m_settings.password));
        }
        // IRCv3 capability discovery is intentionally non-invasive: list what
        // the server offers, request nothing, then end negotiation so classic
        // registration proceeds normally. Servers without CAP simply ignore or
        // reject these lines; classic 005/ISUPPORT is collected separately.
        sendLine(*socket, QStringLiteral("CAP LS 302"));
        sendLine(*socket, QStringLiteral("NICK %1").arg(m_nickname));
        sendLine(*socket,
                 QStringLiteral("USER %1 0 * :%2")
                     .arg(m_nickname,
                          m_settings.realName.isEmpty()
                              ? appDefaultRealName()
                              : m_settings.realName));
        sendLine(*socket, QStringLiteral("CAP END"));

        bool registered = false;
        while (!registered && !m_stopRequested) {
            const QString line = readLine(*socket, 12000);
            if (line.isEmpty()) {
                continue;
            }

            if (m_settings.debug) {
                emit eventReceived(QStringLiteral("status"), QString(),
                                   QStringLiteral("[IRC <=] %1").arg(line));
            }
            const ParsedLine parsed = parseLine(line);

            if (parsed.command == QStringLiteral("PING")) {
                const QString token = parsed.params.isEmpty() ? QString() : parsed.params.back();
                sendLine(*socket, QStringLiteral("PONG :%1").arg(token));
                continue;
            }
            if (parsed.command == QStringLiteral("001")) {
                registered = true;
                break;
            }
            if (QSet<QString>{QStringLiteral("431"), QStringLiteral("432"),
                              QStringLiteral("433"), QStringLiteral("436"),
                              QStringLiteral("464"), QStringLiteral("465")}
                    .contains(parsed.command)) {
                throw std::runtime_error(
                    QStringLiteral("IRC login failed (%1): %2")
                        .arg(parsed.command,
                             parsed.params.isEmpty() ? QString() : parsed.params.back())
                        .toStdString());
            }
            if (parsed.command == QStringLiteral("ERROR")) {
                throw std::runtime_error(
                    (parsed.params.isEmpty() ? QStringLiteral("IRC server error")
                                             : parsed.params.back()).toStdString());
            }
            processLine(line);
        }

        if (!registered) {
            throw std::runtime_error("IRC registration cancelled");
        }

        emit eventReceived(
            QStringLiteral("status"), QString(),
            QStringLiteral("[online] IRC signed on as %1 via %2:%3%4")
                .arg(m_nickname, m_settings.server)
                .arg(m_settings.port)
                .arg(m_settings.tls ? QStringLiteral(" (TLS)") : QString()));
        emit connected(m_nickname,
                       QStringLiteral("%1:%2").arg(m_settings.server).arg(m_settings.port));
        {
            QStringList watched = m_watchBuddies.values();
            watched.sort(Qt::CaseInsensitive);
            emit buddyListChanged(watched);
            if (!watched.isEmpty())
                emit eventReceived(QStringLiteral("status"), QString(),
                                   QStringLiteral("[IRC] Local buddy/watch list loaded (%1 name(s)); using ISON presence checks.").arg(watched.size()));
        }
        qint64 nextWatchPoll = 0;

        while (!m_stopRequested) {
            for (const Command &command : takeCommands()) {
                switch (command.type) {
                case CommandType::SendIm:
                    sendLine(*socket,
                             QStringLiteral("PRIVMSG %1 :%2").arg(command.a, command.b));
                    emit eventReceived(QStringLiteral("im"), command.a,
                                       QStringLiteral("<%1> %2").arg(m_nickname, command.b));
                    break;

                case CommandType::Join: {
                    const QString room = canonicalChannel(command.a);
                    if (!room.isEmpty()) {
                        m_roomNames[room.toCaseFolded()] = room;
                        sendLine(*socket, QStringLiteral("JOIN %1").arg(room));
                    }
                    break;
                }

                case CommandType::SendRoom:
                    sendLine(*socket,
                             QStringLiteral("PRIVMSG %1 :%2").arg(command.a, command.b));
                    emit eventReceived(QStringLiteral("chat"), command.a,
                                       QStringLiteral("<%1> %2").arg(m_nickname, command.b));
                    break;

                case CommandType::Part:
                    sendLine(*socket, QStringLiteral("PART %1 :Leaving").arg(command.a));
                    m_members.remove(command.a.toCaseFolded());
                    m_roomNames.remove(command.a.toCaseFolded());
                    emit membersChanged(command.a, QStringLiteral("replace"), {});
                    break;

                case CommandType::Raw:
                    if (!command.a.trimmed().isEmpty()) {
                        sendLine(*socket, command.a);
                    }
                    break;

                case CommandType::Nick:
                    if (!command.a.trimmed().isEmpty() && !command.a.contains(QLatin1Char(' '))) {
                        sendLine(*socket, QStringLiteral("NICK %1").arg(command.a.trimmed()));
                    }
                    break;

                case CommandType::WatchAdd: {
                    const QString clean = command.a.trimmed();
                    if (!clean.isEmpty()) {
                        bool exists = false;
                        for (const QString &watched : m_watchBuddies)
                            if (watched.compare(clean, Qt::CaseInsensitive) == 0) { exists = true; break; }
                        if (!exists) m_watchBuddies.insert(clean);
                        nextWatchPoll = 0;
                    }
                    break;
                }

                case CommandType::WatchRemove: {
                    const QString folded = command.a.trimmed().toCaseFolded();
                    for (auto it = m_watchBuddies.begin(); it != m_watchBuddies.end();) {
                        if (it->toCaseFolded() == folded) it = m_watchBuddies.erase(it);
                        else ++it;
                    }
                    if (m_onlineWatchBuddies.remove(folded)) emit buddyPresenceChanged(command.a.trimmed(), false);
                    nextWatchPoll = 0;
                    break;
                }
                }
            }

            // Standard IRC ISON presence polling for local buddy/watch names.
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (!m_watchBuddies.isEmpty() && nowMs >= nextWatchPoll) {
                QStringList names = m_watchBuddies.values();
                names.sort(Qt::CaseInsensitive);
                QString batch;
                for (const QString &name : names) {
                    if (!batch.isEmpty() && batch.size() + name.size() + 1 > 350) {
                        sendLine(*socket, QStringLiteral("ISON %1").arg(batch));
                        batch.clear();
                    }
                    if (!batch.isEmpty()) batch += QLatin1Char(' ');
                    batch += name;
                }
                if (!batch.isEmpty()) sendLine(*socket, QStringLiteral("ISON %1").arg(batch));
                nextWatchPoll = nowMs + 20000;
            }

            if (socket->bytesAvailable() <= 0 && !socket->waitForReadyRead(75)) {
                if (socket->state() != QAbstractSocket::ConnectedState) {
                    throw std::runtime_error("IRC server closed the connection");
                }
                continue;
            }

            while (socket->canReadLine()) {
                QString line = QString::fromUtf8(socket->readLine());
                while (line.endsWith(QLatin1Char('\n')) || line.endsWith(QLatin1Char('\r'))) {
                    line.chop(1);
                }
                const ParsedLine parsed = parseLine(line);
                if (parsed.command == QStringLiteral("PING")) {
                    const QString token = parsed.params.isEmpty() ? QString() : parsed.params.back();
                    sendLine(*socket, QStringLiteral("PONG :%1").arg(token));
                } else {
                    processLine(line);
                }
            }
        }

        try {
            sendLine(*socket, QStringLiteral("QUIT :%1 signing off").arg(appDisplayName()));
        } catch (...) {
        }
        // disconnectFromHost() can complete synchronously. Avoid calling
        // waitForDisconnected() once Qt has already transitioned the socket
        // to UnconnectedState, which otherwise prints a warning to stderr.
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->disconnectFromHost();
            if (socket->state() != QAbstractSocket::UnconnectedState) {
                socket->waitForDisconnected(500);
            }
        }
    } catch (const std::exception &e) {
        disconnectReason = QString::fromUtf8(e.what());
        emit backendError(QStringLiteral("IRC connection"), disconnectReason);
    }

    emit eventReceived(QStringLiteral("status"), QString(), QStringLiteral("[offline] IRC signed off"));
    emit disconnected(disconnectReason);
}

