#pragma once

#include <QMainWindow>
#include <QByteArray>
#include <QSet>
#include <QString>
#include <QStringList>

class ChatBackend;
class AnsiTerminalWidget;
class QLabel;
class QLineEdit;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QCloseEvent;
class QEvent;
class QObject;
class QResizeEvent;
class QWidget;

class ChatWindow : public QMainWindow {
    Q_OBJECT
public:
    ChatWindow(ChatBackend *backend,
               QString kind,
               QString target,
               QString displayName,
               QString opacitySettingsKey,
               QWidget *parent = nullptr);

    QString backendId() const;
    QString kind() const { return m_kind; }
    QString target() const { return m_target; }
    QString displayName() const { return m_displayName; }
    QStringList members() const { return m_memberSet.values(); }

    void appendMessage(const QString &text);
    void updateMembers(const QString &action, const QStringList &names);
    void setDisplayName(const QString &displayName);
    void setBackendOnline(bool online);
    void setShowTimestamps(bool enabled) { m_showTimestamps = enabled; }
    void setShowSidePane(bool enabled);
    void setSecurityState(bool active,
                          bool trusted,
                          const QString &peerFingerprint = QString(),
                          const QString &localFingerprint = QString());

signals:
    void conversationClosing(ChatWindow *window);
    void messageSubmitted(ChatWindow *window, const QString &message);
    void terminalBytesSubmitted(ChatWindow *window, const QByteArray &bytes);
    void secureRequested(ChatWindow *window);
    void secureStatusRequested(ChatWindow *window);
    void trustRequested(ChatWindow *window);
    void untrustRequested(ChatWindow *window);
    void secureOffRequested(ChatWindow *window);
    void fileSendRequested(ChatWindow *window);

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void sendMessage();
    void setTransparency();
    void opacitySliderChanged(int percent);

private:
    void buildUi();
    void buildMenus();
    void refreshTitle();
    void refreshMembers();
    void loadOpacity();
    void saveOpacity() const;
    void updateTerminalSize();
    QStringList availableSlashCommands() const;
    bool completeSlashCommand(int direction = 1);
    bool completeMemberName(int direction = 1);
    void resetCommandCompletion();
    void resetMemberCompletion();

    ChatBackend *m_backend = nullptr;
    QString m_kind;
    QString m_target;
    QString m_displayName;
    QString m_opacitySettingsKey;
    bool m_online = true;
    bool m_closeSignalSent = false;
    bool m_showTimestamps = true;
    bool m_showSidePane = true;
    bool m_secureActive = false;
    double m_opacity = 1.0;

    QLabel *m_heading = nullptr;
    QLabel *m_connectionLabel = nullptr;
    QLabel *m_securityLabel = nullptr;
    QPlainTextEdit *m_transcript = nullptr;
    AnsiTerminalWidget *m_terminal = nullptr;
    QWidget *m_memberPane = nullptr;
    QListWidget *m_members = nullptr;
    QLabel *m_membersTitle = nullptr;
    QLineEdit *m_messageEdit = nullptr;
    QPushButton *m_sendButton = nullptr;
    QPushButton *m_secureButton = nullptr;
    QPushButton *m_secureStatusButton = nullptr;
    QPushButton *m_secureCloseButton = nullptr;
    QPushButton *m_sendFileButton = nullptr;
    QSlider *m_opacitySlider = nullptr;
    QLabel *m_opacityValueLabel = nullptr;
    QSet<QString> m_memberSet;
    QStringList m_commandCompletionMatches;
    int m_commandCompletionIndex = -1;
    QStringList m_memberCompletionMatches;
    int m_memberCompletionIndex = -1;
    int m_memberCompletionStart = -1;
};
