#include "telnetbackend.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QHostInfo>
#include <QMutexLocker>
#include <QTcpSocket>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <utility>

namespace {
constexpr quint8 IAC  = 255;
constexpr quint8 DONT = 254;
constexpr quint8 DO   = 253;
constexpr quint8 WONT = 252;
constexpr quint8 WILL = 251;
constexpr quint8 SB   = 250;
constexpr quint8 SE   = 240;

constexpr quint8 OPT_BINARY = 0;
constexpr quint8 OPT_ECHO = 1;
constexpr quint8 OPT_SGA = 3;
constexpr quint8 OPT_TTYPE = 24;
constexpr quint8 OPT_NAWS = 31;
constexpr quint8 OPT_LINEMODE = 34;

constexpr quint8 TTYPE_IS = 0;
constexpr quint8 TTYPE_SEND = 1;

QByteArray escapeIac(QByteArray data)
{
    data.replace(QByteArray(1, static_cast<char>(IAC)),
                 QByteArray(2, static_cast<char>(IAC)));
    return data;
}
} // namespace

TelnetBackend::TelnetBackend(ConnectionSettings settings, QObject *parent)
    : ChatBackend(std::move(settings), parent)
{
}

TelnetBackend::~TelnetBackend()
{
    m_stopRequested = true;
    if (m_thread && m_thread != QThread::currentThread()) {
        m_thread->wait();
    }
}

void TelnetBackend::setConnectionSettings(const ConnectionSettings &settings)
{
    ChatBackend::setConnectionSettings(settings);
}

void TelnetBackend::start()
{
    if (m_thread) {
        return;
    }
    m_stopRequested = false;
    m_telnetState = TelnetState::Data;
    m_subnegData.clear();
    m_serverEcho = false;
    m_nawsEnabled = false;
    m_thread = QThread::create([this] { run(); });
    connect(m_thread, &QThread::finished, m_thread, &QObject::deleteLater);
    connect(m_thread, &QThread::finished, this,
            [this] { m_thread = nullptr; }, Qt::QueuedConnection);
    m_thread->start();
}

void TelnetBackend::stop()
{
    m_stopRequested = true;
    QThread *thread = m_thread;
    if (thread && thread != QThread::currentThread()) {
        thread->wait(12000);
    }
}

void TelnetBackend::enqueue(Command command)
{
    QMutexLocker locker(&m_commandMutex);
    m_commands.enqueue(std::move(command));
}

QList<TelnetBackend::Command> TelnetBackend::takeCommands()
{
    QList<Command> result;
    QMutexLocker locker(&m_commandMutex);
    while (!m_commands.isEmpty()) {
        result.push_back(m_commands.dequeue());
    }
    return result;
}

void TelnetBackend::sendPrivateMessage(const QString &, const QString &message)
{
    enqueue({CommandType::SendLine, message, {}});
}

void TelnetBackend::joinRoom(const QString &, bool)
{
    emit backendError(QStringLiteral("Telnet"),
                      QStringLiteral("Telnet sessions do not use rooms/channels."));
}

void TelnetBackend::sendRoomMessage(const QString &, const QString &message)
{
    enqueue({CommandType::SendLine, message, {}});
}

void TelnetBackend::leaveRoom(const QString &)
{
    stop();
}

void TelnetBackend::sendRaw(const QString &line, const QString &, const QString &)
{
    enqueue({CommandType::RawLine, line, {}});
}

void TelnetBackend::setTerminalSize(int columns, int rows)
{
    const int newColumns = std::clamp(columns, 20, 65535);
    const int newRows = std::clamp(rows, 5, 65535);
    const bool changed = newColumns != m_columns.load() || newRows != m_rows.load();
    m_columns = newColumns;
    m_rows = newRows;
    // Once the peer has negotiated NAWS, push live viewport changes instead of
    // waiting for a reconnect. This is important on Termux when font size, the
    // soft keyboard, or device rotation changes the number of visible cells.
    if (changed && m_thread) enqueue({CommandType::WindowSize, {}, {}});
}

void TelnetBackend::sendTerminalInput(const QByteArray &bytes)
{
    if (!bytes.isEmpty()) enqueue({CommandType::RawBytes, {}, bytes});
}

void TelnetBackend::writeAll(QTcpSocket &socket, const QByteArray &data)
{
    qint64 offset = 0;
    while (offset < data.size()) {
        const qint64 written = socket.write(data.constData() + offset, data.size() - offset);
        if (written < 0) {
            throw std::runtime_error(socket.errorString().toStdString());
        }
        offset += written;
        if (!socket.waitForBytesWritten(3000) && socket.bytesToWrite() > 0) {
            throw std::runtime_error(socket.errorString().toStdString());
        }
    }
}

void TelnetBackend::sendLine(QTcpSocket &socket, const QString &text)
{
    QByteArray bytes = escapeIac(text.toUtf8());
    bytes.append("\r\n");
    writeAll(socket, bytes);

    // If the remote side is not doing Telnet ECHO, provide local echo so line-mode
    // MUD/BBS sessions remain readable.
    if (!m_serverEcho.load()) {
        emit eventReceived(QStringLiteral("terminal"), m_settings.server,
                           QStringLiteral("> %1\n").arg(text));
    }
}

void TelnetBackend::sendIac(QTcpSocket &socket, quint8 command, quint8 option)
{
    QByteArray out;
    out.append(static_cast<char>(IAC));
    out.append(static_cast<char>(command));
    out.append(static_cast<char>(option));
    writeAll(socket, out);
}

void TelnetBackend::sendSubneg(QTcpSocket &socket, quint8 option, const QByteArray &payload)
{
    QByteArray out;
    out.append(static_cast<char>(IAC));
    out.append(static_cast<char>(SB));
    out.append(static_cast<char>(option));
    out.append(escapeIac(payload));
    out.append(static_cast<char>(IAC));
    out.append(static_cast<char>(SE));
    writeAll(socket, out);
}

void TelnetBackend::sendWindowSize(QTcpSocket &socket)
{
    const quint16 cols = static_cast<quint16>(m_columns.load());
    const quint16 rows = static_cast<quint16>(m_rows.load());
    QByteArray payload;
    payload.append(static_cast<char>((cols >> 8) & 0xff));
    payload.append(static_cast<char>(cols & 0xff));
    payload.append(static_cast<char>((rows >> 8) & 0xff));
    payload.append(static_cast<char>(rows & 0xff));
    sendSubneg(socket, OPT_NAWS, payload);
}

void TelnetBackend::handleNegotiation(QTcpSocket &socket, quint8 command, quint8 option)
{
    if (m_settings.debug) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[TELNET] IAC %1 option %2")
                               .arg(command).arg(option));
    }

    if (command == WILL) {
        switch (option) {
        case OPT_BINARY:
        case OPT_SGA:
            sendIac(socket, DO, option);
            break;
        case OPT_ECHO:
            m_serverEcho = true;
            sendIac(socket, DO, option);
            break;
        default:
            sendIac(socket, DONT, option);
            break;
        }
        return;
    }

    if (command == WONT) {
        if (option == OPT_ECHO) {
            m_serverEcho = false;
        }
        sendIac(socket, DONT, option);
        return;
    }

    if (command == DO) {
        switch (option) {
        case OPT_BINARY:
        case OPT_SGA:
            sendIac(socket, WILL, option);
            break;
        case OPT_TTYPE:
            sendIac(socket, WILL, option);
            break;
        case OPT_NAWS:
            sendIac(socket, WILL, option);
            m_nawsEnabled = true;
            sendWindowSize(socket);
            break;
        case OPT_ECHO:
        case OPT_LINEMODE:
        default:
            sendIac(socket, WONT, option);
            break;
        }
        return;
    }

    if (command == DONT) {
        if (option == OPT_NAWS) m_nawsEnabled = false;
        sendIac(socket, WONT, option);
    }
}

void TelnetBackend::handleSubnegotiation(QTcpSocket &socket,
                                         quint8 option,
                                         const QByteArray &payload)
{
    if (option == OPT_TTYPE && !payload.isEmpty()
        && static_cast<quint8>(payload.front()) == TTYPE_SEND) {
        QByteArray response;
        response.append(static_cast<char>(TTYPE_IS));
        QString terminal = m_settings.telnetTerminalType.trimmed();
        if (terminal.isEmpty()) {
            terminal = QStringLiteral("ANSI");
        }
        response.append(terminal.toLatin1());
        sendSubneg(socket, OPT_TTYPE, response);
    }
}

QString TelnetBackend::decodeTerminalText(const QByteArray &bytes) const
{
    // Classic BBS output is commonly CP437. Preserve C0 controls and ANSI ESC
    // sequences verbatim while translating the high half into Unicode box/block
    // drawing characters for Qt/ncurses rendering.
    static const ushort cp437[256] = {0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007, 0x0008, 0x0009, 0x000a, 0x000b, 0x000c, 0x000d, 0x000e, 0x000f, 0x0010, 0x0011, 0x0012, 0x0013, 0x0014, 0x0015, 0x0016, 0x0017, 0x0018, 0x0019, 0x001a, 0x001b, 0x001c, 0x001d, 0x001e, 0x001f, 0x0020, 0x0021, 0x0022, 0x0023, 0x0024, 0x0025, 0x0026, 0x0027, 0x0028, 0x0029, 0x002a, 0x002b, 0x002c, 0x002d, 0x002e, 0x002f, 0x0030, 0x0031, 0x0032, 0x0033, 0x0034, 0x0035, 0x0036, 0x0037, 0x0038, 0x0039, 0x003a, 0x003b, 0x003c, 0x003d, 0x003e, 0x003f, 0x0040, 0x0041, 0x0042, 0x0043, 0x0044, 0x0045, 0x0046, 0x0047, 0x0048, 0x0049, 0x004a, 0x004b, 0x004c, 0x004d, 0x004e, 0x004f, 0x0050, 0x0051, 0x0052, 0x0053, 0x0054, 0x0055, 0x0056, 0x0057, 0x0058, 0x0059, 0x005a, 0x005b, 0x005c, 0x005d, 0x005e, 0x005f, 0x0060, 0x0061, 0x0062, 0x0063, 0x0064, 0x0065, 0x0066, 0x0067, 0x0068, 0x0069, 0x006a, 0x006b, 0x006c, 0x006d, 0x006e, 0x006f, 0x0070, 0x0071, 0x0072, 0x0073, 0x0074, 0x0075, 0x0076, 0x0077, 0x0078, 0x0079, 0x007a, 0x007b, 0x007c, 0x007d, 0x007e, 0x007f, 0x00c7, 0x00fc, 0x00e9, 0x00e2, 0x00e4, 0x00e0, 0x00e5, 0x00e7, 0x00ea, 0x00eb, 0x00e8, 0x00ef, 0x00ee, 0x00ec, 0x00c4, 0x00c5, 0x00c9, 0x00e6, 0x00c6, 0x00f4, 0x00f6, 0x00f2, 0x00fb, 0x00f9, 0x00ff, 0x00d6, 0x00dc, 0x00a2, 0x00a3, 0x00a5, 0x20a7, 0x0192, 0x00e1, 0x00ed, 0x00f3, 0x00fa, 0x00f1, 0x00d1, 0x00aa, 0x00ba, 0x00bf, 0x2310, 0x00ac, 0x00bd, 0x00bc, 0x00a1, 0x00ab, 0x00bb, 0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255d, 0x255c, 0x255b, 0x2510, 0x2514, 0x2534, 0x252c, 0x251c, 0x2500, 0x253c, 0x255e, 0x255f, 0x255a, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256c, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256b, 0x256a, 0x2518, 0x250c, 0x2588, 0x2584, 0x258c, 0x2590, 0x2580, 0x03b1, 0x00df, 0x0393, 0x03c0, 0x03a3, 0x03c3, 0x00b5, 0x03c4, 0x03a6, 0x0398, 0x03a9, 0x03b4, 0x221e, 0x03c6, 0x03b5, 0x2229, 0x2261, 0x00b1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00f7, 0x2248, 0x00b0, 0x2219, 0x00b7, 0x221a, 0x207f, 0x00b2, 0x25a0, 0x00a0};
    QString out;
    out.reserve(bytes.size());
    for (unsigned char b : bytes) out.append(QChar(cp437[b]));
    return out;
}

void TelnetBackend::processBytes(QTcpSocket &socket, const QByteArray &bytes)
{
    QByteArray display;

    for (const char raw : bytes) {
        const quint8 b = static_cast<quint8>(raw);

        switch (m_telnetState) {
        case TelnetState::Data:
            if (b == IAC) {
                m_telnetState = TelnetState::Iac;
            } else {
                display.append(raw);
            }
            break;

        case TelnetState::Iac:
            if (b == IAC) {
                display.append(static_cast<char>(IAC));
                m_telnetState = TelnetState::Data;
            } else if (b == DO || b == DONT || b == WILL || b == WONT) {
                m_pendingCommand = b;
                m_telnetState = TelnetState::Negotiation;
            } else if (b == SB) {
                m_telnetState = TelnetState::SubnegOption;
            } else {
                m_telnetState = TelnetState::Data;
            }
            break;

        case TelnetState::Negotiation:
            handleNegotiation(socket, m_pendingCommand, b);
            m_telnetState = TelnetState::Data;
            break;

        case TelnetState::SubnegOption:
            m_subnegOption = b;
            m_subnegData.clear();
            m_telnetState = TelnetState::SubnegData;
            break;

        case TelnetState::SubnegData:
            if (b == IAC) {
                m_telnetState = TelnetState::SubnegIac;
            } else {
                m_subnegData.append(raw);
            }
            break;

        case TelnetState::SubnegIac:
            if (b == SE) {
                handleSubnegotiation(socket, m_subnegOption, m_subnegData);
                m_subnegData.clear();
                m_telnetState = TelnetState::Data;
            } else if (b == IAC) {
                m_subnegData.append(static_cast<char>(IAC));
                m_telnetState = TelnetState::SubnegData;
            } else {
                m_telnetState = TelnetState::SubnegData;
            }
            break;
        }
    }

    if (!display.isEmpty()) {
        const QString text = decodeTerminalText(display);
        if (!text.isEmpty()) {
            emit eventReceived(QStringLiteral("terminal"), m_settings.server, text);
        }
    }
}

void TelnetBackend::run()
{
    QString disconnectReason = QStringLiteral("signed off");

    try {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[TELNET] Resolving %1…")
                               .arg(m_settings.server));

        QList<QHostAddress> addresses;
        QHostAddress literal;
        if (literal.setAddress(m_settings.server)) {
            addresses.append(literal);
        } else {
            const QHostInfo info = QHostInfo::fromName(m_settings.server);
            if (info.error() != QHostInfo::NoError) {
                throw std::runtime_error(
                    QStringLiteral("Telnet DNS lookup failed for %1: %2")
                        .arg(m_settings.server, info.errorString()).toStdString());
            }
            addresses = info.addresses();
        }

        if (addresses.isEmpty()) {
            throw std::runtime_error(
                QStringLiteral("Telnet DNS lookup returned no addresses for %1")
                    .arg(m_settings.server).toStdString());
        }

        std::stable_sort(addresses.begin(), addresses.end(),
                         [](const QHostAddress &a, const QHostAddress &b) {
            const bool aV4 = a.protocol() == QAbstractSocket::IPv4Protocol;
            const bool bV4 = b.protocol() == QAbstractSocket::IPv4Protocol;
            return aV4 && !bV4;
        });

        std::unique_ptr<QTcpSocket> socket;
        QStringList errors;
        for (const QHostAddress &address : addresses) {
            if (m_stopRequested) {
                break;
            }

            emit eventReceived(QStringLiteral("status"), QString(),
                               QStringLiteral("[TELNET] Trying %1:%2…")
                                   .arg(address.toString()).arg(m_settings.port));

            auto candidate = std::make_unique<QTcpSocket>();
            candidate->connectToHost(address, m_settings.port);
            if (!candidate->waitForConnected(6000)) {
                errors.append(QStringLiteral("%1: %2")
                                  .arg(address.toString(), candidate->errorString()));
                candidate->abort();
                continue;
            }
            socket = std::move(candidate);
            break;
        }

        if (!socket) {
            if (m_stopRequested) {
                throw std::runtime_error("Telnet connection cancelled");
            }
            throw std::runtime_error(
                QStringLiteral("Telnet could not connect to any resolved address: %1")
                    .arg(errors.join(QStringLiteral("; "))).toStdString());
        }

        const QString identity = m_settings.username.trimmed().isEmpty()
            ? m_settings.server
            : m_settings.username.trimmed();
        const QString endpoint = QStringLiteral("%1:%2")
            .arg(m_settings.server).arg(m_settings.port);

        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[online] Telnet connected to %1")
                               .arg(endpoint));
        emit connected(identity, endpoint);

        while (!m_stopRequested) {
            for (const Command &command : takeCommands()) {
                switch (command.type) {
                case CommandType::SendLine:
                    sendLine(*socket, command.text);
                    break;
                case CommandType::RawLine:
                    sendLine(*socket, command.text);
                    break;
                case CommandType::RawBytes: {
                    const QByteArray raw = escapeIac(command.bytes);
                    writeAll(*socket, raw);
                    // Raw BBS input is mirrored by the UI, not injected into the
                    // terminal model here.  That avoids duplicate characters and,
                    // critically, prevents password bytes from being displayed when
                    // the BBS intentionally suppresses application-level echo.
                    break;
                }
                case CommandType::WindowSize:
                    if (m_nawsEnabled.load()) sendWindowSize(*socket);
                    break;
                }
            }

            if (socket->bytesAvailable() <= 0 && !socket->waitForReadyRead(50)) {
                if (socket->state() != QAbstractSocket::ConnectedState) {
                    throw std::runtime_error("Telnet server closed the connection");
                }
                continue;
            }

            const QByteArray incoming = socket->readAll();
            if (!incoming.isEmpty()) {
                processBytes(*socket, incoming);
            }
        }

        // disconnectFromHost() may complete synchronously.  Calling
        // waitForDisconnected() after the socket has already reached
        // UnconnectedState makes Qt print a warning directly to stderr; in
        // ncurses mode that external write corrupts the terminal display.
        // Only wait while there is actually a disconnect transition left.
        if (socket->state() != QAbstractSocket::UnconnectedState) {
            socket->disconnectFromHost();
            if (socket->state() != QAbstractSocket::UnconnectedState) {
                socket->waitForDisconnected(500);
            }
        }
    } catch (const std::exception &e) {
        disconnectReason = QString::fromUtf8(e.what());
        if (!m_stopRequested || disconnectReason != QStringLiteral("Telnet connection cancelled")) {
            emit backendError(QStringLiteral("Telnet connection"), disconnectReason);
        }
    }

    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[offline] Telnet signed off"));
    emit disconnected(disconnectReason);
}
