#include "oscarbackend.h"
#include "appbranding.h"

#include <QElapsedTimer>
#include <QMutexLocker>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <utility>

using namespace Oscar;

OscarBackend::OscarBackend(ConnectionSettings settings, QObject *parent)
    : ChatBackend(std::move(settings), parent)
{
}

OscarBackend::~OscarBackend()
{
    m_stopRequested = true;
    if (m_thread && m_thread != QThread::currentThread()) {
        m_thread->wait();
    }
}

void OscarBackend::start()
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

void OscarBackend::stop()
{
    m_stopRequested = true;
    QThread *thread = m_thread;
    if (thread && thread != QThread::currentThread()) {
        thread->wait(15000);
    }
}

void OscarBackend::enqueue(Command command)
{
    QMutexLocker locker(&m_commandMutex);
    m_commands.enqueue(std::move(command));
}

QList<OscarBackend::Command> OscarBackend::takeCommands()
{
    QList<Command> result;
    QMutexLocker locker(&m_commandMutex);
    while (!m_commands.isEmpty()) {
        result.push_back(m_commands.dequeue());
    }
    return result;
}

void OscarBackend::sendPrivateMessage(const QString &target, const QString &message)
{
    enqueue({CommandType::SendIm, target, message, {}, false});
}

void OscarBackend::joinRoom(const QString &room, bool privateRoom)
{
    enqueue({CommandType::JoinRoom, room, {}, {}, privateRoom});
}

void OscarBackend::sendRoomMessage(const QString &room, const QString &message)
{
    enqueue({CommandType::SendRoom, room, message, {}, false});
}

void OscarBackend::leaveRoom(const QString &room)
{
    enqueue({CommandType::LeaveRoom, room, {}, {}, false});
}

void OscarBackend::changePassword(const QString &currentPassword,
                                  const QString &newPassword)
{
    enqueue({CommandType::Password, currentPassword, newPassword, {}, false});
}

void OscarBackend::sendRaw(const QString &family,
                           const QString &subtype,
                           const QString &hexBody)
{
    enqueue({CommandType::Raw, family, subtype, hexBody, false});
}

void OscarBackend::addBuddy(const QString &name)
{
    enqueue({CommandType::AddBuddy, name.trimmed(), {}, {}, false});
}

void OscarBackend::removeBuddy(const QString &name)
{
    enqueue({CommandType::RemoveBuddy, name.trimmed(), {}, {}, false});
}

void OscarBackend::setAwayMessage(const QString &message)
{
    enqueue({CommandType::SetAway, message.trimmed(), {}, {}, false, 0});
}

void OscarBackend::setAfkMessage(const QString &message)
{
    enqueue({CommandType::SetAfk, message.trimmed(), {}, {}, true, 0});
}

void OscarBackend::setIdleSeconds(quint32 seconds)
{
    enqueue({CommandType::SetIdle, {}, {}, {}, false, seconds});
}

void OscarBackend::setBack()
{
    enqueue({CommandType::SetBack, {}, {}, {}, false, 0});
}

void OscarBackend::requestClientVersion(const QString &target)
{
    const QString clean = target.trimmed();
    if (clean.isEmpty()) return;
    enqueue({CommandType::SendIm, clean, QStringLiteral("[[WHVER:Q]]"), {}, true, 0});
}

void OscarBackend::protocolLog(const QString &text)
{
    emit eventReceived(QStringLiteral("status"), QString(), text);
}

[[noreturn]] void OscarBackend::fail(const QString &message) const
{
    throw ProtocolError(message);
}

QPair<QString, quint16> OscarBackend::redirectEndpoint(const QString &advertised,
                                                       quint16 fallbackPort) const
{
    auto endpoint = parseEndpoint(advertised, fallbackPort);
    if (!m_settings.redirectHost.trimmed().isEmpty()) {
        endpoint.first = m_settings.redirectHost.trimmed();
    }
    if (m_settings.redirectPort != 0) {
        endpoint.second = m_settings.redirectPort;
    }
    return endpoint;
}

void OscarBackend::expectGreeting(FlapConnection &connection)
{
    const FlapFrame frame = connection.receiveFlap();
    if (frame.channel != FLAP_SIGNON) {
        fail(QStringLiteral("%1: expected FLAP signon greeting, got channel %2")
                 .arg(connection.label())
                 .arg(frame.channel));
    }
    if (m_settings.debug && frame.payload.size() >= 4) {
        protocolLog(QStringLiteral("[debug] %1: server FLAP version %2")
                        .arg(connection.label())
                        .arg(readU32(frame.payload, 0)));
    }
}

QPair<QString, QByteArray> OscarBackend::authenticate()
{
    FlapConnection auth(m_settings.server,
                        m_settings.port,
                        QStringLiteral("auth"),
                        m_settings.debug,
                        [this](const QString &s) { protocolLog(s); });
    auth.connectToHost();
    expectGreeting(auth);
    auth.sendSignon();

    auth.sendSnac(FAM_BUCP,
                  BUCP_CHALLENGE_REQUEST,
                  tlv(TLV_SCREEN_NAME, m_settings.username));

    QByteArray authKey;
    while (true) {
        const Snac snac = auth.receiveSnac();
        if (snac.family == FAM_BUCP && snac.subtype == BUCP_CHALLENGE_RESPONSE) {
            if (snac.body.size() < 2) {
                fail(QStringLiteral("truncated BUCP challenge response"));
            }
            const quint16 length = readU16(snac.body, 0);
            if (snac.body.size() < 2 + length) {
                fail(QStringLiteral("truncated BUCP auth key"));
            }
            authKey = snac.body.mid(2, length);
            break;
        }

        if (snac.family == FAM_BUCP && snac.subtype == BUCP_LOGIN_RESPONSE) {
            qsizetype offset = 0;
            const auto items = parseTlvs(snac.body, offset);
            const QByteArray error = firstTlv(items, TLV_LOGIN_ERROR);
            if (error.size() >= 2) {
                fail(QStringLiteral("login challenge rejected, OSCAR error 0x%1")
                         .arg(readU16(error, 0), 4, 16, QLatin1Char('0')));
            }
        }
    }

    QByteArray body;
    body += tlv(TLV_SCREEN_NAME, m_settings.username);
    body += tlv(TLV_PASSWORD_HASH, passwordHash(m_settings.password, authKey));
    body += tlv(TLV_CLIENT_ID, QStringLiteral("%1 C++ OSCAR client").arg(appDisplayName()));
    body += tlv(TLV_MULTI_CONN, QByteArray(1, char(1)));
    auth.sendSnac(FAM_BUCP, BUCP_LOGIN_REQUEST, body);

    while (true) {
        const Snac snac = auth.receiveSnac();
        if (snac.family != FAM_BUCP || snac.subtype != BUCP_LOGIN_RESPONSE) {
            continue;
        }

        qsizetype offset = 0;
        const auto items = parseTlvs(snac.body, offset);
        const QByteArray error = firstTlv(items, TLV_LOGIN_ERROR);
        if (error.size() >= 2) {
            fail(QStringLiteral("login failed, OSCAR error 0x%1")
                     .arg(readU16(error, 0), 4, 16, QLatin1Char('0')));
        }

        const QByteArray hostRaw = firstTlv(items, TLV_RECONNECT_HOST);
        const QByteArray cookie = firstTlv(items, TLV_AUTH_COOKIE);
        if (hostRaw.isEmpty() || cookie.isEmpty()) {
            fail(QStringLiteral("login succeeded but BOS host/cookie are missing"));
        }

        auth.close();
        return {QString::fromUtf8(hostRaw), cookie};
    }
}

void OscarBackend::bootstrapService(FlapConnection &connection,
                                    const QByteArray &cookie,
                                    bool addIcbmParams)
{
    connection.connectToHost();
    expectGreeting(connection);
    connection.sendSignon({Tlv{TLV_AUTH_COOKIE, cookie}});

    while (true) {
        const Snac snac = connection.receiveSnac();
        if (snac.family == FAM_OSERVICE && snac.subtype == OS_HOST_ONLINE) {
            if (snac.body.size() % 2 != 0) {
                fail(QStringLiteral("odd HostOnline family list"));
            }
            connection.families.clear();
            for (qsizetype i = 0; i < snac.body.size(); i += 2) {
                connection.families.push_back(readU16(snac.body, i));
            }
            break;
        }
    }

    QByteArray versions;
    for (const quint16 family : connection.families) {
        appendU16(versions, family);
        appendU16(versions, 1);
    }
    const quint32 versionsRequest = connection.sendSnac(
        FAM_OSERVICE, OS_CLIENT_VERSIONS, versions);

    while (true) {
        const Snac snac = connection.receiveSnac();
        if (snac.family == FAM_OSERVICE
            && snac.subtype == OS_HOST_VERSIONS
            && snac.requestId == versionsRequest) {
            break;
        }
    }

    const quint32 rateRequest = connection.sendSnac(FAM_OSERVICE, OS_RATE_QUERY);
    QList<quint16> rateIds;
    while (true) {
        const Snac snac = connection.receiveSnac();
        if (snac.family == FAM_OSERVICE
            && snac.subtype == OS_RATE_REPLY
            && snac.requestId == rateRequest) {
            if (snac.body.size() >= 2) {
                const quint16 count = readU16(snac.body, 0);
                qsizetype offset = 2;
                for (quint16 i = 0; i < count; ++i) {
                    if (offset + 30 > snac.body.size()) {
                        break;
                    }
                    rateIds.push_back(readU16(snac.body, offset));
                    offset += 30;
                }
            }
            break;
        }
    }

    if (!rateIds.isEmpty()) {
        QByteArray ack;
        for (const quint16 rateId : rateIds) {
            appendU16(ack, rateId);
        }
        connection.sendSnac(FAM_OSERVICE, OS_RATE_ACK, ack);
    }

    if (addIcbmParams && connection.families.contains(FAM_ICBM)) {
        QByteArray params;
        appendU16(params, ICBM_CHANNEL_IM);
        appendU32(params, 3);
        appendU16(params, 8000);
        appendU16(params, 999);
        appendU16(params, 999);
        appendU32(params, 0);
        connection.sendSnac(FAM_ICBM, ICBM_ADD_PARAMS, params);
    }

    QByteArray online;
    for (const quint16 family : connection.families) {
        appendU16(online, family);
        appendU16(online, 1);
        appendU16(online, 0x0110);
        appendU16(online, 0x0001);
    }
    connection.sendSnac(FAM_OSERVICE, OS_CLIENT_ONLINE, online);
}

Snac OscarBackend::request(FlapConnection &connection,
                           quint16 family,
                           quint16 subtype,
                           const QByteArray &body,
                           int timeoutMs)
{
    const quint32 requestId = connection.sendSnac(family, subtype, body);
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        const int remaining = std::max(1, timeoutMs - static_cast<int>(timer.elapsed()));
        const Snac snac = connection.receiveSnac(remaining);
        if (snac.requestId == requestId) {
            return snac;
        }
        dispatchSnac(connection, snac);
    }

    fail(QStringLiteral("%1: timed out waiting for reply to %2/%3")
             .arg(connection.label())
             .arg(family, 4, 16, QLatin1Char('0'))
             .arg(subtype, 4, 16, QLatin1Char('0')));
}

OscarBackend::ServiceRedirect OscarBackend::requestService(
    quint16 family,
    const QList<Tlv> &extraTlvs)
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    QByteArray body;
    appendU16(body, family);
    for (const auto &item : extraTlvs) {
        body += tlv(item.type, item.value);
    }

    const Snac snac = request(*m_bos, FAM_OSERVICE, OS_SERVICE_REQUEST, body);
    if (snac.family == FAM_OSERVICE && snac.subtype == 0x0001) {
        const int code = snac.body.size() >= 2 ? readU16(snac.body, 0) : -1;
        fail(QStringLiteral("service request failed, OSCAR error 0x%1")
                 .arg(code, 4, 16, QLatin1Char('0')));
    }
    if (snac.family != FAM_OSERVICE || snac.subtype != OS_SERVICE_RESPONSE) {
        fail(QStringLiteral("unexpected service reply %1/%2")
                 .arg(snac.family, 4, 16, QLatin1Char('0'))
                 .arg(snac.subtype, 4, 16, QLatin1Char('0')));
    }

    qsizetype offset = 0;
    const auto items = parseTlvs(snac.body, offset);
    const QByteArray hostRaw = firstTlv(items, TLV_RECONNECT_HOST);
    const QByteArray cookie = firstTlv(items, TLV_AUTH_COOKIE);
    if (hostRaw.isEmpty() || cookie.isEmpty()) {
        fail(QStringLiteral("service redirect missing host or cookie"));
    }

    const auto endpoint = redirectEndpoint(QString::fromUtf8(hostRaw), m_settings.port);
    return {endpoint.first, endpoint.second, cookie};
}

FlapConnection &OscarBackend::ensureChatNav()
{
    if (m_chatNav && m_chatNav->isConnected()) {
        return *m_chatNav;
    }

    const ServiceRedirect redirect = requestService(FAM_CHATNAV);
    m_chatNav = std::make_unique<FlapConnection>(
        redirect.host,
        redirect.port,
        QStringLiteral("ChatNav"),
        m_settings.debug,
        [this](const QString &s) { protocolLog(s); });
    bootstrapService(*m_chatNav, redirect.cookie);
    return *m_chatNav;
}

void OscarBackend::doSendIm(const QString &recipient, const QString &message, bool echo)
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    const QByteArray name = recipient.toUtf8();
    if (name.size() > 255) {
        fail(QStringLiteral("recipient screen name is too long"));
    }

    QByteArray body;
    const quint64 cookie = (static_cast<quint64>(QRandomGenerator::global()->generate64()));
    appendU64(body, cookie);
    appendU16(body, ICBM_CHANNEL_IM);
    body += lp8(name);
    body += tlv(ICBM_TLV_IM_DATA, marshalIcbmFragments(message));
    body += tlv(ICBM_TLV_ACK, QByteArray());
    m_bos->sendSnac(FAM_ICBM, ICBM_MSG_TO_HOST, body);
    if (echo) {
        emit eventReceived(QStringLiteral("im"), recipient,
                           QStringLiteral("<%1> %2").arg(m_settings.username, message));
    }
}

void OscarBackend::doJoinRoom(const QString &name, bool privateRoom)
{
    FlapConnection &nav = ensureChatNav();
    const quint16 exchange = privateRoom ? PRIVATE_EXCHANGE : PUBLIC_EXCHANGE;

    QByteArray roomRequest;
    appendU16(roomRequest, exchange);
    roomRequest += lp8(QStringLiteral("create"));
    appendU16(roomRequest, 0);
    appendU8(roomRequest, 2);
    roomRequest += tlvBlock({Tlv{CHAT_ROOM_TLV_NAME, name.toUtf8()}});

    const Snac reply = request(nav, FAM_CHATNAV, CHATNAV_CREATE_ROOM, roomRequest);
    if (reply.family == FAM_CHATNAV && reply.subtype == 0x0001) {
        const int code = reply.body.size() >= 2 ? readU16(reply.body, 0) : -1;
        fail(QStringLiteral("ChatNav could not resolve '%1', OSCAR error 0x%2")
                 .arg(name)
                 .arg(code, 4, 16, QLatin1Char('0')));
    }
    if (reply.family != FAM_CHATNAV || reply.subtype != CHATNAV_NAV_INFO) {
        fail(QStringLiteral("unexpected ChatNav reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }

    qsizetype offset = 0;
    const auto items = parseTlvs(reply.body, offset);
    const QByteArray roomBlob = firstTlv(items, 0x0004);
    if (roomBlob.isEmpty()) {
        fail(QStringLiteral("ChatNav response contains no room-info TLV"));
    }

    qsizetype roomOffset = 0;
    const RoomInfo room = parseRoomInfo(roomBlob, roomOffset, true);

    QByteArray shortRoom;
    appendU16(shortRoom, room.exchange);
    shortRoom += lp8(room.cookie);
    appendU16(shortRoom, room.instance);

    const ServiceRedirect redirect = requestService(
        FAM_CHAT,
        {Tlv{0x0001, shortRoom}});

    auto connection = std::make_unique<FlapConnection>(
        redirect.host,
        redirect.port,
        QStringLiteral("chat:%1").arg(room.name()),
        m_settings.debug,
        [this](const QString &s) { protocolLog(s); });
    bootstrapService(*connection, redirect.cookie);

    const QString key = room.name().toCaseFolded();
    auto session = std::make_shared<ChatSession>();
    session->room = room;
    session->connection = std::move(connection);
    m_chats.insert(key, std::move(session));

    emit eventReceived(
        QStringLiteral("chat"),
        room.name(),
        QStringLiteral("*** joined '%1' (exchange=%2, cookie=%3)")
            .arg(room.name())
            .arg(room.exchange)
            .arg(room.cookie));
}

void OscarBackend::doSendRoom(const QString &name, const QString &message)
{
    const QString key = name.toCaseFolded();
    auto it = m_chats.find(key);
    if (it == m_chats.end() || !it.value()) {
        fail(QStringLiteral("not joined to chat '%1'").arg(name));
    }

    const QString rendered = QStringLiteral(
        "<HTML><BODY BGCOLOR=\"#ffffff\"><FONT LANG=\"0\">%1</FONT></BODY></HTML>")
                                 .arg(message.toHtmlEscaped());
    const QByteArray encoding = message.toLatin1() == message.toUtf8()
                              ? QByteArray("us-ascii")
                              : QByteArray("utf-8");

    QByteArray messageInfo;
    messageInfo += tlv(CHAT_MSG_TLV_ENCODING, encoding);
    messageInfo += tlv(CHAT_MSG_TLV_LANG, QByteArray("en"));
    messageInfo += tlv(CHAT_MSG_TLV_TEXT, rendered.toUtf8());

    QByteArray body;
    appendU64(body, QRandomGenerator::global()->generate64());
    appendU16(body, 3);
    body += tlv(CHAT_TLV_PUBLIC, QByteArray());
    body += tlv(CHAT_TLV_REFLECT, QByteArray());
    body += tlv(CHAT_TLV_MESSAGE_INFO, messageInfo);

    it.value()->connection->sendSnac(FAM_CHAT, CHAT_MSG_TO_HOST, body);
}

void OscarBackend::doLeaveRoom(const QString &name)
{
    const QString key = name.toCaseFolded();
    auto it = m_chats.find(key);
    if (it == m_chats.end()) {
        return;
    }
    if (it.value() && it.value()->connection) {
        it.value()->connection->close();
    }
    m_chats.erase(it);
    emit membersChanged(name, QStringLiteral("replace"), {});
}

void OscarBackend::doChangePassword(const QString &currentPassword,
                                    const QString &newPassword)
{
    if (currentPassword.isEmpty() || newPassword.isEmpty()) {
        fail(QStringLiteral("passwords cannot be empty"));
    }

    const ServiceRedirect redirect = requestService(FAM_ADMIN);
    FlapConnection admin(redirect.host,
                         redirect.port,
                         QStringLiteral("Admin"),
                         m_settings.debug,
                         [this](const QString &s) { protocolLog(s); });
    bootstrapService(admin, redirect.cookie);

    QByteArray body;
    body += tlv(ADMIN_TLV_NEW_PASSWORD, newPassword);
    body += tlv(ADMIN_TLV_OLD_PASSWORD, currentPassword);

    const Snac reply = request(admin, FAM_ADMIN, ADMIN_INFO_CHANGE_REQUEST, body);
    if (reply.family == FAM_ADMIN && reply.subtype == 0x0001) {
        const int code = reply.body.size() >= 2 ? readU16(reply.body, 0) : -1;
        fail(QStringLiteral("Admin service error 0x%1")
                 .arg(code, 4, 16, QLatin1Char('0')));
    }
    if (reply.family != FAM_ADMIN || reply.subtype != ADMIN_INFO_CHANGE_REPLY) {
        fail(QStringLiteral("unexpected Admin reply %1/%2")
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }
    if (reply.body.size() < 4) {
        fail(QStringLiteral("truncated Admin password-change reply"));
    }

    qsizetype offset = 4;
    const quint16 count = readU16(reply.body, 2);
    const auto items = parseTlvs(reply.body, offset, static_cast<int>(count));
    const QByteArray error = firstTlv(items, ADMIN_TLV_ERROR_CODE);
    if (!error.isEmpty()) {
        const int code = error.size() >= 2 ? readU16(error, 0) : -1;
        fail(QStringLiteral("password change failed: Admin error 0x%1")
                 .arg(code, 4, 16, QLatin1Char('0')));
    }

    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[account] password changed successfully"));
    admin.close();
}

void OscarBackend::doRaw(const QString &familyText,
                         const QString &subtypeText,
                         const QString &hexBody)
{
    if (!m_bos) {
        fail(QStringLiteral("not logged in"));
    }

    bool familyOk = false;
    bool subtypeOk = false;
    const quint16 family = familyText.toUShort(&familyOk, 0);
    const quint16 subtype = subtypeText.toUShort(&subtypeOk, 0);
    if (!familyOk || !subtypeOk) {
        fail(QStringLiteral("family and subtype must be numeric, e.g. 0x01 and 0x16"));
    }

    QString compact = hexBody;
    compact.remove(QRegularExpression(QStringLiteral("\\s+")));
    if (compact.size() % 2 != 0
        || compact.contains(QRegularExpression(QStringLiteral("[^0-9A-Fa-f]")))) {
        fail(QStringLiteral("raw SNAC body must be hexadecimal bytes"));
    }
    const QByteArray body = compact.isEmpty() ? QByteArray() : QByteArray::fromHex(compact.toLatin1());
    const quint32 requestId = m_bos->sendSnac(family, subtype, body);
    emit eventReceived(
        QStringLiteral("status"), QString(),
        QStringLiteral("[raw] sent %1/%2 req=%3")
            .arg(family, 4, 16, QLatin1Char('0'))
            .arg(subtype, 4, 16, QLatin1Char('0'))
            .arg(requestId));
}


QList<OscarBackend::FeedbagItem> OscarBackend::parseFeedbagItems(const QByteArray &body) const
{
    QList<FeedbagItem> items;
    if (body.size() < 3) {
        return items;
    }

    qsizetype offset = 0;
    offset += 1; // feedbag version
    const quint16 count = readU16(body, offset);
    offset += 2;

    for (quint16 i = 0; i < count; ++i) {
        if (offset + 2 > body.size()) {
            break;
        }
        const quint16 nameLength = readU16(body, offset);
        offset += 2;
        if (offset + nameLength + 8 > body.size()) {
            break;
        }

        FeedbagItem item;
        item.name = QString::fromUtf8(body.mid(offset, nameLength));
        offset += nameLength;
        item.groupId = readU16(body, offset);
        offset += 2;
        item.itemId = readU16(body, offset);
        offset += 2;
        item.classId = readU16(body, offset);
        offset += 2;

        if (offset + 2 > body.size()) {
            break;
        }
        const quint16 tlvBytes = readU16(body, offset);
        offset += 2;
        if (offset + tlvBytes > body.size()) {
            break;
        }
        item.data = body.mid(offset, tlvBytes);
        offset += tlvBytes;
        items.push_back(item);
    }

    return items;
}

QStringList OscarBackend::feedbagBuddyNames() const
{
    QStringList names;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_BUDDY && !item.name.isEmpty()) {
            names.push_back(item.name);
        }
    }
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QByteArray OscarBackend::marshalFeedbagItem(const FeedbagItem &item) const
{
    QByteArray out;
    const QByteArray name = item.name.toUtf8();
    appendU16(out, static_cast<quint16>(name.size()));
    out += name;
    appendU16(out, item.groupId);
    appendU16(out, item.itemId);
    appendU16(out, item.classId);
    appendU16(out, static_cast<quint16>(item.data.size()));
    out += item.data;
    return out;
}

QByteArray OscarBackend::marshalFeedbagItems(const QList<FeedbagItem> &items) const
{
    QByteArray out;
    for (const FeedbagItem &item : items) {
        out += marshalFeedbagItem(item);
    }
    return out;
}

QByteArray OscarBackend::withGroupMembers(const QByteArray &data, const QList<quint16> &itemIds) const
{
    QList<Tlv> tlvs;
    if (!data.isEmpty()) {
        qsizetype offset = 0;
        try {
            tlvs = parseTlvs(data, offset);
        } catch (const std::exception &) {
            // Preserve operation even if an unusual group TLV block cannot be parsed.
            // The member-list TLV is the only value the client needs to replace here.
            tlvs.clear();
        }
    }

    QList<Tlv> rebuilt;
    for (const Tlv &entry : tlvs) {
        if (entry.type != FEEDBAG_TLV_GROUP_MEMBERS) {
            rebuilt.push_back(entry);
        }
    }

    if (!itemIds.isEmpty()) {
        QByteArray members;
        for (const quint16 id : itemIds) {
            appendU16(members, id);
        }
        rebuilt.push_back(Tlv{FEEDBAG_TLV_GROUP_MEMBERS, members});
    }

    QByteArray out;
    for (const Tlv &entry : rebuilt) {
        out += tlv(entry.type, entry.value);
    }
    return out;
}

quint16 OscarBackend::nextFeedbagGroupId() const
{
    QSet<quint16> used;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_GROUP) {
            used.insert(item.groupId);
        }
    }
    for (quint32 candidate = 1; candidate < 0xFFFF; ++candidate) {
        if (!used.contains(static_cast<quint16>(candidate))) {
            return static_cast<quint16>(candidate);
        }
    }
    fail(QStringLiteral("no free Feedbag group IDs remain"));
}

quint16 OscarBackend::nextFeedbagItemId(quint16 groupId) const
{
    QSet<quint16> used;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.groupId == groupId) {
            used.insert(item.itemId);
        }
    }
    for (quint32 candidate = 1; candidate < 0xFFFF; ++candidate) {
        if (!used.contains(static_cast<quint16>(candidate))) {
            return static_cast<quint16>(candidate);
        }
    }
    fail(QStringLiteral("no free Feedbag item IDs remain"));
}

void OscarBackend::checkFeedbagAck(const Snac &reply,
                                   int expectedItems,
                                   const QString &operation) const
{
    if (reply.family != FAM_FEEDBAG || reply.subtype != FEEDBAG_STATUS) {
        fail(QStringLiteral("%1: unexpected Feedbag reply %2/%3")
                 .arg(operation)
                 .arg(reply.family, 4, 16, QLatin1Char('0'))
                 .arg(reply.subtype, 4, 16, QLatin1Char('0')));
    }
    if (reply.body.size() < expectedItems * 2) {
        fail(QStringLiteral("%1: truncated Feedbag acknowledgement").arg(operation));
    }
    for (int i = 0; i < expectedItems; ++i) {
        const quint16 result = readU16(reply.body, i * 2);
        if (result != 0) {
            fail(QStringLiteral("%1: server rejected item %2 with SSI status 0x%3")
                     .arg(operation)
                     .arg(i + 1)
                     .arg(result, 4, 16, QLatin1Char('0')));
        }
    }
}

void OscarBackend::persistFeedbagChanges(const QList<FeedbagItem> &adds,
                                         const QList<FeedbagItem> &mods,
                                         const QList<FeedbagItem> &dels)
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG)) {
        fail(QStringLiteral("server does not advertise persistent Feedbag/SSI support"));
    }

    m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_EDIT_START, QByteArray());
    try {
        if (!dels.isEmpty()) {
            const Snac ack = request(*m_bos, FAM_FEEDBAG, FEEDBAG_DELETE,
                                     marshalFeedbagItems(dels), 5000);
            checkFeedbagAck(ack, dels.size(), QStringLiteral("buddy delete"));
        }
        if (!adds.isEmpty()) {
            const Snac ack = request(*m_bos, FAM_FEEDBAG, FEEDBAG_ADD,
                                     marshalFeedbagItems(adds), 5000);
            checkFeedbagAck(ack, adds.size(), QStringLiteral("buddy add"));
        }
        if (!mods.isEmpty()) {
            const Snac ack = request(*m_bos, FAM_FEEDBAG, FEEDBAG_MODIFY,
                                     marshalFeedbagItems(mods), 5000);
            checkFeedbagAck(ack, mods.size(), QStringLiteral("group update"));
        }
        m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_EDIT_END, QByteArray());
    } catch (...) {
        try {
            m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_EDIT_END, QByteArray());
        } catch (...) {
        }
        throw;
    }
}

void OscarBackend::loadBuddyList()
{
    if (!m_bos || !m_bos->families.contains(FAM_FEEDBAG)) {
        protocolLog(QStringLiteral("[buddy] server did not advertise Feedbag/SSI"));
        return;
    }

    try {
        const Snac reply = request(*m_bos, FAM_FEEDBAG, FEEDBAG_QUERY, QByteArray(), 5000);
        if (reply.family != FAM_FEEDBAG || reply.subtype != FEEDBAG_REPLY) {
            protocolLog(QStringLiteral("[buddy] unexpected feedbag reply %1/%2")
                            .arg(reply.family, 4, 16, QLatin1Char('0'))
                            .arg(reply.subtype, 4, 16, QLatin1Char('0')));
            return;
        }

        m_feedbagItems = parseFeedbagItems(reply.body);
        const QStringList names = feedbagBuddyNames();
        m_buddies.clear();
        for (const QString &name : names) {
            m_buddies.insert(name);
        }
        emit buddyListChanged(names);

        // Activate SSI/Feedbag for presence notifications.
        m_bos->sendSnac(FAM_FEEDBAG, FEEDBAG_USE, QByteArray());
        protocolLog(QStringLiteral("[buddy] loaded %1 persistent buddies").arg(names.size()));
    } catch (const std::exception &e) {
        protocolLog(QStringLiteral("[buddy] could not load feedbag: %1")
                        .arg(QString::fromUtf8(e.what())));
    }
}

void OscarBackend::doAddBuddy(const QString &name)
{
    if (!m_bos || name.trimmed().isEmpty()) {
        return;
    }
    const QString cleaned = name.trimmed();

    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_BUDDY
            && item.name.compare(cleaned, Qt::CaseInsensitive) == 0) {
            emit eventReceived(QStringLiteral("status"), QString(),
                               QStringLiteral("[buddy] %1 is already in the persistent buddy list").arg(cleaned));
            return;
        }
    }

    if (!m_bos->families.contains(FAM_FEEDBAG)) {
        // Legacy fallback: useful for presence during this session, but not persistent.
        m_bos->sendSnac(FAM_BUDDY, BUDDY_ADD, lp8(cleaned));
        m_buddies.insert(cleaned);
        QStringList names = m_buddies.values();
        names.sort(Qt::CaseInsensitive);
        emit buddyListChanged(names);
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[buddy] server lacks SSI; added %1 for this session only").arg(cleaned));
        return;
    }

    QList<FeedbagItem> adds;
    QList<FeedbagItem> mods;

    int groupIndex = -1;
    for (int i = 0; i < m_feedbagItems.size(); ++i) {
        const FeedbagItem &item = m_feedbagItems.at(i);
        if (item.classId == FEEDBAG_CLASS_GROUP && item.groupId != 0 && item.itemId == 0) {
            groupIndex = i;
            break;
        }
    }

    quint16 groupId = 0;
    if (groupIndex >= 0) {
        groupId = m_feedbagItems.at(groupIndex).groupId;
    } else {
        groupId = nextFeedbagGroupId();
    }

    FeedbagItem buddy;
    buddy.name = cleaned;
    buddy.groupId = groupId;
    buddy.itemId = nextFeedbagItemId(groupId);
    buddy.classId = FEEDBAG_CLASS_BUDDY;

    if (groupIndex >= 0) {
        FeedbagItem group = m_feedbagItems.at(groupIndex);
        QList<quint16> memberIds;
        for (const FeedbagItem &item : m_feedbagItems) {
            if (item.classId == FEEDBAG_CLASS_BUDDY && item.groupId == groupId) {
                memberIds.push_back(item.itemId);
            }
        }
        memberIds.push_back(buddy.itemId);
        group.data = withGroupMembers(group.data, memberIds);
        adds.push_back(buddy);
        mods.push_back(group);
    } else {
        FeedbagItem group;
        group.name = QStringLiteral("Buddies");
        group.groupId = groupId;
        group.itemId = 0;
        group.classId = FEEDBAG_CLASS_GROUP;
        group.data = withGroupMembers(QByteArray(), {buddy.itemId});

        int masterIndex = -1;
        for (int i = 0; i < m_feedbagItems.size(); ++i) {
            const FeedbagItem &item = m_feedbagItems.at(i);
            if (item.classId == FEEDBAG_CLASS_GROUP && item.groupId == 0 && item.itemId == 0) {
                masterIndex = i;
                break;
            }
        }

        if (masterIndex >= 0) {
            FeedbagItem master = m_feedbagItems.at(masterIndex);
            QList<quint16> groupIds;
            for (const FeedbagItem &item : m_feedbagItems) {
                if (item.classId == FEEDBAG_CLASS_GROUP && item.groupId != 0 && item.itemId == 0) {
                    groupIds.push_back(item.groupId);
                }
            }
            groupIds.push_back(groupId);
            master.data = withGroupMembers(master.data, groupIds);
            mods.push_back(master);
        } else {
            FeedbagItem master;
            master.groupId = 0;
            master.itemId = 0;
            master.classId = FEEDBAG_CLASS_GROUP;
            master.data = withGroupMembers(QByteArray(), {groupId});
            adds.push_back(master);
        }

        adds.push_back(group);
        adds.push_back(buddy);
    }

    persistFeedbagChanges(adds, mods, {});
    loadBuddyList();
    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[buddy] persistently added %1").arg(cleaned));
}

void OscarBackend::doRemoveBuddy(const QString &name)
{
    if (!m_bos || name.trimmed().isEmpty()) {
        return;
    }
    const QString cleaned = name.trimmed();

    if (!m_bos->families.contains(FAM_FEEDBAG)) {
        m_bos->sendSnac(FAM_BUDDY, BUDDY_REMOVE, lp8(cleaned));
        for (auto it = m_buddies.begin(); it != m_buddies.end();) {
            if (it->compare(cleaned, Qt::CaseInsensitive) == 0) {
                it = m_buddies.erase(it);
            } else {
                ++it;
            }
        }
        QStringList names = m_buddies.values();
        names.sort(Qt::CaseInsensitive);
        emit buddyListChanged(names);
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[buddy] server lacks SSI; removed %1 for this session only").arg(cleaned));
        return;
    }

    QList<FeedbagItem> dels;
    QSet<quint16> affectedGroups;
    for (const FeedbagItem &item : m_feedbagItems) {
        if (item.classId == FEEDBAG_CLASS_BUDDY
            && item.name.compare(cleaned, Qt::CaseInsensitive) == 0) {
            dels.push_back(item);
            affectedGroups.insert(item.groupId);
        }
    }

    if (dels.isEmpty()) {
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[buddy] %1 is not in the persistent buddy list").arg(cleaned));
        return;
    }

    QList<FeedbagItem> mods;
    for (const quint16 groupId : affectedGroups) {
        for (const FeedbagItem &item : m_feedbagItems) {
            if (item.classId != FEEDBAG_CLASS_GROUP
                || item.groupId != groupId || item.itemId != 0) {
                continue;
            }
            FeedbagItem group = item;
            QList<quint16> remaining;
            for (const FeedbagItem &candidate : m_feedbagItems) {
                if (candidate.classId != FEEDBAG_CLASS_BUDDY
                    || candidate.groupId != groupId) {
                    continue;
                }
                if (candidate.name.compare(cleaned, Qt::CaseInsensitive) != 0) {
                    remaining.push_back(candidate.itemId);
                }
            }
            group.data = withGroupMembers(group.data, remaining);
            mods.push_back(group);
            break;
        }
    }

    persistFeedbagChanges({}, mods, dels);
    loadBuddyList();
    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[buddy] persistently removed %1").arg(cleaned));
}

void OscarBackend::emitPresence()
{
    emit presenceChanged(m_presenceState, m_presenceMessage, m_idleSeconds);
    QString summary = m_presenceState;
    if (m_idleSeconds > 0) {
        summary += QStringLiteral(" + IDLE %1s").arg(m_idleSeconds);
    }
    if (!m_presenceMessage.isEmpty()) {
        summary += QStringLiteral(" — %1").arg(m_presenceMessage);
    }
    emit eventReceived(QStringLiteral("status"), QString(),
                       QStringLiteral("[presence] %1").arg(summary));
}

void OscarBackend::doSetAway(const QString &message, bool afk)
{
    if (!m_bos) fail(QStringLiteral("not logged in"));

    QString clean = message.trimmed();
    if (clean.isEmpty()) clean = afk ? QStringLiteral("AFK") : QStringLiteral("Away");
    QString wireText = afk && !clean.startsWith(QStringLiteral("[AFK]"), Qt::CaseInsensitive)
        ? QStringLiteral("[AFK] %1").arg(clean)
        : clean;
    const QString html = QStringLiteral("<HTML><BODY>%1</BODY></HTML>")
                             .arg(wireText.toHtmlEscaped());

    QByteArray body;
    body += tlv(LOCATE_TLV_UNAVAILABLE_TYPE,
                QByteArray("text/x-aolrtf; charset=\"us-ascii\""));
    body += tlv(LOCATE_TLV_UNAVAILABLE_DATA, html.toLatin1());
    m_bos->sendSnac(FAM_LOCATE, LOCATE_SET_INFO, body);

    m_presenceState = afk ? QStringLiteral("AFK") : QStringLiteral("AWAY");
    m_presenceMessage = clean;
    emitPresence();
}

void OscarBackend::doSetIdle(quint32 seconds)
{
    if (!m_bos) fail(QStringLiteral("not logged in"));
    QByteArray body;
    appendU32(body, seconds);
    m_bos->sendSnac(FAM_OSERVICE, OS_IDLE_NOTIFICATION, body);
    m_idleSeconds = seconds;
    emitPresence();
}

void OscarBackend::doSetBack()
{
    if (!m_bos) fail(QStringLiteral("not logged in"));

    // Empty UNAVAILABLE tags clear the classic AIM away message.
    QByteArray locateBody;
    locateBody += tlv(LOCATE_TLV_UNAVAILABLE_TYPE, QByteArray());
    locateBody += tlv(LOCATE_TLV_UNAVAILABLE_DATA, QByteArray());
    m_bos->sendSnac(FAM_LOCATE, LOCATE_SET_INFO, locateBody);

    QByteArray idleBody;
    appendU32(idleBody, 0);
    m_bos->sendSnac(FAM_OSERVICE, OS_IDLE_NOTIFICATION, idleBody);

    m_presenceState = QStringLiteral("ONLINE");
    m_presenceMessage.clear();
    m_idleSeconds = 0;
    emitPresence();
}

void OscarBackend::processCommand(const Command &command)
{
    try {
        switch (command.type) {
        case CommandType::SendIm:
            doSendIm(command.a, command.b, !command.flag);
            if (command.flag) {
                const QString hint = m_peerClientHints.value(command.a.toCaseFolded());
                if (!hint.isEmpty()) emit eventReceived(QStringLiteral("version"), command.a, hint);
            }
            break;
        case CommandType::JoinRoom:
            doJoinRoom(command.a, command.flag);
            break;
        case CommandType::SendRoom:
            doSendRoom(command.a, command.b);
            break;
        case CommandType::LeaveRoom:
            doLeaveRoom(command.a);
            break;
        case CommandType::Password:
            doChangePassword(command.a, command.b);
            break;
        case CommandType::Raw:
            doRaw(command.a, command.b, command.c);
            break;
        case CommandType::AddBuddy:
            doAddBuddy(command.a);
            break;
        case CommandType::RemoveBuddy:
            doRemoveBuddy(command.a);
            break;
        case CommandType::SetAway:
            doSetAway(command.a, false);
            break;
        case CommandType::SetAfk:
            doSetAway(command.a, true);
            break;
        case CommandType::SetIdle:
            doSetIdle(command.number);
            break;
        case CommandType::SetBack:
            doSetBack();
            break;
        }
    } catch (const std::exception &e) {
        emit backendError(QStringLiteral("AIM/OSCAR"), QString::fromUtf8(e.what()));
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[error] %1").arg(QString::fromUtf8(e.what())));
    }
}

void OscarBackend::dispatchBos(const Snac &snac)
{
    if (snac.family == FAM_ICBM && snac.subtype == ICBM_MSG_TO_CLIENT) {
        if (snac.body.size() < 10) {
            protocolLog(QStringLiteral("[event error] truncated incoming ICBM"));
            return;
        }

        qsizetype offset = 10;
        const UserInfo sender = parseUserInfo(snac.body, offset);
        const auto items = parseTlvs(snac.body, offset);
        const QByteArray messageBlob = firstTlv(items, ICBM_TLV_IM_DATA);
        const QString message = messageBlob.isEmpty()
                              ? QStringLiteral("<non-text ICBM>")
                              : stripAimHtml(parseIcbmMessage(messageBlob));
        const QString peerKey = sender.name.toCaseFolded();
        if (message == QStringLiteral("[[WHVER:Q]]")) {
            doSendIm(sender.name,
                     QStringLiteral("[[WHVER:R:%1]]").arg(appVersionString()),
                     false);
            return;
        }
        if (message.startsWith(QStringLiteral("[[WHVER:R:")) && message.endsWith(QStringLiteral("]]"))) {
            const QString version = message.mid(10, message.size() - 12).trimmed();
            const QString reported = version.isEmpty()
                ? QStringLiteral("WaffleHouse-Client (version unavailable)")
                : QStringLiteral("WaffleHouse-Client %1").arg(version);
            m_peerClientHints.insert(peerKey, reported);
            emit eventReceived(QStringLiteral("version"), sender.name, reported);
            return;
        }
        if (message.startsWith(QStringLiteral("[[CPX3:")) && !m_peerClientHints.contains(peerKey)) {
            m_peerClientHints.insert(peerKey,
                QStringLiteral("Legacy CPX3-compatible client detected; exact WaffleHouse-Client version unavailable"));
        }
        emit eventReceived(QStringLiteral("im"), sender.name,
                           QStringLiteral("<%1> %2").arg(sender.name, message));
        return;
    }

    if (snac.family == FAM_ICBM && snac.subtype == ICBM_HOST_ACK) {
        if (m_settings.debug) {
            protocolLog(QStringLiteral("[debug] IM acknowledged by host"));
        }
        return;
    }

    if (snac.family == FAM_ICBM
        && (snac.subtype == 0x0001 || snac.subtype == ICBM_CLIENT_ERROR)) {
        const int code = snac.body.size() >= 2 ? readU16(snac.body, 0) : -1;
        protocolLog(QStringLiteral("[IM error] OSCAR error 0x%1")
                        .arg(code, 4, 16, QLatin1Char('0')));
        return;
    }

    if (snac.family == FAM_BUDDY
        && (snac.subtype == BUDDY_ONCOMING || snac.subtype == BUDDY_OFFGOING)) {
        try {
            qsizetype offset = 0;
            const UserInfo info = parseUserInfo(snac.body, offset);
            const bool online = snac.subtype == BUDDY_ONCOMING;
            if (!info.name.isEmpty()) {
                emit buddyPresenceChanged(info.name, online);
            }
        } catch (const std::exception &e) {
            protocolLog(QStringLiteral("[buddy] presence parse error: %1")
                            .arg(QString::fromUtf8(e.what())));
        }
        return;
    }

    if (m_settings.debug) {
        protocolLog(QStringLiteral("[debug] BOS: unhandled SNAC %1/%2 req=%3 body=%4")
                        .arg(snac.family, 4, 16, QLatin1Char('0'))
                        .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                        .arg(snac.requestId)
                        .arg(QString::fromLatin1(snac.body.toHex())));
    }
}

void OscarBackend::dispatchChat(FlapConnection &connection, const Snac &snac)
{
    const QString roomName = connection.label().startsWith(QStringLiteral("chat:"))
                           ? connection.label().mid(5)
                           : connection.label();

    if (snac.family == FAM_CHAT && snac.subtype == CHAT_MSG_TO_CLIENT) {
        if (snac.body.size() < 10) {
            protocolLog(QStringLiteral("[chat event error] truncated chat message"));
            return;
        }

        qsizetype offset = 10;
        const auto items = parseTlvs(snac.body, offset);
        const QByteArray senderBlob = firstTlv(items, CHAT_TLV_SENDER);
        const QByteArray messageBlob = firstTlv(items, CHAT_TLV_MESSAGE_INFO);

        QString sender = QStringLiteral("?");
        if (!senderBlob.isEmpty()) {
            qsizetype senderOffset = 0;
            sender = parseUserInfo(senderBlob, senderOffset).name;
        }

        QString text = QStringLiteral("<non-text chat message>");
        if (!messageBlob.isEmpty()) {
            qsizetype nestedOffset = 0;
            const auto nested = parseTlvs(messageBlob, nestedOffset);
            const QByteArray textBlob = firstTlv(nested, CHAT_MSG_TLV_TEXT);
            if (!textBlob.isEmpty()) {
                text = stripAimHtml(QString::fromUtf8(textBlob));
            }
        }

        emit eventReceived(QStringLiteral("chat"), roomName,
                           QStringLiteral("<%1> %2").arg(sender, text));
        return;
    }

    if (snac.family == FAM_CHAT
        && (snac.subtype == CHAT_USERS_JOINED || snac.subtype == CHAT_USERS_LEFT)) {
        qsizetype offset = 0;
        QStringList names;
        while (offset < snac.body.size()) {
            const qsizetype previous = offset;
            names.push_back(parseUserInfo(snac.body, offset).name);
            if (offset <= previous) {
                break;
            }
        }

        if (!names.isEmpty()) {
            const bool joined = snac.subtype == CHAT_USERS_JOINED;
            emit membersChanged(roomName,
                                joined ? QStringLiteral("add") : QStringLiteral("remove"),
                                names);
            emit eventReceived(QStringLiteral("chat"), roomName,
                               QStringLiteral("*** %1 %2")
                                   .arg(names.join(QStringLiteral(", ")),
                                        joined ? QStringLiteral("joined") : QStringLiteral("left")));
        }
        return;
    }

    if (snac.family == FAM_CHAT && snac.subtype == CHAT_ROOM_INFO) {
        if (m_settings.debug) {
            qsizetype offset = 0;
            const RoomInfo room = parseRoomInfo(snac.body, offset, true);
            protocolLog(QStringLiteral("[debug] chat room info: %1 exchange=%2 cookie=%3")
                            .arg(room.name())
                            .arg(room.exchange)
                            .arg(room.cookie));
        }
        return;
    }

    if (m_settings.debug) {
        protocolLog(QStringLiteral("[debug] %1: unhandled SNAC %2/%3 req=%4 body=%5")
                        .arg(connection.label())
                        .arg(snac.family, 4, 16, QLatin1Char('0'))
                        .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                        .arg(snac.requestId)
                        .arg(QString::fromLatin1(snac.body.toHex())));
    }
}

void OscarBackend::dispatchSnac(FlapConnection &connection, const Snac &snac)
{
    if (&connection == m_bos.get()) {
        dispatchBos(snac);
        return;
    }
    if (connection.label().startsWith(QStringLiteral("chat:"))) {
        dispatchChat(connection, snac);
        return;
    }

    if (m_settings.debug) {
        protocolLog(QStringLiteral("[debug] %1: unhandled SNAC %2/%3 req=%4 body=%5")
                        .arg(connection.label())
                        .arg(snac.family, 4, 16, QLatin1Char('0'))
                        .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                        .arg(snac.requestId)
                        .arg(QString::fromLatin1(snac.body.toHex())));
    }
}

void OscarBackend::processIncoming(FlapConnection &connection)
{
    while (connection.hasData()) {
        dispatchSnac(connection, connection.receiveSnac(1000));
    }
}

void OscarBackend::run()
{
    QString disconnectReason = QStringLiteral("signed off");

    try {
        const auto authResult = authenticate();
        const auto endpoint = redirectEndpoint(authResult.first, m_settings.port);

        m_bos = std::make_unique<FlapConnection>(
            endpoint.first,
            endpoint.second,
            QStringLiteral("BOS"),
            m_settings.debug,
            [this](const QString &s) { protocolLog(s); });
        bootstrapService(*m_bos, authResult.second, true);
        loadBuddyList();

        m_presenceState = QStringLiteral("ONLINE");
        m_presenceMessage.clear();
        m_idleSeconds = 0;
        emit presenceChanged(m_presenceState, m_presenceMessage, m_idleSeconds);

        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[online] signed on as %1 via %2:%3")
                               .arg(m_settings.username, endpoint.first)
                               .arg(endpoint.second));
        emit connected(m_settings.username,
                       QStringLiteral("%1:%2").arg(endpoint.first).arg(endpoint.second));

        while (!m_stopRequested) {
            for (const Command &command : takeCommands()) {
                processCommand(command);
            }

            if (m_bos && !m_bos->isConnected()) {
                fail(QStringLiteral("BOS connection closed"));
            }

            if (m_bos && m_bos->isConnected()) {
                // Let the BOS socket block briefly so this loop does not spin.
                // receiveSnac() itself is only called when bytes are available.
                // QTcpSocket::waitForReadyRead pumps the socket in this worker thread.
                if (!m_bos->hasData()) {
                    m_bos->waitForData(20);
                }
                processIncoming(*m_bos);
            }

            if (m_chatNav && m_chatNav->isConnected()) {
                if (!m_chatNav->hasData()) {
                    m_chatNav->waitForData(1);
                }
                processIncoming(*m_chatNav);
            }

            for (auto it = m_chats.begin(); it != m_chats.end(); ++it) {
                if (it.value() && it.value()->connection && it.value()->connection->isConnected()) {
                    if (!it.value()->connection->hasData()) {
                        it.value()->connection->waitForData(1);
                    }
                    processIncoming(*it.value()->connection);
                }
            }
        }
    } catch (const std::exception &e) {
        disconnectReason = QString::fromUtf8(e.what());
        emit backendError(QStringLiteral("AIM/OSCAR connection"), disconnectReason);
        emit eventReceived(QStringLiteral("status"), QString(),
                           QStringLiteral("[connection] %1").arg(disconnectReason));
    }

    m_chats.clear();
    if (m_chatNav) {
        m_chatNav->close();
        m_chatNav.reset();
    }
    if (m_bos) {
        m_bos->close();
        m_bos.reset();
    }

    emit eventReceived(QStringLiteral("status"), QString(), QStringLiteral("[offline] signed off"));
    emit disconnected(disconnectReason);
}
