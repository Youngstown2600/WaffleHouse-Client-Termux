#pragma once

#include "backend.h"
#include "securechannel.h"
#include "secureroom.h"
#include "filetransfer.h"
#include "directtransfer.h"
#include "ansiterminal.h"

#include <QObject>
#include <QHash>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <memory>

class ChatBackend;
class SipController;
class MediaController;

class TerminalUi : public QObject {
    Q_OBJECT
public:
    explicit TerminalUi(QObject *parent = nullptr);
    ~TerminalUi() override;

    bool start();

signals:
    void finished();

private:
    struct ConnectionEntry {
        QString id;
        ConnectionSettings settings;
        ChatBackend *backend = nullptr;
        bool secretRequired = false;
        bool hasSessionSecret = false;
        bool connected = false;
        bool connecting = false;
        bool persistent = true;
        QString identity;
        QString endpoint;
        QSet<QString> buddies;
        QSet<QString> onlineBuddies;
        QHash<QString, QString> targetNames;
        QHash<QString, QString> discoveredRooms;
        QString presenceState = QStringLiteral("ONLINE");
        QString presenceMessage;
        quint32 idleSeconds = 0;
        QString autoPresenceState;
    };

    struct UiOptions {
        bool showSplash = true;
        bool showTimestamps = true;
        bool showScrollbars = true;
        bool showSidePanes = true;
        bool encryptedDmEnabled = true;
        bool autoReplySecure = true;
        bool showSecureFingerprints = true;
        bool autoPresenceEnabled = true;
        int autoIdleMinutes = 5;
        int autoAwayMinutes = 15;
        QString theme = QStringLiteral("phosphor");
    };

    struct Buffer {
        int number = 0;
        QString key;
        QString name;
        QString kind;
        QString connectionId;
        QString target;
        QStringList lines;
        QSet<QString> members;
        int unread = 0;
        int scroll = 0;
        bool sensitiveInput = false;
        std::unique_ptr<AnsiTerminalModel> terminal;
    };

    void initCurses();
    void applyTheme();
    void showSplash();
    void shutdownCurses();
    void tick();
    void syncTerminalGeometry();
    void updateTelnetTerminalGeometry();
    int terminalPaneColumns() const;
    int terminalPaneRows() const;
    void draw();
    void handleInput();
    void handleCharacter(uint codepoint);
    void handleSpecialKey(int key);
    void submitInput();
    void completeCommand(int direction = 1);
    void resetCommandCompletion();
    static QStringList slashCommands();
    bool sendPrivateText(ConnectionEntry *entry, const QString &target, const QString &text, Buffer *buffer = nullptr);
    bool sendSecureControlPayload(ConnectionEntry *entry,
                                  const QString &target,
                                  const QString &plaintext,
                                  Buffer *buffer = nullptr);
    bool handleFileTransferPayload(ConnectionEntry *entry,
                                   const QString &target,
                                   const QString &plaintext,
                                   Buffer *buffer,
                                   bool secureTransport = true);
    void pumpFileTransfers();
    void appendTransferProgress(Buffer *buffer,
                                const CpxFileTransferManager::Event &event,
                                const QString &direction);
    bool resumeIncomingFileTransfer(const QString &transferId, ConnectionEntry *entry, Buffer *buffer);
    void startDirectOutgoing(const CpxFileTransferManager::Event &event, ConnectionEntry *entry, Buffer *buffer);
    void handleDirectProgress(const QString &transferId, qint64 transferred, qint64 total, bool outgoing);
    void handleDirectIncomingFinished(const QString &transferId);
    void handleDirectOutgoingFinished(const QString &transferId);
    void handleDirectFailure(const QString &transferId, const QString &reason, bool outgoing);

    void drawHeader(int width);
    void drawConnectionsBar(int row, int width);
    void drawBufferPane(Buffer *buffer, int top, int bottom, int width);
    void drawScrollBar(int top, int bottom, int x, int totalLines, int firstLine, int visibleLines);
    void drawBuddyPane(ConnectionEntry *entry, int top, int bottom, int startX, int width);
    void drawMemberPane(Buffer *buffer, int top, int bottom, int startX, int width);
    void drawShortcutHint(int row, int width);
    void drawStatusBar(int row, int width);
    void drawInputLine(int row, int width);

    void safeAdd(int y, int x, const QString &text, int attr = 0, int maxWidth = -1);
    static QStringList wrapText(const QString &text, int width);
    static QString protocolShort(ConnectionSettings::Protocol protocol);
    static QString protocolName(ConnectionSettings::Protocol protocol);
    static QString connectionLabel(const ConnectionEntry *entry);

    Buffer *ensureBuffer(const QString &kind,
                         const QString &connectionId = QString(),
                         const QString &target = QString(),
                         const QString &displayName = QString(),
                         bool switchTo = false);
    Buffer *findBuffer(const QString &key) const;
    Buffer *activeBuffer() const;
    void switchBuffer(int index);
    void switchToBuffer(Buffer *buffer);
    void nextBuffer();
    void previousBuffer();
    void closeActiveBuffer();
    void removeConnectionConversationBuffers(const QString &connectionId);
    void renumberBuffers();
    QString bufferKey(const QString &kind,
                      const QString &connectionId,
                      const QString &target) const;

    void append(Buffer *buffer, const QString &text, bool markUnread = true);
    void status(const QString &text);
    void connectionStatus(ConnectionEntry *entry, const QString &text);

    ChatBackend *createBackend(const ConnectionSettings &settings);
    ConnectionEntry *addConnectionEntry(const ConnectionSettings &settings,
                                        bool secretRequired,
                                        bool hasSessionSecret,
                                        bool persist,
                                        bool autoConnect,
                                        const QString &profileId = QString());
    void attachBackend(ConnectionEntry *entry, ChatBackend *backend);
    void replaceBackend(ConnectionEntry *entry, const ConnectionSettings &settings);
    void deleteConnection(ConnectionEntry *entry);
    void connectConnection(ConnectionEntry *entry);
    void disconnectConnection(ConnectionEntry *entry);
    ConnectionEntry *connectionById(const QString &id) const;
    ConnectionEntry *selectedConnection() const;
    ConnectionEntry *selectedSipConnection() const;
    ConnectionEntry *sipConnectionByAccountId(const QString &accountId) const;
    ConnectionEntry *resolveConnection(const QString &token, bool allowEmpty = true) const;
    void selectConnection(ConnectionEntry *entry, bool switchToStatus = true);
    void nextConnection();
    void previousConnection();

    void loadConnections();
    void saveConnections() const;
    void loadOptions();
    void saveOptions() const;
    void showOptions();
    void markUserActivity();
    void updateAutoPresence();
    void requestClientVersion(ConnectionEntry *entry, QString target);
    bool ensureSecret(ConnectionEntry *entry);

    void onBackendEvent(ConnectionEntry *entry,
                        const QString &kind,
                        const QString &target,
                        const QString &text);
    void onMembersChanged(ConnectionEntry *entry,
                          const QString &room,
                          const QString &action,
                          const QStringList &names);
    void onTargetNamed(ConnectionEntry *entry,
                       const QString &kind,
                       const QString &target,
                       const QString &displayName);
    void onRoomDiscovered(ConnectionEntry *entry,
                          const QString &roomId,
                          const QString &displayName);
    void onBuddyListChanged(ConnectionEntry *entry, const QStringList &names);
    void onBuddyPresenceChanged(ConnectionEntry *entry,
                                const QString &name,
                                bool online);
    void onConnected(ConnectionEntry *entry,
                     const QString &identity,
                     const QString &endpoint);
    void onDisconnected(ConnectionEntry *entry, const QString &reason);
    void onBackendError(ConnectionEntry *entry,
                        const QString &context,
                        const QString &message);

    QString imPayload(const QString &text) const;
    QString imSpeakerPrefix(const QString &text) const;
    QString secureTrustKey(ConnectionEntry *entry, const QString &target) const;
    QString trustedFingerprint(ConnectionEntry *entry, const QString &target) const;
    void setTrustedFingerprint(ConnectionEntry *entry, const QString &target, const QString &fingerprint);
    void clearTrustedFingerprint(ConnectionEntry *entry, const QString &target);
    bool secureTarget(ConnectionEntry *entry, QString target, bool switchTo = true);
    bool startSecureRoom(ConnectionEntry *entry, Buffer *buffer);
    void showSecureRoomStatus(ConnectionEntry *entry, Buffer *buffer);
    void closeSecureRoom(ConnectionEntry *entry, Buffer *buffer);
    void distributeSecureRoomKey(ConnectionEntry *entry, Buffer *buffer, const QString &peer);
    void distributeSecureRoomKeyToMembers(ConnectionEntry *entry, Buffer *buffer);
    void flushPendingSecureRoomKeys(ConnectionEntry *entry, const QString &peer);
    bool handleSecureRoomKeyOffer(ConnectionEntry *entry, const QString &peer, const QString &plaintext);
    QString activeImTarget(ConnectionEntry *entry) const;

    void handleCommand(const QString &line);
    void showHelp();
    Buffer *phoneBuffer(bool switchTo = false);
    void showPhoneMain(bool switchTo = true);
    void showPhoneCalls(bool switchTo = true);
    void showPhoneProfile(bool switchTo = true);
    void showPhoneSipLog(int callId = -1, bool switchTo = true);
    void showPhoneLadder(int callId, bool switchTo = true);
    void showPhoneActivity(bool switchTo = true);
    void configurePhoneProfile();
    void listConnections();
    void listActiveConnections();
    void listBuddies(ConnectionEntry *entry);
    void listRooms(ConnectionEntry *entry);
    void listMembers(Buffer *buffer);

    void addConnectionWizard();
    void importBbsList(const QString &path);
    void openAdHocTelnet(const QString &spec, const QString &portText = QString());
    void editConnectionWizard(ConnectionEntry *entry);
    bool promptConnectionSettings(ConnectionSettings &settings,
                                  bool &secretRequired,
                                  QString &sessionSecret,
                                  bool editing);
    bool promptFileTransfer(ConnectionEntry *entry,
                            QString &target,
                            QString &path,
                            bool &secureTransfer);
    QString browseFile(const QString &initialPath);

    QString prompt(const QString &title,
                   const QString &label,
                   const QString &initial = QString(),
                   bool secret = false,
                   bool *cancelled = nullptr);
    bool confirm(const QString &title,
                 const QString &question,
                 bool defaultYes = false);
    void messageBox(const QString &title, const QStringList &lines);
    void scrollablePopup(const QString &title, const QStringList &lines);

    static QString takeArgument(QString &rest);
    static bool parseYesNo(const QString &value, bool defaultValue);
    static quint16 parsePort(const QString &value, quint16 fallback);

    void moveHistory(int direction);
    void requestQuit();

    bool m_cursesActive = false;
    bool m_quitting = false;
    QTimer m_tickTimer;

    QList<ConnectionEntry *> m_connections;
    QHash<QString, ConnectionEntry *> m_connectionById;
    QString m_selectedConnectionId;

    QList<Buffer *> m_buffers;
    QHash<QString, Buffer *> m_bufferByKey;
    QSet<QString> m_hiddenConnectionBuffers;
    QSet<QString> m_closedChatBuffers;
    int m_activeBuffer = 0;

    QString m_input;
    int m_inputCursor = 0;
    QStringList m_history;
    int m_historyPos = 0;

    QStringList m_commandCompletionMatches;
    int m_commandCompletionIndex = -1;
    QString m_completionMode;
    int m_completionStart = -1;

    UiOptions m_options;
    SecureChannelManager m_secure;
    SecureRoomManager m_secureRooms;
    bool m_secureReady = false;
    QSet<QString> m_outgoingSecureFrames;
    QSet<QString> m_outgoingUnsecuredFileFrames;
    QHash<QString, QSet<QString>> m_pendingSecureRoomKeys;
    QSet<QString> m_pendingVersionQueries;
    CpxFileTransferManager m_fileTransfers;
    CpxDirectTransferManager m_directTransfers;
    SipController *m_sipController = nullptr;
    MediaController *m_mediaController = nullptr;
    QHash<QString, QString> m_fileTransferProfiles;
    QHash<QString, bool> m_fileTransferSecure;
    QHash<QString, int> m_fileTransferProgressShown;
    qint64 m_nextFilePumpMs = 0;
    qint64 m_lastUserActivityMs = 0;
    qint64 m_nextPresenceCheckMs = 0;
    qint64 m_nextGeometryCheckMs = 0;
    int m_lastTerminalRows = 0;
    int m_lastTerminalCols = 0;
};
