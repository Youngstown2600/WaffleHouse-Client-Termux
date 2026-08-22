#include "oscarprotocol.h"

#include <QCryptographicHash>
#include <QRandomGenerator>
#include <QRegularExpression>

#include <algorithm>
#include <string>

namespace Oscar {
namespace {

void requireRange(const QByteArray &data, qsizetype offset, qsizetype length, const char *what)
{
    if (offset < 0 || length < 0 || offset + length > data.size()) {
        throw ProtocolError(QStringLiteral("truncated %1").arg(QString::fromLatin1(what)));
    }
}

QString decodeText(const QByteArray &bytes, quint16 charset)
{
    if (charset == 2) {
        if (bytes.size() % 2 != 0) {
            return QString::fromUtf8(bytes);
        }
        std::u16string u16;
        u16.reserve(static_cast<std::size_t>(bytes.size() / 2));
        for (qsizetype i = 0; i + 1 < bytes.size(); i += 2) {
            u16.push_back(static_cast<char16_t>((static_cast<quint8>(bytes[i]) << 8)
                                                | static_cast<quint8>(bytes[i + 1])));
        }
        return QString::fromUtf16(u16.data(), static_cast<qsizetype>(u16.size()));
    }
    if (charset == 3) {
        return QString::fromLatin1(bytes);
    }
    return QString::fromUtf8(bytes);
}

} // namespace

void appendU8(QByteArray &out, quint8 value)
{
    out.append(static_cast<char>(value));
}

void appendU16(QByteArray &out, quint16 value)
{
    out.append(static_cast<char>((value >> 8) & 0xff));
    out.append(static_cast<char>(value & 0xff));
}

void appendU32(QByteArray &out, quint32 value)
{
    out.append(static_cast<char>((value >> 24) & 0xff));
    out.append(static_cast<char>((value >> 16) & 0xff));
    out.append(static_cast<char>((value >> 8) & 0xff));
    out.append(static_cast<char>(value & 0xff));
}

void appendU64(QByteArray &out, quint64 value)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.append(static_cast<char>((value >> shift) & 0xff));
    }
}

quint8 readU8(const QByteArray &data, qsizetype offset)
{
    requireRange(data, offset, 1, "uint8");
    return static_cast<quint8>(data[offset]);
}

quint16 readU16(const QByteArray &data, qsizetype offset)
{
    requireRange(data, offset, 2, "uint16");
    return static_cast<quint16>((static_cast<quint8>(data[offset]) << 8)
                                | static_cast<quint8>(data[offset + 1]));
}

quint32 readU32(const QByteArray &data, qsizetype offset)
{
    requireRange(data, offset, 4, "uint32");
    return (static_cast<quint32>(static_cast<quint8>(data[offset])) << 24)
         | (static_cast<quint32>(static_cast<quint8>(data[offset + 1])) << 16)
         | (static_cast<quint32>(static_cast<quint8>(data[offset + 2])) << 8)
         | static_cast<quint32>(static_cast<quint8>(data[offset + 3]));
}

quint64 readU64(const QByteArray &data, qsizetype offset)
{
    requireRange(data, offset, 8, "uint64");
    quint64 result = 0;
    for (int i = 0; i < 8; ++i) {
        result = (result << 8) | static_cast<quint8>(data[offset + i]);
    }
    return result;
}

QByteArray lp8(const QByteArray &data)
{
    if (data.size() > 255) {
        throw ProtocolError(QStringLiteral("8-bit length-prefixed field is too long"));
    }
    QByteArray out;
    appendU8(out, static_cast<quint8>(data.size()));
    out += data;
    return out;
}

QByteArray lp8(const QString &text)
{
    return lp8(text.toUtf8());
}

QByteArray tlv(quint16 type, const QByteArray &value)
{
    if (value.size() > 65535) {
        throw ProtocolError(QStringLiteral("TLV value is too long"));
    }
    QByteArray out;
    appendU16(out, type);
    appendU16(out, static_cast<quint16>(value.size()));
    out += value;
    return out;
}

QByteArray tlv(quint16 type, const QString &value)
{
    return tlv(type, value.toUtf8());
}

QByteArray tlvBlock(const QList<Tlv> &items)
{
    QByteArray out;
    appendU16(out, static_cast<quint16>(items.size()));
    for (const auto &item : items) {
        out += tlv(item.type, item.value);
    }
    return out;
}

QList<Tlv> parseTlvs(const QByteArray &data,
                     qsizetype &offset,
                     std::optional<int> count)
{
    QList<Tlv> out;
    while (offset + 4 <= data.size()
           && (!count.has_value() || out.size() < count.value())) {
        const quint16 type = readU16(data, offset);
        const quint16 length = readU16(data, offset + 2);
        offset += 4;
        requireRange(data, offset, length, "TLV value");
        out.push_back({type, data.mid(offset, length)});
        offset += length;
    }

    if (count.has_value() && out.size() != count.value()) {
        throw ProtocolError(QStringLiteral("truncated counted TLV block"));
    }
    return out;
}

QByteArray firstTlv(const QList<Tlv> &items, quint16 type)
{
    for (const auto &item : items) {
        if (item.type == type) {
            return item.value;
        }
    }
    return {};
}

QByteArray passwordHash(const QString &password, const QByteArray &authKey)
{
    const QByteArray inner = QCryptographicHash::hash(password.toUtf8(), QCryptographicHash::Md5);
    QByteArray data = authKey;
    data += inner;
    data += QByteArray("AOL Instant Messenger (SM)");
    return QCryptographicHash::hash(data, QCryptographicHash::Md5);
}

QPair<QString, quint16> parseEndpoint(const QString &endpoint, quint16 defaultPort)
{
    const QString value = endpoint.trimmed();

    if (value.startsWith(QLatin1Char('['))) {
        const int close = value.indexOf(QLatin1Char(']'));
        if (close > 0) {
            const QString host = value.mid(1, close - 1);
            const QString rest = value.mid(close + 1);
            if (rest.startsWith(QLatin1Char(':'))) {
                bool ok = false;
                const int parsed = rest.mid(1).toInt(&ok);
                if (ok && parsed > 0 && parsed <= 65535) {
                    return {host, static_cast<quint16>(parsed)};
                }
            }
            return {host, defaultPort};
        }
    }

    if (value.count(QLatin1Char(':')) == 1) {
        const int colon = value.lastIndexOf(QLatin1Char(':'));
        bool ok = false;
        const int parsed = value.mid(colon + 1).toInt(&ok);
        if (ok && parsed > 0 && parsed <= 65535) {
            return {value.left(colon), static_cast<quint16>(parsed)};
        }
    }

    return {value, defaultPort};
}

QString stripAimHtml(QString text)
{
    text.replace(QRegularExpression(QStringLiteral("(?i)<br\\s*/?>")), QStringLiteral("\n"));
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    text.replace(QStringLiteral("&lt;"), QStringLiteral("<"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("&gt;"), QStringLiteral(">"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("&quot;"), QStringLiteral("\""), Qt::CaseInsensitive);
    text.replace(QStringLiteral("&#39;"), QStringLiteral("'"), Qt::CaseInsensitive);
    text.replace(QStringLiteral("&nbsp;"), QStringLiteral(" "), Qt::CaseInsensitive);
    text.replace(QStringLiteral("&amp;"), QStringLiteral("&"), Qt::CaseInsensitive);
    return text;
}

QByteArray marshalIcbmFragments(const QString &message)
{
    const QByteArray raw = message.toUtf8();
    QByteArray messagePayload;
    appendU16(messagePayload, 0);
    appendU16(messagePayload, 0);
    messagePayload += raw;

    QByteArray capFragment;
    appendU8(capFragment, 5);
    appendU8(capFragment, 1);
    appendU16(capFragment, 3);
    capFragment.append(char(1));
    capFragment.append(char(1));
    capFragment.append(char(2));

    QByteArray msgFragment;
    appendU8(msgFragment, 1);
    appendU8(msgFragment, 1);
    appendU16(msgFragment, static_cast<quint16>(messagePayload.size()));
    msgFragment += messagePayload;

    return capFragment + msgFragment;
}

QString parseIcbmMessage(const QByteArray &data)
{
    qsizetype offset = 0;
    while (offset + 4 <= data.size()) {
        const quint8 fragmentId = readU8(data, offset);
        const quint16 length = readU16(data, offset + 2);
        offset += 4;
        if (offset + length > data.size()) {
            break;
        }
        const QByteArray payload = data.mid(offset, length);
        offset += length;

        if (fragmentId != 1 || payload.size() < 4) {
            continue;
        }

        const quint16 charset = readU16(payload, 0);
        return decodeText(payload.mid(4), charset);
    }
    return QStringLiteral("<unable to decode message>");
}

UserInfo parseUserInfo(const QByteArray &data, qsizetype &offset)
{
    requireRange(data, offset, 1, "user-info screen name length");
    const quint8 nameLength = readU8(data, offset++);
    requireRange(data, offset, nameLength + 4, "user info");

    UserInfo info;
    info.name = QString::fromUtf8(data.mid(offset, nameLength));
    offset += nameLength;

    // warning level
    offset += 2;
    const quint16 count = readU16(data, offset);
    offset += 2;
    info.tlvs = parseTlvs(data, offset, static_cast<int>(count));
    return info;
}

RoomInfo parseRoomInfo(const QByteArray &data, qsizetype &offset, bool withTlvBlock)
{
    requireRange(data, offset, 3, "room info");

    RoomInfo info;
    info.exchange = readU16(data, offset);
    offset += 2;

    const quint8 cookieLength = readU8(data, offset++);
    requireRange(data, offset, cookieLength + 2, "room cookie");
    info.cookie = QString::fromUtf8(data.mid(offset, cookieLength));
    offset += cookieLength;

    info.instance = readU16(data, offset);
    offset += 2;

    if (withTlvBlock) {
        requireRange(data, offset, 3, "room detail/TLV block");
        info.detailLevel = readU8(data, offset++);
        const quint16 count = readU16(data, offset);
        offset += 2;
        info.tlvs = parseTlvs(data, offset, static_cast<int>(count));
    }

    return info;
}

QString RoomInfo::name() const
{
    const QByteArray rawName = firstTlv(tlvs, CHAT_ROOM_TLV_NAME);
    if (!rawName.isEmpty()) {
        return QString::fromUtf8(rawName);
    }
    const QStringList parts = cookie.split(QLatin1Char('-'), Qt::KeepEmptyParts);
    if (parts.size() >= 3) {
        return parts.mid(2).join(QLatin1Char('-'));
    }
    return cookie;
}

FlapConnection::FlapConnection(QString host,
                               quint16 port,
                               QString label,
                               bool debug,
                               Logger logger)
    : m_host(std::move(host)),
      m_port(port),
      m_label(std::move(label)),
      m_debug(debug),
      m_logger(std::move(logger)),
      m_sequence(static_cast<quint16>(QRandomGenerator::global()->generate() & 0xffff)),
      m_requestId(QRandomGenerator::global()->generate() & 0x7fffffff)
{
    if (m_requestId == 0) {
        m_requestId = 1;
    }
}

FlapConnection::~FlapConnection()
{
    close();
}

void FlapConnection::log(const QString &text) const
{
    if (m_logger) {
        m_logger(text);
    }
}

void FlapConnection::connectToHost(int timeoutMs)
{
    m_socket.connectToHost(m_host, m_port);
    if (!m_socket.waitForConnected(timeoutMs)) {
        throw ProtocolError(QStringLiteral("%1: connect to %2:%3 failed: %4")
                                .arg(m_label, m_host)
                                .arg(m_port)
                                .arg(m_socket.errorString()));
    }
    if (m_debug) {
        log(QStringLiteral("[debug] %1: TCP connected to %2:%3")
                .arg(m_label, m_host)
                .arg(m_port));
    }
}

void FlapConnection::close()
{
    if (m_socket.state() == QAbstractSocket::ConnectedState) {
        try {
            sendFlap(FLAP_SIGNOFF, QByteArray());
        } catch (...) {
        }
        m_socket.disconnectFromHost();
        if (m_socket.state() != QAbstractSocket::UnconnectedState) {
            m_socket.waitForDisconnected(500);
        }
    }
    m_socket.close();
}

bool FlapConnection::isConnected() const
{
    return m_socket.state() == QAbstractSocket::ConnectedState;
}

bool FlapConnection::hasData() const
{
    return m_socket.bytesAvailable() > 0;
}

bool FlapConnection::waitForData(int timeoutMs)
{
    if (m_socket.bytesAvailable() > 0) {
        return true;
    }
    return m_socket.waitForReadyRead(timeoutMs);
}

QByteArray FlapConnection::readExact(qsizetype count, int timeoutMs)
{
    QByteArray out;
    out.reserve(count);
    while (out.size() < count) {
        if (m_socket.bytesAvailable() <= 0 && !m_socket.waitForReadyRead(timeoutMs)) {
            if (m_socket.state() != QAbstractSocket::ConnectedState) {
                throw ProtocolError(QStringLiteral("%1: connection closed").arg(m_label));
            }
            throw ProtocolError(QStringLiteral("%1: timed out reading OSCAR frame").arg(m_label));
        }
        const QByteArray chunk = m_socket.read(count - out.size());
        if (chunk.isEmpty() && m_socket.state() != QAbstractSocket::ConnectedState) {
            throw ProtocolError(QStringLiteral("%1: connection closed").arg(m_label));
        }
        out += chunk;
    }
    return out;
}

void FlapConnection::writeAll(const QByteArray &data, int timeoutMs)
{
    qsizetype offset = 0;
    while (offset < data.size()) {
        const qint64 written = m_socket.write(data.constData() + offset, data.size() - offset);
        if (written < 0) {
            throw ProtocolError(QStringLiteral("%1: socket write failed: %2")
                                    .arg(m_label, m_socket.errorString()));
        }
        offset += written;
        if (m_socket.bytesToWrite() > 0 && !m_socket.waitForBytesWritten(timeoutMs)) {
            throw ProtocolError(QStringLiteral("%1: timed out writing OSCAR frame")
                                    .arg(m_label));
        }
    }
}

void FlapConnection::sendFlap(quint8 channel, const QByteArray &payload)
{
    if (!isConnected()) {
        throw ProtocolError(QStringLiteral("%1: not connected").arg(m_label));
    }
    if (payload.size() > 65535) {
        throw ProtocolError(QStringLiteral("%1: FLAP payload too large").arg(m_label));
    }

    QByteArray frame;
    appendU8(frame, 0x2a);
    appendU8(frame, channel);
    appendU16(frame, m_sequence++);
    appendU16(frame, static_cast<quint16>(payload.size()));
    frame += payload;
    writeAll(frame);

    if (m_debug) {
        log(QStringLiteral("[debug] %1 => FLAP ch=%2 len=%3")
                .arg(m_label)
                .arg(channel)
                .arg(payload.size()));
    }
}

void FlapConnection::sendSignon(const QList<Tlv> &items)
{
    QByteArray payload;
    appendU32(payload, 1);
    for (const auto &item : items) {
        payload += tlv(item.type, item.value);
    }
    sendFlap(FLAP_SIGNON, payload);
}

quint32 FlapConnection::nextRequestId()
{
    m_requestId = (m_requestId + 1) & 0x7fffffff;
    if (m_requestId == 0) {
        m_requestId = 1;
    }
    return m_requestId;
}

quint32 FlapConnection::sendSnac(quint16 family,
                                 quint16 subtype,
                                 const QByteArray &body,
                                 std::optional<quint32> requestId,
                                 quint16 flags)
{
    const quint32 id = requestId.value_or(nextRequestId());
    QByteArray payload;
    appendU16(payload, family);
    appendU16(payload, subtype);
    appendU16(payload, flags);
    appendU32(payload, id);
    payload += body;
    sendFlap(FLAP_DATA, payload);

    if (m_debug) {
        log(QStringLiteral("[debug] %1 => SNAC %2/%3 req=%4 body=%5")
                .arg(m_label)
                .arg(family, 4, 16, QLatin1Char('0'))
                .arg(subtype, 4, 16, QLatin1Char('0'))
                .arg(id)
                .arg(QString::fromLatin1(body.toHex())));
    }
    return id;
}

FlapFrame FlapConnection::receiveFlap(int timeoutMs)
{
    const QByteArray header = readExact(6, timeoutMs);
    if (readU8(header, 0) != 0x2a) {
        throw ProtocolError(QStringLiteral("%1: invalid FLAP marker").arg(m_label));
    }

    FlapFrame frame;
    frame.channel = readU8(header, 1);
    frame.sequence = readU16(header, 2);
    const quint16 length = readU16(header, 4);
    frame.payload = readExact(length, timeoutMs);

    if (m_debug) {
        log(QStringLiteral("[debug] %1 <= FLAP ch=%2 seq=%3 len=%4")
                .arg(m_label)
                .arg(frame.channel)
                .arg(frame.sequence)
                .arg(frame.payload.size()));
    }
    return frame;
}

Snac FlapConnection::receiveSnac(int timeoutMs)
{
    while (true) {
        const FlapFrame frame = receiveFlap(timeoutMs);
        if (frame.channel == FLAP_KEEPALIVE) {
            continue;
        }
        if (frame.channel == FLAP_SIGNOFF) {
            throw ProtocolError(QStringLiteral("%1: server signed off").arg(m_label));
        }
        if (frame.channel != FLAP_DATA) {
            continue;
        }
        if (frame.payload.size() < 10) {
            throw ProtocolError(QStringLiteral("%1: truncated SNAC header").arg(m_label));
        }

        Snac snac;
        snac.family = readU16(frame.payload, 0);
        snac.subtype = readU16(frame.payload, 2);
        snac.flags = readU16(frame.payload, 4);
        snac.requestId = readU32(frame.payload, 6);
        snac.body = frame.payload.mid(10);

        if (snac.flags & SNAC_EXTENDED_INFO) {
            if (snac.body.size() < 2) {
                throw ProtocolError(QStringLiteral("truncated extended SNAC info"));
            }
            const quint16 extLength = readU16(snac.body, 0);
            if (snac.body.size() < 2 + extLength) {
                throw ProtocolError(QStringLiteral("truncated extended SNAC block"));
            }
            snac.body = snac.body.mid(2 + extLength);
        }

        if (m_debug) {
            log(QStringLiteral("[debug] %1 <= SNAC %2/%3 req=%4 body=%5")
                    .arg(m_label)
                    .arg(snac.family, 4, 16, QLatin1Char('0'))
                    .arg(snac.subtype, 4, 16, QLatin1Char('0'))
                    .arg(snac.requestId)
                    .arg(QString::fromLatin1(snac.body.toHex())));
        }
        return snac;
    }
}

} // namespace Oscar
