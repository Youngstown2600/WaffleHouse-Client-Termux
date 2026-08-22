#pragma once

#include "backend.h"
#include "trunkmonkey/Profile.h"

#include <QPointer>

class SipController;

trunkmonkey::SipProfile sipProfileFromConnectionSettings(const ConnectionSettings &settings);
void applySipProfileToConnectionSettings(const trunkmonkey::SipProfile &profile,
                                         ConnectionSettings &settings);

class SipBackend final : public ChatBackend {
    Q_OBJECT
public:
    SipBackend(ConnectionSettings settings, SipController *controller, QObject *parent = nullptr);
    ~SipBackend() override;

    QString protocolName() const override { return QStringLiteral("SIP"); }
    void start() override;
    void stop() override;
    void setConnectionSettings(const ConnectionSettings &settings) override;
    bool initializeAccount(QString *error = nullptr);
    void clearSessionPassword();

    void sendPrivateMessage(const QString &, const QString &) override;
    void joinRoom(const QString &, bool = false) override;
    void sendRoomMessage(const QString &, const QString &) override;
    void leaveRoom(const QString &) override;
    void addBuddy(const QString &name) override;
    void removeBuddy(const QString &name) override;

    QString sipAccountId() const { return id(); }

private:
    void syncState();
    QPointer<SipController> m_controller;
    bool m_connectRequested = false;
    bool m_reportedConnected = false;
    bool m_accountInitialized = false;
};
