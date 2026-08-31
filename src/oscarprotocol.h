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
constexpr quint16 FAM_ADVERT = 0x0005;
constexpr quint16 FAM_INVITE = 0x0006;
constexpr quint16 FAM_ADMIN = 0x0007;
constexpr quint16 FAM_POPUP = 0x0008;
constexpr quint16 FAM_PERMIT_DENY = 0x0009;
constexpr quint16 FAM_USER_LOOKUP = 0x000A;
constexpr quint16 FAM_STATS = 0x000B;
constexpr quint16 FAM_TRANSLATE = 0x000C;
constexpr quint16 FAM_CHATNAV = 0x000D;
constexpr quint16 FAM_CHAT = 0x000E;
constexpr quint16 FAM_ODIR = 0x000F;
constexpr quint16 FAM_BART = 0x0010;
constexpr quint16 FAM_FEEDBAG = 0x0013;
constexpr quint16 FAM_ICQ = 0x0015;
constexpr quint16 FAM_BUCP = 0x0017;
constexpr quint16 FAM_ALERT = 0x0018;
constexpr quint16 FAM_PLUGIN = 0x0022;
constexpr quint16 FAM_UNNAMED_24 = 0x0024;
constexpr quint16 FAM_MDIR = 0x0025;
constexpr quint16 FAM_ARS = 0x044A;

constexpr quint16 OS_CLIENT_ONLINE = 0x0002;
constexpr quint16 OS_HOST_ONLINE = 0x0003;
constexpr quint16 OS_SERVICE_REQUEST = 0x0004;
constexpr quint16 OS_SERVICE_RESPONSE = 0x0005;
constexpr quint16 OS_RATE_QUERY = 0x0006;
constexpr quint16 OS_RATE_REPLY = 0x0007;
constexpr quint16 OS_RATE_ACK = 0x0008;
constexpr quint16 OS_USER_INFO_QUERY = 0x000E;
constexpr quint16 OS_USER_INFO_UPDATE = 0x000F;
constexpr quint16 OS_EVIL_NOTIFICATION = 0x0010;
constexpr quint16 OS_IDLE_NOTIFICATION = 0x0011;
constexpr quint16 OS_MOTD = 0x0013;
constexpr quint16 OS_SET_PRIVACY_FLAGS = 0x0014;
constexpr quint16 OS_WELL_KNOWN_URLS = 0x0015;
constexpr quint16 OS_CLIENT_VERSIONS = 0x0017;
constexpr quint16 OS_HOST_VERSIONS = 0x0018;

constexpr quint16 LOCATE_RIGHTS_QUERY = 0x0002;
constexpr quint16 LOCATE_RIGHTS_REPLY = 0x0003;
constexpr quint16 LOCATE_SET_INFO = 0x0004;
constexpr quint16 LOCATE_USER_INFO_QUERY = 0x0005;
constexpr quint16 LOCATE_USER_INFO_REPLY = 0x0006;
constexpr quint16 LOCATE_WATCHER_SUB_REQUEST = 0x0007;
constexpr quint16 LOCATE_WATCHER_NOTIFICATION = 0x0008;
constexpr quint16 LOCATE_SET_DIR_INFO = 0x0009;
constexpr quint16 LOCATE_SET_DIR_REPLY = 0x000A;
constexpr quint16 LOCATE_GET_DIR_INFO = 0x000B;
constexpr quint16 LOCATE_GET_DIR_REPLY = 0x000C;
constexpr quint16 LOCATE_SET_KEYWORD_INFO = 0x000F;
constexpr quint16 LOCATE_SET_KEYWORD_REPLY = 0x0010;
constexpr quint16 LOCATE_GET_KEYWORD_INFO = 0x0011;
constexpr quint16 LOCATE_GET_KEYWORD_REPLY = 0x0012;
constexpr quint16 LOCATE_FIND_LIST_BY_EMAIL = 0x0013;
constexpr quint16 LOCATE_FIND_LIST_REPLY = 0x0014;
constexpr quint16 LOCATE_USER_INFO_QUERY2 = 0x0015;
constexpr quint16 LOCATE_TLV_PROFILE_TYPE = 0x0001;
constexpr quint16 LOCATE_TLV_PROFILE_DATA = 0x0002;
constexpr quint16 LOCATE_TLV_UNAVAILABLE_TYPE = 0x0003;
constexpr quint16 LOCATE_TLV_UNAVAILABLE_DATA = 0x0004;
constexpr quint16 LOCATE_TLV_CAPABILITIES = 0x0005;

// Generic OSCAR user-info TLVs returned by Buddy/Locate/ICBM user-info blocks.
constexpr quint16 USERINFO_TLV_FLAGS = 0x0001;
constexpr quint16 USERINFO_TLV_SIGNON_TIME = 0x0003;
constexpr quint16 USERINFO_TLV_IDLE_TIME = 0x0004;
constexpr quint16 USERINFO_TLV_MEMBER_SINCE = 0x0005;
constexpr quint16 USERINFO_TLV_STATUS = 0x0006;
constexpr quint16 USERINFO_TLV_CAPABILITIES = 0x000D;
constexpr quint16 USERINFO_TLV_ONLINE_TIME = 0x000F;
constexpr quint16 USERINFO_TLV_SHORT_CAPABILITIES = 0x0019;
constexpr quint16 USERINFO_TLV_FLAGS2 = 0x001F;

// OSERVICE__USER_FLAGS / OSERVICE__USER_STATUS values used by native AIM/ICQ
// presence blocks carried in BUDDY__ARRIVED and LOCATE__USER_INFO_REPLY.
constexpr quint64 USER_FLAG_UNAVAILABLE = 0x0020ULL;
constexpr quint32 USER_STATUS_AWAY = 0x0001U;
constexpr quint32 USER_STATUS_DND = 0x0002U;
constexpr quint32 USER_STATUS_NA = 0x0004U;
constexpr quint32 USER_STATUS_BUSY = 0x0010U;
constexpr quint32 USER_STATUS_FREE_FOR_CHAT = 0x0020U;
constexpr quint32 USER_STATUS_INVISIBLE = 0x0100U;

constexpr quint16 BUCP_ERR = 0x0001;
constexpr quint16 BUCP_LOGIN_REQUEST = 0x0002;
constexpr quint16 BUCP_LOGIN_RESPONSE = 0x0003;
constexpr quint16 BUCP_CHALLENGE_REQUEST = 0x0006;
constexpr quint16 BUCP_CHALLENGE_RESPONSE = 0x0007;
constexpr quint16 BUCP_SECURID_REQUEST = 0x000A;

constexpr quint16 TLV_SCREEN_NAME = 0x0001;
constexpr quint16 TLV_CLIENT_ID = 0x0003;
constexpr quint16 TLV_ERROR_URL = 0x0004;
constexpr quint16 TLV_RECONNECT_HOST = 0x0005;
constexpr quint16 TLV_AUTH_COOKIE = 0x0006;
constexpr quint16 TLV_LOGIN_ERROR = 0x0008;
constexpr quint16 TLV_PASSWORD_HASH = 0x0025;
constexpr quint16 TLV_MULTI_CONN = 0x004A;

constexpr quint16 ICBM_ADD_PARAMS = 0x0002;
constexpr quint16 ICBM_MSG_TO_HOST = 0x0006;
constexpr quint16 ICBM_MSG_TO_CLIENT = 0x0007;
constexpr quint16 ICBM_CLIENT_ERROR = 0x000B;
constexpr quint16 ICBM_PARAMETER_QUERY = 0x0004;
constexpr quint16 ICBM_PARAMETER_REPLY = 0x0005;
constexpr quint16 ICBM_MISSED_CALLS = 0x000A;
constexpr quint16 ICBM_HOST_ACK = 0x000C;
constexpr quint16 ICBM_SIN_STORED = 0x000D;
constexpr quint16 ICBM_SIN_LIST_QUERY = 0x000E;
constexpr quint16 ICBM_SIN_LIST_REPLY = 0x000F;
constexpr quint16 ICBM_SIN_RETRIEVE = 0x0010;
constexpr quint16 ICBM_SIN_DELETE = 0x0011;
constexpr quint16 ICBM_CLIENT_EVENT = 0x0014;
constexpr quint16 ICBM_SIN_REPLY = 0x0017;
constexpr quint16 ICBM_TLV_IM_DATA = 0x0002;
constexpr quint16 ICBM_TLV_ACK = 0x0003;
constexpr quint16 ICBM_CHANNEL_IM = 0x0001;
constexpr quint16 ICBM_CHANNEL_RENDEZVOUS = 0x0002;
constexpr quint16 ICBM_TLV_RENDEZVOUS = 0x0005;
constexpr quint16 ICBM_EVENT_FINISHED = 0x0000;
constexpr quint16 ICBM_EVENT_TYPED = 0x0001;
constexpr quint16 ICBM_EVENT_TYPING = 0x0002;

constexpr quint16 RENDEZVOUS_PROPOSE = 0x0000;
constexpr quint16 RENDEZVOUS_CANCEL = 0x0001;
constexpr quint16 RENDEZVOUS_ACCEPT = 0x0002;
constexpr quint16 RENDEZVOUS_REJECT = 0x0003;
constexpr quint16 RENDEZVOUS_TLV_CHANNEL = 0x0001;
constexpr quint16 RENDEZVOUS_TLV_IP = 0x0002;
constexpr quint16 RENDEZVOUS_TLV_REQUESTER_IP = 0x0003;
constexpr quint16 RENDEZVOUS_TLV_VERIFIED_IP = 0x0004;
constexpr quint16 RENDEZVOUS_TLV_PORT = 0x0005;
constexpr quint16 RENDEZVOUS_TLV_SEQUENCE = 0x000A;
constexpr quint16 RENDEZVOUS_TLV_CANCEL_REASON = 0x000B;
constexpr quint16 RENDEZVOUS_TLV_INVITATION = 0x000C;
// First service-specific TLV used by WaffleHouse voice.
constexpr quint16 RENDEZVOUS_TLV_WAFFLE_VOICE = 0x2711;

// Family 0x0003 - old-style buddy list / presence notifications.
constexpr quint16 BUDDY_RIGHTS_QUERY = 0x0002;
constexpr quint16 BUDDY_RIGHTS_REPLY = 0x0003;
constexpr quint16 BUDDY_ADD = 0x0004;
constexpr quint16 BUDDY_REMOVE = 0x0005;
constexpr quint16 BUDDY_WATCHER_LIST_QUERY = 0x0006;
constexpr quint16 BUDDY_WATCHER_LIST_RESPONSE = 0x0007;
constexpr quint16 BUDDY_WATCHER_SUB_REQUEST = 0x0008;
constexpr quint16 BUDDY_WATCHER_NOTIFICATION = 0x0009;
constexpr quint16 BUDDY_REJECT_NOTIFICATION = 0x000A;
constexpr quint16 BUDDY_ONCOMING = 0x000B;
constexpr quint16 BUDDY_OFFGOING = 0x000C;
constexpr quint16 BUDDY_ADD_TEMP = 0x000F;
constexpr quint16 BUDDY_REMOVE_TEMP = 0x0010;
constexpr quint16 BUDDY_RIGHTS_TLV_FLAGS = 0x0005;
constexpr quint16 BUDDY_RIGHTS_FLAG_INITIAL_DEPARTS = 0x0002;

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
constexpr quint16 FEEDBAG_AUTHORIZE_BUDDY = 0x0013;
constexpr quint16 FEEDBAG_PRE_AUTHORIZE_BUDDY = 0x0014;
constexpr quint16 FEEDBAG_PRE_AUTHORIZED_BUDDY = 0x0015;
constexpr quint16 FEEDBAG_REMOVE_ME = 0x0016;
constexpr quint16 FEEDBAG_REQUEST_AUTHORIZE_TO_HOST = 0x0018;
constexpr quint16 FEEDBAG_REQUEST_AUTHORIZE_TO_CLIENT = 0x0019;
constexpr quint16 FEEDBAG_RESPOND_AUTHORIZE_TO_HOST = 0x001A;
constexpr quint16 FEEDBAG_RESPOND_AUTHORIZE_TO_CLIENT = 0x001B;
constexpr quint16 FEEDBAG_BUDDY_ADDED = 0x001C;
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

constexpr quint16 INVITE_REQUEST_QUERY = 0x0002;
constexpr quint16 INVITE_REQUEST_REPLY = 0x0003;
constexpr quint16 INVITE_TLV_EMAIL = 0x0011;
constexpr quint16 INVITE_TLV_PERSONAL_TEXT = 0x0015;

constexpr quint16 ADMIN_INFO_QUERY = 0x0002;
constexpr quint16 ADMIN_INFO_REPLY = 0x0003;
constexpr quint16 ADMIN_INFO_CHANGE_REQUEST = 0x0004;
constexpr quint16 ADMIN_INFO_CHANGE_REPLY = 0x0005;
constexpr quint16 ADMIN_ACCOUNT_CONFIRM_REQUEST = 0x0006;
constexpr quint16 ADMIN_ACCOUNT_CONFIRM_REPLY = 0x0007;
constexpr quint16 ADMIN_ACCOUNT_DELETE_REQUEST = 0x0008;
constexpr quint16 ADMIN_ACCOUNT_DELETE_REPLY = 0x0009;
constexpr quint16 ADMIN_TLV_SCREEN_NAME = 0x0001;
constexpr quint16 ADMIN_TLV_NEW_PASSWORD = 0x0002;
constexpr quint16 ADMIN_TLV_ERROR_CODE = 0x0008;
constexpr quint16 ADMIN_TLV_EMAIL = 0x0011;
constexpr quint16 ADMIN_TLV_OLD_PASSWORD = 0x0012;
constexpr quint16 ADMIN_TLV_REG_STATUS = 0x0013;

constexpr quint16 PD_RIGHTS_QUERY = 0x0002;
constexpr quint16 PD_RIGHTS_REPLY = 0x0003;
constexpr quint16 PD_SET_GROUP_PERMIT_MASK = 0x0004;
constexpr quint16 PD_ADD_PERMIT = 0x0005;
constexpr quint16 PD_REMOVE_PERMIT = 0x0006;
constexpr quint16 PD_ADD_DENY = 0x0007;
constexpr quint16 PD_REMOVE_DENY = 0x0008;
constexpr quint16 PD_ADD_TEMP_PERMIT = 0x000A;
constexpr quint16 PD_REMOVE_TEMP_PERMIT = 0x000B;

constexpr quint16 USER_LOOKUP_FIND_BY_EMAIL = 0x0002;
constexpr quint16 USER_LOOKUP_FIND_REPLY = 0x0003;

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
QString authErrorDescription(quint16 code);

QByteArray passwordHash(const QString &password, const QByteArray &authKey);
QPair<QString, quint16> parseEndpoint(const QString &endpoint, quint16 defaultPort);
QString stripAimHtml(QString text);
QByteArray marshalIcbmFragments(const QString &message);
QString parseIcbmMessage(const QByteArray &data);

struct UserInfo {
    QString name;
    quint16 warningLevel = 0;
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
