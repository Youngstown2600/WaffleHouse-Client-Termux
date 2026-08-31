#pragma once

#include "backend.h"

#include <QHash>
#include <QMutex>
#include <QQueue>
#include <QSet>
#include <QThread>

class QAbstractSocket;

class IrcBackend : public ChatBackend {
    Q_OBJECT
public:
    explicit IrcBackend(ConnectionSettings settings, QObject *parent = nullptr);
    ~IrcBackend() override;

    QString protocolName() const override { return QStringLiteral("IRC"); }

    void setConnectionSettings(const ConnectionSettings &settings) override;
    void start() override;
    void stop() override;
    void sendPrivateMessage(const QString &target, const QString &message) override;
    void joinRoom(const QString &room, bool privateRoom = false) override;
    void sendRoomMessage(const QString &room, const QString &message) override;
    void leaveRoom(const QString &room) override;
    void sendRaw(const QString &line,
                 const QString &unusedB = QString(),
                 const QString &unusedC = QString()) override;
    void changeNickname(const QString &newNick) override;
    void addBuddy(const QString &name) override;
    void removeBuddy(const QString &name) override;
    void requestClientVersion(const QString &target);
    void requestWhois(const QString &target);
    void refreshServerCapabilities();

    // Shared IRC slash-command parser used by both the Qt GUI and ncurses CLI.
    // Returns true only when INPUT is a recognized IRC command. Unknown slash
    // text intentionally returns false so the caller can send it as chat text.
    bool handleSlashCommand(const QString &contextTarget, const QString &input);
    static QStringList slashCommands();

signals:
    void serverCapabilitiesChanged(const QStringList &ircv3Capabilities,
                                   const QStringList &isupportTokens);
    void whoisReply(const QString &nick, const QString &line, bool complete);

private:
    enum class CommandType {
        SendIm,
        Join,
        SendRoom,
        Part,
        Raw,
        Nick,
        WatchAdd,
        WatchRemove,
    };

    struct Command {
        CommandType type;
        QString a;
        QString b;
    };

    struct ParsedLine {
        QString prefix;
        QString command;
        QStringList params;
    };

    void enqueue(Command command);
    QList<Command> takeCommands();
    void run();
    void processLine(const QString &line);

    void sendLine(QAbstractSocket &socket, const QString &line);
    QString readLine(QAbstractSocket &socket, int timeoutMs);
    static ParsedLine parseLine(const QString &line);
    static QString nickFromPrefix(const QString &prefix);
    static QString stripMemberPrefix(QString nick);
    static QString stripFormatting(const QString &text);
    static bool isChannel(const QString &target);
    static QString canonicalChannel(QString room);

    void addMembers(const QString &room, const QStringList &names);
    void removeMembers(const QString &room, const QStringList &names);
    void replaceMembers(const QString &room, const QStringList &names);
    void emitServerCapabilities();

    QThread *m_thread = nullptr;
    QMutex m_commandMutex;
    QQueue<Command> m_commands;

    QString m_nickname;
    QHash<QString, QSet<QString>> m_members;
    QHash<QString, QString> m_roomNames;
    QHash<QString, QSet<QString>> m_pendingNames;
    QSet<QString> m_watchBuddies;
    QSet<QString> m_onlineWatchBuddies;
    QSet<QString> m_ircv3Capabilities;
    QHash<QString, QString> m_isupportTokens;
};
