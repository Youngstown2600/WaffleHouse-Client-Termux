#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QTcpSocket>

#include <functional>
#include <optional>
#include <stdexcept>
#include <utility>

namespace Oscar {

constexpr quint8 FLAP_SIGNON = 0x01;
constexpr quint8 FLAP_DATA = 0x02;
constexpr quint8 FLAP_ERROR = 0x03;
constexpr quint8 FLAP_SIGNOFF = 0x04;
constexpr quint8 FLAP_KEEPALIVE = 0x05;

constexpr quint16 FAM_OSERVICE = 0x0001;
constexpr quint16 FAM_LOCATE = 0x0002;
constexpr quint16 FAM_BUDDY = 0x0003;
constexpr quint16 FAM_ICBM = 0x0004;
constexpr quint16 FAM_ADMIN = 0x0007;
constexpr quint16 FAM_PERMIT_DENY = 0x0009;
constexpr quint16 FAM_CHATNAV = 0x000D;
constexpr quint16 FAM_CHAT = 0x000E;
constexpr quint16 FAM_FEEDBAG = 0x0013;
constexpr quint16 FAM_BUCP = 0x0017;

constexpr quint16 OS_CLIENT_ONLINE = 0x0002;
constexpr quint16 OS_HOST_ONLINE = 0x0003;
constexpr quint16 OS_SERVICE_REQUEST = 0x0004;
constexpr quint16 OS_SERVICE_RESPONSE = 0x0005;
constexpr quint16 OS_RATE_QUERY = 0x0006;
constexpr quint16 OS_RATE_REPLY = 0x0007;
constexpr quint16 OS_RATE_ACK = 0x0008;
constexpr quint16 OS_CLIENT_VERSIONS = 0x0017;
constexpr quint16 OS_HOST_VERSIONS = 0x0018;
constexpr quint16 OS_IDLE_NOTIFICATION = 0x0011;

constexpr quint16 LOCATE_SET_INFO = 0x0004;
constexpr quint16 LOCATE_TLV_UNAVAILABLE_TYPE = 0x0003;
constexpr quint16 LOCATE_TLV_UNAVAILABLE_DATA = 0x0004;

constexpr quint16 BUCP_LOGIN_REQUEST = 0x0002;
constexpr quint16 BUCP_LOGIN_RESPONSE = 0x0003;
constexpr quint16 BUCP_CHALLENGE_REQUEST = 0x0006;
constexpr quint16 BUCP_CHALLENGE_RESPONSE = 0x0007;

constexpr quint16 TLV_SCREEN_NAME = 0x0001;
constexpr quint16 TLV_CLIENT_ID = 0x0003;
constexpr quint16 TLV_RECONNECT_HOST = 0x0005;
constexpr quint16 TLV_AUTH_COOKIE = 0x0006;
constexpr quint16 TLV_LOGIN_ERROR = 0x0008;
constexpr quint16 TLV_PASSWORD_HASH = 0x0025;
constexpr quint16 TLV_MULTI_CONN = 0x004A;

constexpr quint16 ICBM_ADD_PARAMS = 0x0002;
constexpr quint16 ICBM_MSG_TO_HOST = 0x0006;
constexpr quint16 ICBM_MSG_TO_CLIENT = 0x0007;
constexpr quint16 ICBM_CLIENT_ERROR = 0x000B;
constexpr quint16 ICBM_HOST_ACK = 0x000C;
constexpr quint16 ICBM_TLV_IM_DATA = 0x0002;
constexpr quint16 ICBM_TLV_ACK = 0x0003;
constexpr quint16 ICBM_CHANNEL_IM = 0x0001;

// Family 0x0003 - old-style buddy list / presence notifications.
constexpr quint16 BUDDY_ADD = 0x0004;
constexpr quint16 BUDDY_REMOVE = 0x0005;
constexpr quint16 BUDDY_ONCOMING = 0x000B;
constexpr quint16 BUDDY_OFFGOING = 0x000C;

// Family 0x0013 - Feedbag / SSI (server-stored buddy list).
constexpr quint16 FEEDBAG_QUERY = 0x0004;
constexpr quint16 FEEDBAG_REPLY = 0x0006;
constexpr quint16 FEEDBAG_USE = 0x0007;
constexpr quint16 FEEDBAG_ADD = 0x0008;
constexpr quint16 FEEDBAG_MODIFY = 0x0009;
constexpr quint16 FEEDBAG_DELETE = 0x000A;
constexpr quint16 FEEDBAG_STATUS = 0x000E;
constexpr quint16 FEEDBAG_EDIT_START = 0x0011;
constexpr quint16 FEEDBAG_EDIT_END = 0x0012;
constexpr quint16 FEEDBAG_CLASS_BUDDY = 0x0000;
constexpr quint16 FEEDBAG_CLASS_GROUP = 0x0001;
constexpr quint16 FEEDBAG_TLV_GROUP_MEMBERS = 0x00C8;

constexpr quint16 CHATNAV_CREATE_ROOM = 0x0008;
constexpr quint16 CHATNAV_NAV_INFO = 0x0009;

constexpr quint16 CHAT_ROOM_INFO = 0x0002;
constexpr quint16 CHAT_USERS_JOINED = 0x0003;
constexpr quint16 CHAT_USERS_LEFT = 0x0004;
constexpr quint16 CHAT_MSG_TO_HOST = 0x0005;
constexpr quint16 CHAT_MSG_TO_CLIENT = 0x0006;

constexpr quint16 CHAT_TLV_PUBLIC = 0x0001;
constexpr quint16 CHAT_TLV_SENDER = 0x0003;
constexpr quint16 CHAT_TLV_MESSAGE_INFO = 0x0005;
constexpr quint16 CHAT_TLV_REFLECT = 0x0006;
constexpr quint16 CHAT_MSG_TLV_TEXT = 0x0001;
constexpr quint16 CHAT_MSG_TLV_ENCODING = 0x0002;
constexpr quint16 CHAT_MSG_TLV_LANG = 0x0003;
constexpr quint16 CHAT_ROOM_TLV_NAME = 0x00D3;

constexpr quint16 ADMIN_INFO_CHANGE_REQUEST = 0x0004;
constexpr quint16 ADMIN_INFO_CHANGE_REPLY = 0x0005;
constexpr quint16 ADMIN_TLV_NEW_PASSWORD = 0x0002;
constexpr quint16 ADMIN_TLV_OLD_PASSWORD = 0x0012;
constexpr quint16 ADMIN_TLV_ERROR_CODE = 0x0008;

constexpr quint16 PRIVATE_EXCHANGE = 4;
constexpr quint16 PUBLIC_EXCHANGE = 5;
constexpr quint16 SNAC_EXTENDED_INFO = 0x8000;

class ProtocolError : public std::runtime_error {
public:
    explicit ProtocolError(const QString &message)
        : std::runtime_error(message.toStdString()) {}
};

struct Tlv {
    quint16 type = 0;
    QByteArray value;
};

struct Snac {
    quint16 family = 0;
    quint16 subtype = 0;
    quint16 flags = 0;
    quint32 requestId = 0;
    QByteArray body;
};

struct FlapFrame {
    quint8 channel = 0;
    quint16 sequence = 0;
    QByteArray payload;
};

struct RoomInfo {
    quint16 exchange = 0;
    QString cookie;
    quint16 instance = 0;
    quint8 detailLevel = 0;
    QList<Tlv> tlvs;

    QString name() const;
};

void appendU8(QByteArray &out, quint8 value);
void appendU16(QByteArray &out, quint16 value);
void appendU32(QByteArray &out, quint32 value);
void appendU64(QByteArray &out, quint64 value);
quint8 readU8(const QByteArray &data, qsizetype offset);
quint16 readU16(const QByteArray &data, qsizetype offset);
quint32 readU32(const QByteArray &data, qsizetype offset);
quint64 readU64(const QByteArray &data, qsizetype offset);

QByteArray lp8(const QByteArray &data);
QByteArray lp8(const QString &text);
QByteArray tlv(quint16 type, const QByteArray &value = QByteArray());
QByteArray tlv(quint16 type, const QString &value);
QByteArray tlvBlock(const QList<Tlv> &items);

QList<Tlv> parseTlvs(const QByteArray &data,
                     qsizetype &offset,
                     std::optional<int> count = std::nullopt);
QByteArray firstTlv(const QList<Tlv> &items, quint16 type);

QByteArray passwordHash(const QString &password, const QByteArray &authKey);
QPair<QString, quint16> parseEndpoint(const QString &endpoint, quint16 defaultPort);
QString stripAimHtml(QString text);
QByteArray marshalIcbmFragments(const QString &message);
QString parseIcbmMessage(const QByteArray &data);

struct UserInfo {
    QString name;
    QList<Tlv> tlvs;
};
UserInfo parseUserInfo(const QByteArray &data, qsizetype &offset);
RoomInfo parseRoomInfo(const QByteArray &data, qsizetype &offset, bool withTlvBlock = true);

class FlapConnection {
public:
    using Logger = std::function<void(const QString &)>;

    FlapConnection(QString host,
                   quint16 port,
                   QString label,
                   bool debug,
                   Logger logger = {});
    ~FlapConnection();

    FlapConnection(const FlapConnection &) = delete;
    FlapConnection &operator=(const FlapConnection &) = delete;

    void connectToHost(int timeoutMs = 10000);
    void close();
    bool isConnected() const;
    bool hasData() const;
    bool waitForData(int timeoutMs);

    void sendFlap(quint8 channel, const QByteArray &payload);
    void sendSignon(const QList<Tlv> &items = {});
    quint32 sendSnac(quint16 family,
                     quint16 subtype,
                     const QByteArray &body = QByteArray(),
                     std::optional<quint32> requestId = std::nullopt,
                     quint16 flags = 0);

    FlapFrame receiveFlap(int timeoutMs = 10000);
    Snac receiveSnac(int timeoutMs = 10000);

    const QString &host() const { return m_host; }
    quint16 port() const { return m_port; }
    const QString &label() const { return m_label; }

    QList<quint16> families;

private:
    QByteArray readExact(qsizetype count, int timeoutMs);
    void writeAll(const QByteArray &data, int timeoutMs = 10000);
    quint32 nextRequestId();
    void log(const QString &text) const;

    QString m_host;
    quint16 m_port;
    QString m_label;
    bool m_debug = false;
    Logger m_logger;
    QTcpSocket m_socket;
    quint16 m_sequence = 0;
    quint32 m_requestId = 1;
};

} // namespace Oscar
