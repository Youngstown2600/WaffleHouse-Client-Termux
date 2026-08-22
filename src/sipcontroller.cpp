#include "sipcontroller.h"

#include "trunkmonkey/Logger.h"
#include "trunkmonkey/RuntimePaths.h"
#include "trunkmonkey/SipEngine.h"
#include "trunkmonkey/SipTrace.h"

#include <QDateTime>
#include <QSettings>

#include <algorithm>
#include <exception>
#include <utility>

using trunkmonkey::CallDirection;
using trunkmonkey::CallSnapshot;
using trunkmonkey::SipDirection;
using trunkmonkey::SipProfile;

namespace {
QString q(const std::string &v) { return QString::fromStdString(v); }
std::string s(const QString &v) { return v.toStdString(); }
QString directionName(CallDirection d) { return d == CallDirection::Incoming ? QStringLiteral("IN") : QStringLiteral("OUT"); }
QString pjsipErrorText(const pj::Error &e) {
    const QString info = QString::fromStdString(e.info());
    return info.isEmpty() ? QStringLiteral("PJSUA2 reported an error without diagnostic text.") : info;
}
}

SipController::SipController(QObject *parent)
    : QObject(parent)
{
    loadSettings();
    connect(&m_pollTimer, &QTimer::timeout, this, &SipController::poll);
    m_pollTimer.setInterval(300);
}

SipController::~SipController()
{
    m_pollTimer.stop();
    if (m_engine) { try { m_engine->stop(); } catch (...) {} }
}

void SipController::initialize()
{
    if (m_initialized) return;
    m_initialized = true;
    m_initializationError.clear();
    try {
        trunkmonkey::runtime::ensureUserDirectories();
        m_logger = std::make_unique<trunkmonkey::Logger>(trunkmonkey::runtime::logPath().string());
        m_logger->setConsoleEnabled(false);
        m_engine = std::make_unique<trunkmonkey::SipEngine>(*m_logger);
        std::vector<std::pair<std::string,SipProfile>> initialAccounts;
        initialAccounts.reserve(static_cast<std::size_t>(m_profiles.size()));
        for (auto it = m_profiles.constBegin(); it != m_profiles.constEnd(); ++it)
            initialAccounts.push_back({s(it.key()), it.value()});
        m_engine->start(initialAccounts, 50);
        m_engine->setAudioAutoSwitch(m_audioAutoSwitch);
        appendActivity(QStringLiteral("Softphone endpoint ready for %1 SIP account(s).").arg(m_profiles.size()));
    } catch (const pj::Error &e) {
        m_initializationError = pjsipErrorText(e);
        if (m_logger) m_logger->error("Softphone initialization failed: " + s(m_initializationError));
        appendActivity(QStringLiteral("Softphone initialization failed: %1").arg(m_initializationError));
    } catch (const std::exception &e) {
        m_initializationError = QString::fromLocal8Bit(e.what());
        if (m_logger) m_logger->error("Softphone initialization failed: " + s(m_initializationError));
        appendActivity(QStringLiteral("Softphone initialization failed: %1").arg(m_initializationError));
    } catch (...) {
        m_initializationError = QStringLiteral("Unknown PJSUA2 initialization error.");
        if (m_logger) m_logger->error("Softphone initialization failed with an unknown PJSUA2 error.");
        appendActivity(QStringLiteral("Softphone initialization failed: %1").arg(m_initializationError));
    }
    m_pollTimer.start();
    emit stateChanged();
}

void SipController::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("softphone/global"));
    m_audioAutoSwitch = settings.value(QStringLiteral("audioAutoSwitch"), true).toBool();
    m_selectedAccountId = settings.value(QStringLiteral("selectedAccountId")).toString();
    settings.endGroup();
}

void SipController::saveSettings() const
{
    QSettings settings;
    settings.beginGroup(QStringLiteral("softphone/global"));
    settings.setValue(QStringLiteral("audioAutoSwitch"), m_audioAutoSwitch);
    settings.setValue(QStringLiteral("selectedAccountId"), m_selectedAccountId);
    settings.endGroup();
    settings.sync();
}

bool SipController::addAccount(const QString &accountId, const SipProfile &profile, QString *error)
{
    const QString id = accountId.trimmed();
    if (id.isEmpty()) { if (error) *error = QStringLiteral("SIP account ID is empty."); return false; }
    if (!m_profiles.contains(id) && m_profiles.size() >= 32) {
        if (error) *error = QStringLiteral("WaffleHouse supports up to 32 SIP accounts in one process.");
        return false;
    }

    bool insertedIntoEngine = false;
    const QString previousSelection = m_selectedAccountId;
    try {
        trunkmonkey::ProfileStore::validate(profile);
        if (m_profiles.contains(id)) {
            if (error) *error = QStringLiteral("SIP account already exists.");
            return false;
        }
        const bool hadValidSelection = m_profiles.contains(m_selectedAccountId);
        m_profiles.insert(id, profile);
        if (!hadValidSelection) m_selectedAccountId = id;
        if (m_initialized) {
            if (!m_engine) {
                const QString detail = m_initializationError.isEmpty()
                    ? QStringLiteral("SIP engine unavailable")
                    : QStringLiteral("SIP engine unavailable: %1").arg(m_initializationError);
                throw std::runtime_error(s(detail));
            }
            if (!m_engine->started()) {
                std::vector<std::pair<std::string,SipProfile>> list;
                for (auto it=m_profiles.constBegin(); it!=m_profiles.constEnd(); ++it)
                    list.push_back({s(it.key()), it.value()});
                m_engine->start(list, 50);
                insertedIntoEngine = true;
            } else {
                m_engine->addAccount(s(id), profile, false);
                insertedIntoEngine = true;
            }
        }
        saveSettings();
        appendActivity(QStringLiteral("SIP account added: %1 (%2@%3)")
                           .arg(q(profile.name), q(profile.username), q(profile.sipDomain)));
        emit accountsChanged();
        emit stateChanged();
        return true;
    } catch (const pj::Error &e) {
        if (insertedIntoEngine && m_engine && m_engine->started()) {
            try { m_engine->removeAccount(s(id)); } catch (...) {}
        }
        m_profiles.remove(id);
        m_selectedAccountId = previousSelection;
        const QString message = pjsipErrorText(e);
        if (error) *error = message;
        appendActivity(QStringLiteral("Unable to add SIP account %1: %2").arg(id, message));
        return false;
    } catch (const std::exception &e) {
        if (insertedIntoEngine && m_engine && m_engine->started()) {
            try { m_engine->removeAccount(s(id)); } catch (...) {}
        }
        m_profiles.remove(id);
        m_selectedAccountId = previousSelection;
        if (error) *error = QString::fromLocal8Bit(e.what());
        appendActivity(QStringLiteral("Unable to add SIP account %1: %2")
                           .arg(id, QString::fromLocal8Bit(e.what())));
        return false;
    } catch (...) {
        if (insertedIntoEngine && m_engine && m_engine->started()) {
            try { m_engine->removeAccount(s(id)); } catch (...) {}
        }
        m_profiles.remove(id);
        m_selectedAccountId = previousSelection;
        const QString message = QStringLiteral("Unknown SIP/PJSUA2 error while adding the account.");
        if (error) *error = message;
        appendActivity(QStringLiteral("Unable to add SIP account %1: %2").arg(id, message));
        return false;
    }
}

bool SipController::updateAccount(const QString &accountId, const SipProfile &profile, QString *error)
{
    const QString id = accountId.trimmed();
    try {
        trunkmonkey::ProfileStore::validate(profile);
        if (!m_engine || !m_engine->started()) { if (!startEngine(error)) return false; }
        if (m_profiles.contains(id)) m_engine->updateAccount(s(id), profile, true);
        else m_engine->addAccount(s(id), profile, false);
        m_profiles[id] = profile;
        appendActivity(QStringLiteral("SIP account updated: %1").arg(q(profile.name)));
        emit accountsChanged(); emit accountStateChanged(id); emit stateChanged();
        return true;
    } catch (const pj::Error &e) {
        if (error) *error = pjsipErrorText(e);
        return false;
    } catch (const std::exception &e) {
        if (error) *error = QString::fromLocal8Bit(e.what());
        return false;
    } catch (...) {
        if (error) *error = QStringLiteral("Unknown SIP/PJSUA2 error while updating the account.");
        return false;
    }
}

void SipController::removeAccount(const QString &accountId)
{
    const QString id = accountId.trimmed();
    if (id.isEmpty()) return;
    try { if (m_engine && m_engine->started()) m_engine->removeAccount(s(id)); } catch (const std::exception &e) {
        appendActivity(QStringLiteral("Unable to remove SIP account %1: %2").arg(id, QString::fromLocal8Bit(e.what())));
        return;
    }
    m_profiles.remove(id); m_registrationWanted.remove(id); m_lastRegistrationStates.remove(id);
    if (m_selectedAccountId == id) m_selectedAccountId = m_profiles.isEmpty() ? QString() : m_profiles.constBegin().key();
    saveSettings();
    appendActivity(QStringLiteral("SIP account removed: %1").arg(id));
    emit accountsChanged(); emit stateChanged();
}

SipProfile SipController::accountProfile(const QString &accountId, bool *ok) const
{
    if (ok) *ok = false;
    const auto it = m_profiles.constFind(accountId);
    if (it == m_profiles.constEnd()) return {};
    if (ok) *ok = true;
    return it.value();
}

QList<SipAccountView> SipController::accounts() const
{
    QList<SipAccountView> out;
    if (!m_engine || !m_engine->started()) {
        for (auto it = m_profiles.constBegin(); it != m_profiles.constEnd(); ++it) {
            SipAccountView v; v.id = it.key(); v.name = q(it.value().name); v.identity = q(it.value().username) + QStringLiteral("@") + q(it.value().sipDomain); v.registrationText = QStringLiteral("Endpoint stopped"); out.push_back(v);
        }
        return out;
    }
    try {
        for (const auto &st : m_engine->accountStatuses()) {
            SipAccountView v; v.id=q(st.id); v.name=q(st.name); v.identity=q(st.identity); v.registered=st.registered; v.registrationEnabled=st.registrationEnabled; v.registrationText=q(st.registrationText); out.push_back(v);
        }
    } catch (...) {}
    std::sort(out.begin(), out.end(), [](const SipAccountView &a, const SipAccountView &b){ return a.name.compare(b.name, Qt::CaseInsensitive) < 0; });
    return out;
}

QStringList SipController::accountIds() const
{
    QStringList out = m_profiles.keys(); out.sort(Qt::CaseInsensitive); return out;
}

bool SipController::hasAccount(const QString &accountId) const { return m_profiles.contains(accountId); }

void SipController::setSelectedAccountId(const QString &accountId)
{
    if (!accountId.isEmpty() && !m_profiles.contains(accountId)) return;
    if (m_selectedAccountId == accountId) return;
    m_selectedAccountId = accountId; saveSettings(); emit stateChanged();
}

bool SipController::started() const { return m_engine && m_engine->started(); }
bool SipController::registered() const { return m_engine && m_engine->registered(); }
bool SipController::accountRegistered(const QString &id) const { try { return m_engine && m_engine->accountRegistered(s(id)); } catch (...) { return false; } }
bool SipController::accountRegistrationEnabled(const QString &id) const { try { return m_engine && m_engine->accountRegistrationEnabled(s(id)); } catch (...) { return false; } }
QString SipController::registrationText() const { return m_engine ? q(m_engine->registrationText()) : QStringLiteral("Stopped"); }
QString SipController::registrationText(const QString &id) const { try { return m_engine ? q(m_engine->accountRegistrationText(s(id))) : QStringLiteral("Stopped"); } catch (...) { return QStringLiteral("Unknown account"); } }
QString SipController::engineLogPath() const { return q(trunkmonkey::runtime::pjsipLogPath().string()); }

bool SipController::startEngine(QString *error)
{
    try {
        if (!m_initialized) initialize();
        if (!m_engine) {
                const QString detail = m_initializationError.isEmpty()
                    ? QStringLiteral("SIP engine unavailable")
                    : QStringLiteral("SIP engine unavailable: %1").arg(m_initializationError);
                throw std::runtime_error(s(detail));
            }
        if (!m_engine->started()) {
            std::vector<std::pair<std::string,SipProfile>> list;
            for (auto it=m_profiles.constBegin(); it!=m_profiles.constEnd(); ++it) list.push_back({s(it.key()), it.value()});
            m_engine->start(list, 50);
            m_engine->setAudioAutoSwitch(m_audioAutoSwitch);
        }
        for (const QString &id : std::as_const(m_registrationWanted)) {
            try { m_engine->setAccountRegistration(s(id), true); } catch (...) {}
        }
        emit stateChanged(); return true;
    } catch (const pj::Error &e) {
        const QString message = pjsipErrorText(e);
        if (error) *error = message;
        appendActivity(QStringLiteral("SIP endpoint start failed: %1").arg(message));
        return false;
    } catch (const std::exception &e) {
        if (error) *error = QString::fromLocal8Bit(e.what());
        appendActivity(QStringLiteral("SIP endpoint start failed: %1").arg(QString::fromLocal8Bit(e.what())));
        return false;
    } catch (...) {
        const QString message = QStringLiteral("Unknown SIP/PJSUA2 error while starting the endpoint.");
        if (error) *error = message;
        appendActivity(message);
        return false;
    }
}

void SipController::stopEngine()
{
    if (!m_engine || !m_engine->started()) return;
    try { m_engine->stop(); } catch (...) {}
    m_seenCalls.clear(); m_lastCallStates.clear(); m_lastRegistrationStates.clear();
    appendActivity(QStringLiteral("SIP endpoint stopped; all SIP registrations are offline."));
    emit stateChanged(); emit accountsChanged(); emit callsChanged();
}

bool SipController::restartEngine(QString *error) { stopEngine(); return startEngine(error); }

bool SipController::connectAccount(const QString &accountId, QString *error)
{
    const QString id=accountId.trimmed();
    if (!m_profiles.contains(id)) { if (error) *error=QStringLiteral("Unknown SIP account."); return false; }
    if (!started() && !startEngine(error)) return false;
    try {
        m_registrationWanted.insert(id);
        m_engine->setAccountRegistration(s(id), true);
        if (m_selectedAccountId.isEmpty()) m_selectedAccountId=id;
        appendActivity(QStringLiteral("Registering SIP account %1").arg(id));
        emit accountStateChanged(id); emit stateChanged(); return true;
    } catch (const pj::Error &e) {
        if (error) *error=pjsipErrorText(e);
        return false;
    } catch (const std::exception &e) {
        if (error) *error=QString::fromLocal8Bit(e.what());
        return false;
    } catch (...) {
        if (error) *error=QStringLiteral("Unknown SIP/PJSUA2 error while registering the account.");
        return false;
    }
}

bool SipController::disconnectAccount(const QString &accountId, QString *error)
{
    const QString id=accountId.trimmed();
    try {
        m_registrationWanted.remove(id);
        if (m_engine && m_engine->started()) m_engine->setAccountRegistration(s(id), false);
        appendActivity(QStringLiteral("SIP account offline: %1").arg(id));
        emit accountStateChanged(id); emit stateChanged(); return true;
    } catch (const pj::Error &e) {
        // pjsua_acc_set_registration(false) may return PJ_EINVALIDOP after a
        // registration transaction has already timed out.  Disconnect is
        // idempotent from the user's perspective: the account is already
        // offline, so do not surface a scary secondary error.
        if (e.status == PJ_EINVALIDOP) {
            appendActivity(QStringLiteral("SIP account already offline: %1").arg(id));
            emit accountStateChanged(id); emit stateChanged();
            return true;
        }
        if (error) *error=pjsipErrorText(e);
        return false;
    } catch (const std::exception &e) {
        if (error) *error=QString::fromLocal8Bit(e.what());
        return false;
    } catch (...) {
        if (error) *error=QStringLiteral("Unknown SIP/PJSUA2 error while disconnecting the account.");
        return false;
    }
}

int SipController::dial(const QString &destination, const QString &callerId, QString *error, bool applyDialPrefix)
{
    return dial(m_selectedAccountId, destination, callerId, error, applyDialPrefix);
}

int SipController::dial(const QString &accountId, const QString &destination, const QString &callerId, QString *error, bool applyDialPrefix)
{
    const QString id = accountId.isEmpty() ? m_selectedAccountId : accountId;
    if (id.isEmpty()) { if (error) *error=QStringLiteral("Select a SIP account first."); return -1; }
    if (!started() && !startEngine(error)) return -1;
    try {
        const int callId=m_engine->makeCall(s(id),s(destination.trimmed()),s(callerId.trimmed()),true,trunkmonkey::CallPurpose::Phone,applyDialPrefix);
        const QString effective=q(m_engine->normalizeDestination(s(id),s(destination.trimmed()),applyDialPrefix));
        appendActivity(QStringLiteral("[%1] Dialing %2 (call %3)%4%5")
                           .arg(id, effective).arg(callId)
                           .arg(callerId.trimmed().isEmpty()?QString():QStringLiteral(" CID=%1").arg(callerId.trimmed()))
                           .arg(applyDialPrefix?QString():QStringLiteral(" [prefix bypassed]")));
        emit callsChanged(); return callId;
    } catch (const pj::Error &e) { const QString message=pjsipErrorText(e); if(error)*error=message; appendActivity(QStringLiteral("Dial failed: %1").arg(message)); return -1; }
    catch (const std::exception &e) { if(error)*error=QString::fromLocal8Bit(e.what()); appendActivity(QStringLiteral("Dial failed: %1").arg(QString::fromLocal8Bit(e.what()))); return -1; }
}

QString SipController::dialPrefix(const QString &accountId) const
{
    const QString id=accountId.isEmpty()?m_selectedAccountId:accountId;
    if(id.isEmpty()) return {};
    try {
        if(m_engine && m_engine->started()) return q(m_engine->dialPrefix(s(id)));
        const auto it=m_profiles.constFind(id);
        return it==m_profiles.constEnd()?QString():q(it.value().dialPrefix);
    } catch (...) { return {}; }
}

bool SipController::setDialPrefix(const QString &accountId,const QString &prefix,QString *error)
{
    const QString id=accountId.isEmpty()?m_selectedAccountId:accountId;
    if(id.isEmpty()){if(error)*error=QStringLiteral("Select a SIP account first.");return false;}
    if(!started() && !startEngine(error)) return false;
    try {
        m_engine->setDialPrefix(s(id),s(prefix.trimmed()));
        appendActivity(QStringLiteral("[%1] Runtime dial prefix: %2").arg(id,prefix.trimmed().isEmpty()?QStringLiteral("<none>"):prefix.trimmed()));
        emit accountStateChanged(id); emit stateChanged();
        return true;
    } catch(const std::exception&e){if(error)*error=QString::fromLocal8Bit(e.what());return false;}
}

QString SipController::dialPreview(const QString &accountId,const QString &destination,bool applyDialPrefix,QString *error) const
{
    const QString id=accountId.isEmpty()?m_selectedAccountId:accountId;
    if(id.isEmpty()){if(error)*error=QStringLiteral("Select a SIP account first.");return {};}
    try {
        if(!m_engine || !m_engine->started()) throw std::runtime_error("SIP endpoint is not running");
        return q(m_engine->normalizeDestination(s(id),s(destination.trimmed()),applyDialPrefix));
    } catch(const std::exception&e){if(error)*error=QString::fromLocal8Bit(e.what());return {};}
}

#define SIP_ACTION(name,expr,activity) \
bool SipController::name(int id, QString *error) { if(!m_engine){if(error)*error=QStringLiteral("SIP engine is unavailable.");return false;} try{expr;appendActivity(QStringLiteral(activity).arg(id));emit callsChanged();return true;}catch(const std::exception&e){if(error)*error=QString::fromLocal8Bit(e.what());return false;} }
SIP_ACTION(answer,m_engine->answer(id),"Answered call %1")
SIP_ACTION(reject,m_engine->reject(id),"Rejected call %1")
SIP_ACTION(hangup,m_engine->hangup(id),"Hung up call %1")
SIP_ACTION(hold,m_engine->hold(id),"Held call %1")
SIP_ACTION(resume,m_engine->resume(id),"Resumed call %1")
SIP_ACTION(setForeground,m_engine->setForeground(id),"Foreground call set to %1")
#undef SIP_ACTION

bool SipController::sendDtmf(int id,const QString&digits,QString*error){if(!m_engine){if(error)*error=QStringLiteral("SIP engine unavailable.");return false;}try{m_engine->sendDtmf(id,s(digits));appendActivity(QStringLiteral("DTMF %1 -> call %2").arg(digits).arg(id));return true;}catch(const std::exception&e){if(error)*error=QString::fromLocal8Bit(e.what());return false;}}
bool SipController::setMuted(int id,bool muted,QString*error){if(!m_engine){if(error)*error=QStringLiteral("SIP engine unavailable.");return false;}try{m_engine->setMicrophoneMuted(id,muted);appendActivity(QStringLiteral("Call %1 microphone %2").arg(id).arg(muted?QStringLiteral("muted"):QStringLiteral("unmuted")));emit callsChanged();return true;}catch(const std::exception&e){if(error)*error=QString::fromLocal8Bit(e.what());return false;}}

std::vector<CallSnapshot> SipController::calls() const { if(!m_engine||!m_engine->started())return{};try{return m_engine->calls();}catch(...){return{};} }
CallSnapshot SipController::call(int id,bool*ok)const{if(ok)*ok=false;if(!m_engine)return{};try{auto r=m_engine->callSnapshot(id);if(ok)*ok=true;return r;}catch(...){return{};}}

QString SipController::formatTraceLine(const trunkmonkey::SipTraceEntry&e){const QString when=QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(e.timestampMs)).toString(QStringLiteral("HH:mm:ss.zzz"));const QString dir=e.direction==SipDirection::Sent?QStringLiteral(">>"):QStringLiteral("<<");QString label=q(e.label);if(label.isEmpty())label=q(e.method);return QStringLiteral("%1  %2  call=%3  %4").arg(when,dir).arg(e.callId).arg(label);}
QString SipController::sipLogText(int id)const{if(!m_engine)return QStringLiteral("SIP engine is not running.");struct Item{std::uint64_t ts;QString text;QString raw;};std::vector<Item>items;for(const auto&c:calls()){if(id>=0&&c.id!=id)continue;try{for(const auto&e:m_engine->sipTrace(c.id))items.push_back({e.timestampMs,formatTraceLine(e),q(e.rawMessage)});}catch(...){}}std::sort(items.begin(),items.end(),[](const Item&a,const Item&b){return a.ts<b.ts;});QStringList out;for(const auto&i:items){out<<i.text;if(!i.raw.trimmed().isEmpty())for(const QString&line:i.raw.trimmed().split('\n'))out<<QStringLiteral("    %1").arg(line.trimmed());out<<QString();}return out.isEmpty()?QStringLiteral("No SIP messages observed for calls in this session."):out.join('\n');}
QString SipController::ladderText(int id)const{if(!m_engine||id<0)return QStringLiteral("Select a call to display its SIP ladder.");try{return q(m_engine->sipLadder(id));}catch(const std::exception&e){return QStringLiteral("Unable to build SIP ladder: %1").arg(QString::fromLocal8Bit(e.what()));}}

QString SipController::audioSummary()const{if(!m_engine||!m_engine->started())return QStringLiteral("Audio: SIP endpoint stopped");try{const auto a=m_engine->audioStatus();return QStringLiteral("Capture [%1] %2 | Playback [%3] %4 | Auto-switch %5 | Route %6").arg(a.captureId).arg(q(a.captureDevice)).arg(a.playbackId).arg(q(a.playbackDevice)).arg(a.autoSwitchEnabled?QStringLiteral("ON"):QStringLiteral("OFF")).arg(q(a.systemRoute));}catch(...){return QStringLiteral("Audio status unavailable");}}
QString SipController::audioDevicesText()const{if(!m_engine||!m_engine->started())return QStringLiteral("Start the SIP endpoint first.");try{QStringList lines;for(const auto&d:m_engine->audioDevices())lines<<QStringLiteral("[%1] %2 / %3  inputs=%4 outputs=%5").arg(d.id).arg(q(d.driver),q(d.name)).arg(d.inputCount).arg(d.outputCount);return lines.join('\n');}catch(const std::exception&e){return QString::fromLocal8Bit(e.what());}}
bool SipController::setAudioDevices(int captureId,int playbackId,QString*error){if(!m_engine||!m_engine->started()){if(error)*error=QStringLiteral("Start the SIP endpoint first.");return false;}try{m_engine->selectAudioDevices(captureId,playbackId);appendActivity(QStringLiteral("Audio devices selected: capture=%1 playback=%2").arg(captureId).arg(playbackId));emit stateChanged();return true;}catch(const std::exception&e){if(error)*error=QString::fromLocal8Bit(e.what());return false;}}
void SipController::setAudioAutoSwitch(bool enabled){m_audioAutoSwitch=enabled;saveSettings();if(m_engine)m_engine->setAudioAutoSwitch(enabled);appendActivity(QStringLiteral("Audio auto-switch %1").arg(enabled?QStringLiteral("enabled"):QStringLiteral("disabled")));emit stateChanged();}
bool SipController::audioAutoSwitch()const{return m_engine?m_engine->audioAutoSwitchEnabled():m_audioAutoSwitch;}

void SipController::appendActivity(const QString&text){const QString line=QStringLiteral("[%1] %2").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),text);m_activity.append(line);while(m_activity.size()>1500)m_activity.removeFirst();emit activityLine(line);emit activityChanged();}

void SipController::poll()
{
    if(!m_engine||!m_engine->started())return;
    try{m_engine->pollSystemAudioRoute();}catch(...){}

    for(const auto& account:accounts()){
        const QString state=QStringLiteral("%1|%2|%3").arg(account.registered).arg(account.registrationEnabled).arg(account.registrationText);
        if(m_lastRegistrationStates.value(account.id)!=state){
            m_lastRegistrationStates[account.id]=state;
            appendActivity(QStringLiteral("[%1] Registration: %2").arg(account.name.isEmpty()?account.id:account.name,account.registrationText));
            emit accountStateChanged(account.id); emit stateChanged();
        }
    }

    bool changed=false;int traceCount=0;
    for(const auto&c:calls()){
        const QString state=q(c.accountId)+QStringLiteral("|")+q(c.state)+QStringLiteral("|")+QString::number(c.connected)+QStringLiteral("|")+QString::number(c.disconnected)+QStringLiteral("|")+QString::number(c.microphoneMuted);
        if(!m_lastCallStates.contains(c.id)||m_lastCallStates.value(c.id)!=state){m_lastCallStates[c.id]=state;changed=true;appendActivity(QStringLiteral("[%1] Call %2 %3 %4 — %5").arg(q(c.accountName).isEmpty()?q(c.accountId):q(c.accountName)).arg(c.id).arg(directionName(c.direction),q(c.remoteUri),q(c.state)));}
        if(!m_seenCalls.contains(c.id)){m_seenCalls.insert(c.id);changed=true;if(c.direction==CallDirection::Incoming&&!c.disconnected)emit incomingCall(q(c.accountId),c.id,q(c.remoteUri));}
        try{traceCount+=static_cast<int>(m_engine->sipTrace(c.id).size());}catch(...){}
    }
    if(changed)emit callsChanged();
    if(traceCount!=m_lastTraceCount){m_lastTraceCount=traceCount;emit sipLogChanged();}
}
