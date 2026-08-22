#pragma once

#include "backend.h"

#include <QMutex>
#include <QQueue>
#include <QThread>

#include <atomic>

class QTcpSocket;

class TelnetBackend : public ChatBackend {
    Q_OBJECT
public:
    explicit TelnetBackend(ConnectionSettings settings, QObject *parent = nullptr);
    ~TelnetBackend() override;

    QString protocolName() const override { return QStringLiteral("Telnet"); }

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
    void setTerminalSize(int columns, int rows) override;
    void sendTerminalInput(const QByteArray &bytes) override;

private:
    enum class CommandType {
        SendLine,
        RawLine,
        RawBytes,
        WindowSize,
    };

    struct Command {
        CommandType type;
        QString text;
        QByteArray bytes;
    };

    enum class TelnetState {
        Data,
        Iac,
        Negotiation,
        SubnegOption,
        SubnegData,
        SubnegIac,
    };

    void enqueue(Command command);
    QList<Command> takeCommands();
    void run();

    void writeAll(QTcpSocket &socket, const QByteArray &data);
    void sendLine(QTcpSocket &socket, const QString &text);
    void sendIac(QTcpSocket &socket, quint8 command, quint8 option);
    void sendSubneg(QTcpSocket &socket, quint8 option, const QByteArray &payload);
    void sendWindowSize(QTcpSocket &socket);
    void handleNegotiation(QTcpSocket &socket, quint8 command, quint8 option);
    void handleSubnegotiation(QTcpSocket &socket, quint8 option, const QByteArray &payload);
    void processBytes(QTcpSocket &socket, const QByteArray &bytes);
    QString decodeTerminalText(const QByteArray &bytes) const;

    QThread *m_thread = nullptr;
    QMutex m_commandMutex;
    QQueue<Command> m_commands;

    TelnetState m_telnetState = TelnetState::Data;
    quint8 m_pendingCommand = 0;
    quint8 m_subnegOption = 0;
    QByteArray m_subnegData;

    std::atomic_int m_columns{80};
    std::atomic_int m_rows{24};
    std::atomic_bool m_serverEcho{false};
    std::atomic_bool m_nawsEnabled{false};
};
