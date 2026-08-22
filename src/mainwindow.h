#pragma once

#include "backend.h"
#include "securechannel.h"
#include "secureroom.h"
#include "filetransfer.h"
#include "directtransfer.h"

#include <QHash>
#include <QMainWindow>
#include <QSet>

class QAction;
class ChatWindow;
class TransferWindow;
class SoftphoneWindow;
class SipController;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QPlainTextEdit;
class QPushButton;
class QSystemTrayIcon;
class QTreeWidget;
class QTreeWidgetItem;
class QTimer;
class QCloseEvent;
class QEvent;
class QComboBox;
class QLineEdit;
class QPoint;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(const ConnectionSettings &defaults = {}, QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    struct UiOptions {
        QString theme = QStringLiteral("system");
        bool showTimestamps = true;
        bool showSidePanes = true;
        bool encryptedDmEnabled = true;
        bool autoReplySecure = true;
        bool showSecureFingerprints = true;
        bool autoPresenceEnabled = true;
        int autoIdleMinutes = 5;
        int autoAwayMinutes = 15;
    };

    struct BackendState {
        ChatBackend *backend = nullptr;
        QListWidgetItem *connectionItem = nullptr;
        QString profileId;
        QString identity;
        QString endpoint;
        bool connected = false;
        bool connecting = false;
        bool secretRequired = false;
        bool hasSessionSecret = false;
        QSet<QString> buddies;
        QSet<QString> onlineBuddies;
        QHash<QString, QString> targetNames;
        QHash<QString, QString> discoveredRooms;
        QString presenceState = QStringLiteral("ONLINE");
        QString presenceMessage;
        quint32 idleSeconds = 0;
        QString autoPresenceState;
    };

    void buildMenus();
    void buildUi();
    void buildConnectionsWindow();
    void buildTrayIcon();

    void importBbsList();
    void openConnectionDialog(const ConnectionSettings &defaults = {},
                              BackendState *editingState = nullptr);
    ChatBackend *createBackend(const ConnectionSettings &settings);
    void attachBackend(ChatBackend *backend,
                       bool persist = true,
                       bool secretRequired = false,
                       bool hasSessionSecret = false,
                       bool autoConnect = false,
                       const QString &profileId = QString());
    void wireBackend(ChatBackend *backend);

    void loadConnections();
    void saveConnections() const;
    void loadUiSettings();
    void saveUiSettings() const;
    void loadOptions();
    void saveOptions() const;
    void applyTheme();
    void applyConversationOptions();
    void showOptionsDialog();
    void showHelpDialog();
    bool ensureConnectionSecret(BackendState *state);

    BackendState *stateFor(ChatBackend *backend) const;
    BackendState *stateById(const QString &id) const;
    BackendState *selectedState() const;
    BackendState *stateFromBuddyItem(QTreeWidgetItem *item) const;
    void selectState(BackendState *state);

    void updateConnectionItem(BackendState *state);
    void updateActions();
    void refreshBuddyList();
    void refreshSoftphoneControls();
    void appendActivity(ChatBackend *backend, const QString &text);
    void rebuildTrayMenu();
    void rebuildAccountsMenu();
    QString accountMenuLabel(BackendState *state) const;
    void showAccountContextMenu(BackendState *state, const QPoint &globalPos);
    void openMessagingDialog(BackendState *state, const QString &presetTarget = QString(), bool startRoomTab = false);
    void openBuddyManager(BackendState *state);

    QString conversationKey(ChatBackend *backend,
                            const QString &kind,
                            const QString &target) const;
    QString targetDisplayName(BackendState *state,
                              const QString &kind,
                              const QString &target) const;
    QString conversationOpacityKey(ChatBackend *backend,
                                   const QString &kind,
                                   const QString &target) const;
    ChatWindow *ensureConversationWindow(ChatBackend *backend,
                                         const QString &kind,
                                         const QString &target,
                                         bool showWindow = true);
    void closeBackendWindows(ChatBackend *backend);

    void connectState(BackendState *state);
    void connectSelected();
    void disconnectSelected();
    void deleteSelected();
    void editSelected();

    void newIm();
    void joinRoom();
    void addBuddy();
    void removeBuddy();
    void changePassword();
    void setAimPresence(BackendState *state);
    void markUserActivity();
    void updateAutoPresence();
    void requestClientVersion(BackendState *state, const QString &target);
    void changeIrcNick();
    void rawProtocolCommand();

    void setBuddyTransparency();
    void setConnectionsTransparency();
    void showConnectionsWindow();
    void showTransferWindow();
    void showBuddyWindow();
    void quitApplication();

    // Secure CPX3-compatible DM overlay.
    QString imPayload(const QString &text) const;
    QString imSpeakerPrefix(const QString &text) const;
    QString secureTrustKey(BackendState *state, const QString &target) const;
    QString trustedFingerprint(BackendState *state, const QString &target) const;
    void setTrustedFingerprint(BackendState *state,
                               const QString &target,
                               const QString &fingerprint);
    void clearTrustedFingerprint(BackendState *state, const QString &target);
    void startSecureSession(ChatWindow *window);
    void showSecureStatus(ChatWindow *window);
    void trustSecurePeer(ChatWindow *window);
    void untrustSecurePeer(ChatWindow *window);
    void closeSecureSession(ChatWindow *window);
    void startSecureRoom(ChatWindow *window);
    void showSecureRoomStatus(ChatWindow *window);
    void closeSecureRoom(ChatWindow *window);
    void distributeSecureRoomKey(BackendState *state, ChatWindow *window, const QString &peer);
    void distributeSecureRoomKeyToMembers(BackendState *state, ChatWindow *window);
    void flushPendingSecureRoomKeys(BackendState *state, const QString &peer);
    bool handleSecureRoomKeyOffer(BackendState *state, const QString &peer, const QString &plaintext);
    void showSelectedFingerprint();
    void sendFile(ChatWindow *window);
    bool sendSecureControlPayload(BackendState *state,
                                  const QString &target,
                                  const QString &plaintext);
    bool handleFileTransferPayload(BackendState *state,
                                   const QString &target,
                                   const QString &plaintext,
                                   ChatWindow *window,
                                   bool secureTransport = true);
    void pumpFileTransfers();
    void appendTransferProgress(const CpxFileTransferManager::Event &event,
                                const QString &direction,
                                const QString &peer);
    void logTransfer(const QString &message, bool showWindow = true);
    void refreshTransferWindow(const QString &transferId,
                               const QString &direction,
                               const QString &peer,
                               const QString &status = QString());
    void cancelFileTransfer(const QString &transferId);
    void resumeFileTransfer(const QString &transferId);
    void clearFileTransfer(const QString &transferId);
    bool resumeIncomingFileTransfer(const QString &transferId, BackendState *state, const QString &peer);
    void startDirectOutgoing(const CpxFileTransferManager::Event &event, BackendState *state);
    void handleDirectProgress(const QString &transferId, qint64 transferred, qint64 total, bool outgoing);
    void handleDirectIncomingFinished(const QString &transferId);
    void handleDirectOutgoingFinished(const QString &transferId);
    void handleDirectFailure(const QString &transferId, const QString &reason, bool outgoing);
    bool sendPrivateText(BackendState *state,
                         const QString &target,
                         const QString &text,
                         ChatWindow *window);
    void handleConversationMessage(ChatWindow *window, const QString &text);
    void updateConversationSecurity(ChatWindow *window);

    void handleConnected(ChatBackend *backend,
                         const QString &identity,
                         const QString &endpoint);
    void handleDisconnected(ChatBackend *backend, const QString &reason);
    void handleEvent(ChatBackend *backend,
                     const QString &kind,
                     const QString &target,
                     const QString &text);
    void handleMembers(ChatBackend *backend,
                       const QString &room,
                       const QString &action,
                       const QStringList &names);
    void handleTargetNamed(ChatBackend *backend,
                           const QString &kind,
                           const QString &target,
                           const QString &displayName);
    void handleRoomDiscovered(ChatBackend *backend,
                              const QString &roomId,
                              const QString &displayName);
    void handleBuddyList(ChatBackend *backend, const QStringList &names);
    void handleBuddyPresence(ChatBackend *backend,
                             const QString &name,
                             bool online);
    void handleBackendError(ChatBackend *backend,
                            const QString &context,
                            const QString &message);
    void handleConversationClosing(ChatWindow *window);

    ConnectionSettings m_defaults;

    // Main Buddy List window.
    QTreeWidget *m_buddyTree = nullptr;
    QComboBox *m_buddySipAccount = nullptr;
    QLineEdit *m_buddyDialPrefix = nullptr;
    QLineEdit *m_buddyDial = nullptr;
    QPushButton *m_buddyDialButton = nullptr;
    QPushButton *m_buddySipConnectButton = nullptr;
    QPushButton *m_buddySipDisconnectButton = nullptr;

    // Secondary Connections window.
    QMainWindow *m_connectionsWindow = nullptr;
    QListWidget *m_connectionList = nullptr;
    QPlainTextEdit *m_activity = nullptr;
    QPushButton *m_addConnectionButton = nullptr;
    QPushButton *m_editConnectionButton = nullptr;
    QPushButton *m_deleteConnectionButton = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_disconnectButton = nullptr;

    QMenu *m_accountsMenu = nullptr;

    QAction *m_addConnectionAction = nullptr;
    QAction *m_importBbsAction = nullptr;
    QAction *m_editConnectionAction = nullptr;
    QAction *m_deleteConnectionAction = nullptr;
    QAction *m_connectAction = nullptr;
    QAction *m_disconnectAction = nullptr;
    QAction *m_showConnectionsAction = nullptr;
    QAction *m_changePasswordAction = nullptr;
    QAction *m_rawAction = nullptr;
    QAction *m_buddyTransparencyAction = nullptr;
    QAction *m_connectionsTransparencyAction = nullptr;
    QAction *m_optionsAction = nullptr;
    QAction *m_transferWindowAction = nullptr;
    QAction *m_fingerprintAction = nullptr;
    QAction *m_phoneAction = nullptr;

    QSystemTrayIcon *m_trayIcon = nullptr;
    QMenu *m_trayMenu = nullptr;
    QAction *m_trayShowBuddyAction = nullptr;
    QAction *m_trayShowConnectionsAction = nullptr;
    QAction *m_trayShowPhoneAction = nullptr;
    QAction *m_trayConnectAction = nullptr;
    QAction *m_trayDisconnectAction = nullptr;

    double m_buddyOpacity = 1.0;
    double m_connectionsOpacity = 1.0;
    bool m_quitting = false;
    bool m_trayHintShown = false;

    UiOptions m_options;
    SecureChannelManager m_secure;
    SecureRoomManager m_secureRooms;
    bool m_secureReady = false;
    QString m_secureError;
    QSet<QString> m_outgoingSecureFrames;
    QSet<QString> m_outgoingUnsecuredFileFrames;
    QHash<QString, QSet<QString>> m_pendingSecureRoomKeys;
    QSet<QString> m_pendingVersionQueries;
    QSet<QString> m_closedRoomKeys;
    CpxFileTransferManager m_fileTransfers;
    CpxDirectTransferManager m_directTransfers;
    QHash<QString, QString> m_fileTransferProfiles;
    QHash<QString, bool> m_fileTransferSecure;
    QHash<QString, int> m_fileTransferProgressShown;
    QTimer *m_fileTransferTimer = nullptr;
    QTimer *m_presenceTimer = nullptr;
    qint64 m_lastUserActivityMs = 0;
    TransferWindow *m_transferWindow = nullptr;
    SipController *m_sipController = nullptr;
    SoftphoneWindow *m_softphoneWindow = nullptr;

    QHash<QString, BackendState *> m_states;
    QHash<QString, ChatWindow *> m_windows;
};
