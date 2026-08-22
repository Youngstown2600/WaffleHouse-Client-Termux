#pragma once

#include "trunkmonkey/CallSnapshot.h"
#include "trunkmonkey/Profile.h"
#include "trunkmonkey/SipTrace.h"

#include <QObject>
#include <QHash>
#include <QStringList>
#include <QSet>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

namespace trunkmonkey { class Logger; class SipEngine; }

struct SipAccountView {
    QString id;
    QString name;
    QString identity;
    bool registered = false;
    bool registrationEnabled = false;
    QString registrationText;
};

class SipController final : public QObject {
    Q_OBJECT
public:
    explicit SipController(QObject *parent = nullptr);
    ~SipController() override;

    void initialize();

    bool addAccount(const QString &accountId, const trunkmonkey::SipProfile &profile, QString *error = nullptr);
    bool updateAccount(const QString &accountId, const trunkmonkey::SipProfile &profile, QString *error = nullptr);
    void removeAccount(const QString &accountId);
    trunkmonkey::SipProfile accountProfile(const QString &accountId, bool *ok = nullptr) const;
    QList<SipAccountView> accounts() const;
    QStringList accountIds() const;
    bool hasAccount(const QString &accountId) const;

    QString selectedAccountId() const { return m_selectedAccountId; }
    void setSelectedAccountId(const QString &accountId);

    bool started() const;
    bool registered() const;
    bool accountRegistered(const QString &accountId) const;
    bool accountRegistrationEnabled(const QString &accountId) const;
    QString registrationText() const;
    QString registrationText(const QString &accountId) const;
    QString engineLogPath() const;

    bool startEngine(QString *error = nullptr);
    void stopEngine();
    bool restartEngine(QString *error = nullptr);
    bool connectAccount(const QString &accountId, QString *error = nullptr);
    bool disconnectAccount(const QString &accountId, QString *error = nullptr);

    int dial(const QString &destination, const QString &callerId = QString(), QString *error = nullptr, bool applyDialPrefix = true);
    int dial(const QString &accountId, const QString &destination, const QString &callerId = QString(), QString *error = nullptr, bool applyDialPrefix = true);
    QString dialPrefix(const QString &accountId = QString()) const;
    bool setDialPrefix(const QString &accountId, const QString &prefix, QString *error = nullptr);
    QString dialPreview(const QString &accountId, const QString &destination, bool applyDialPrefix = true, QString *error = nullptr) const;
    bool answer(int id, QString *error = nullptr);
    bool reject(int id, QString *error = nullptr);
    bool hangup(int id, QString *error = nullptr);
    bool hold(int id, QString *error = nullptr);
    bool resume(int id, QString *error = nullptr);
    bool sendDtmf(int id, const QString &digits, QString *error = nullptr);
    bool setMuted(int id, bool muted, QString *error = nullptr);
    bool setForeground(int id, QString *error = nullptr);

    std::vector<trunkmonkey::CallSnapshot> calls() const;
    trunkmonkey::CallSnapshot call(int id, bool *ok = nullptr) const;
    QString sipLogText(int id = -1) const;
    QString ladderText(int id) const;
    QString activityText() const { return m_activity.join('\n'); }

    QString audioSummary() const;
    QString audioDevicesText() const;
    bool setAudioDevices(int captureId, int playbackId, QString *error = nullptr);
    void setAudioAutoSwitch(bool enabled);
    bool audioAutoSwitch() const;

signals:
    void stateChanged();
    void accountsChanged();
    void accountStateChanged(const QString &accountId);
    void callsChanged();
    void sipLogChanged();
    void activityChanged();
    void activityLine(const QString &line);
    void incomingCall(const QString &accountId, int id, const QString &remoteUri);

private slots:
    void poll();

private:
    void loadSettings();
    void saveSettings() const;
    void appendActivity(const QString &text);
    static QString formatTraceLine(const trunkmonkey::SipTraceEntry &entry);

    bool m_audioAutoSwitch = true;
    std::unique_ptr<trunkmonkey::Logger> m_logger;
    std::unique_ptr<trunkmonkey::SipEngine> m_engine;
    QTimer m_pollTimer;
    QStringList m_activity;
    QHash<QString, trunkmonkey::SipProfile> m_profiles;
    QSet<QString> m_registrationWanted;
    QString m_selectedAccountId;
    QSet<int> m_seenCalls;
    QHash<int, QString> m_lastCallStates;
    QHash<QString, QString> m_lastRegistrationStates;
    int m_lastTraceCount = 0;
    bool m_initialized = false;
};
