#pragma once
#include "trunkmonkey/CallSnapshot.h"
#include "trunkmonkey/Profile.h"
#include "trunkmonkey/SipTrace.h"
#include <pjsua2.hpp>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
namespace trunkmonkey {
class CallSession; class Logger; class SipAccount; class SipWireMonitor;
struct AudioDeviceInfo { int id{-1}; std::string driver; std::string name; unsigned inputCount{0}; unsigned outputCount{0}; };
struct AudioStatusInfo {
    int captureId{-1};
    int playbackId{-1};
    bool soundActive{false};
    bool hotplugWatchAvailable{false};
    bool autoSwitchEnabled{true};
    std::string hotplugBackend;
    std::string captureDevice;
    std::string playbackDevice;
    std::string systemRoute;
};
struct SipAccountStatus {
    std::string id;
    std::string name;
    std::string identity;
    bool registered{false};
    bool registrationEnabled{false};
    std::string registrationText{"Offline"};
};
class SipEngine {
public:
    explicit SipEngine(Logger& logger); ~SipEngine();
    SipEngine(const SipEngine&)=delete; SipEngine& operator=(const SipEngine&)=delete;

    // Legacy single-account entry point plus the native WaffleHouse multi-account API.
    void start(const SipProfile& profile,unsigned maxCalls=50);
    void start(const std::vector<std::pair<std::string,SipProfile>>& accounts,unsigned maxCalls=50);
    void stop();
    bool started()const;
    bool registered()const; // true when any SIP account is registered
    std::string registrationText()const; // aggregate status
    const SipProfile& profile()const; // legacy default profile

    void addAccount(const std::string& accountId,const SipProfile& profile,bool registerNow=false);
    void updateAccount(const std::string& accountId,const SipProfile& profile,bool preserveRegistration=true);
    void removeAccount(const std::string& accountId);
    void setAccountRegistration(const std::string& accountId,bool enabled);
    bool accountRegistered(const std::string& accountId)const;
    bool accountRegistrationEnabled(const std::string& accountId)const;
    std::string accountRegistrationText(const std::string& accountId)const;
    SipProfile accountProfile(const std::string& accountId)const;
    std::vector<SipAccountStatus> accountStatuses()const;
    std::vector<std::string> accountIds()const;

    // Per-account runtime PBX dial prefix. Each account starts with its saved
    // profile dialPrefix, but the active value can be changed without rewriting
    // the saved WaffleHouse connection/profile.
    void setDialPrefix(const std::string& accountId,const std::string& prefix);
    std::string dialPrefix(const std::string& accountId)const;
    void setDialPrefix(const std::string& prefix); // legacy/default account
    std::string dialPrefix()const;                 // legacy/default account

    int makeCall(const std::string& destination,const std::string& callerId={},bool makeForeground=true,CallPurpose purpose=CallPurpose::Phone,bool applyDialPrefix=true);
    int makeCall(const std::string& accountId,const std::string& destination,const std::string& callerId={},bool makeForeground=true,CallPurpose purpose=CallPurpose::Phone,bool applyDialPrefix=true);
    void answer(int id); void reject(int id,int code=603); void hangup(int id); void hangupAll();
    void hold(int id); void resume(int id); void sendDtmf(int id,const std::string& digits,unsigned durationMs=0);
    void setMicrophoneMuted(int id,bool muted);
    std::vector<AudioDeviceInfo> audioDevices()const;
    int activeCaptureDevice()const; int activePlaybackDevice()const;
    void selectAudioDevices(int captureId,int playbackId);
    void selectPlaybackDevice(int playbackId);
    void refreshAudioDevices();
    void reopenAudioDevices();
    AudioStatusInfo audioStatus()const;
    bool pollSystemAudioRoute();
    void setAudioAutoSwitch(bool enabled);
    bool audioAutoSwitchEnabled() const;
    void setForeground(int id); void clearForeground();
    std::vector<CallSnapshot> calls()const;
    CallSnapshot callSnapshot(int id)const;
    std::string mediaDump(int id)const;
    std::string sipLadder(int id)const;
    std::string callReport(int id)const;
    void exportCallReport(int id,const std::string& path)const;

    std::vector<SipTraceEntry> sipTrace(int id)const;
    void startSipTraceFile(int id,const std::string& path);
    void stopSipTraceFile(int id);
    bool sipTraceRecording(int id)const;
    std::string sipTracePath(int id)const;

    std::string normalizeDestination(const std::string& value,bool applyDialPrefix=true)const;
    std::string normalizeDestination(const std::string& accountId,const std::string& value,bool applyDialPrefix=true)const;
    std::string callerIdentityUri(const std::string& value)const;
    std::string callerIdentityUri(const std::string& accountId,const std::string& value)const;
    void onIncomingCall(const std::string& accountId,int id);
    void onRegistrationState(const std::string& accountId,bool active,int code,const std::string& reason);
    void onSipMessage(SipTraceEntry entry);
private:
    struct ArchivedCall {
        CallSnapshot snapshot;
        std::vector<SipTraceEntry> sipTrace;
        std::string sipTracePath;
    };
    struct AccountState {
        SipProfile profile;
        std::unique_ptr<SipAccount> account;
        pj::TransportId transportId{PJSUA_INVALID_ID};
        bool registered{false};
        bool registrationEnabled{false};
        std::string registrationText{"Offline"};
        std::vector<std::string> registrationHistory;
        std::string dialPrefix;
    };
    std::shared_ptr<CallSession> findCall(int id)const;
    const ArchivedCall* findArchivedCallLocked(int id)const;
    std::shared_ptr<CallSession> requirePhoneCall(int id)const;
    bool addCall(const std::shared_ptr<CallSession>& call);
    void archiveDisconnectedCall(const std::shared_ptr<CallSession>& call,const CallSnapshot& state);
    void onCallUpdated(int id);
    void configureIdentity(const SipProfile& profile,pj::CallOpParam& prm,const std::string& callerId)const;
    std::string normalizeDestination(const SipProfile& profile,const std::string& value,bool applyDialPrefix)const;
    std::string callerIdentityUri(const SipProfile& profile,const std::string& value)const;
    SipProfile profileForAccount(const std::string& accountId)const;
    pj::TransportId ensureTransport(const SipProfile& profile);
    void createAccount(const std::string& accountId,const SipProfile& profile,bool registerNow);
    void refreshStunServers();
    void updateAggregateRegistration();
    bool accountHasLiveCalls(const std::string& accountId)const;

    Logger& logger_;
    mutable std::mutex mutex_;
    mutable std::mutex callCreateMutex_;
    mutable std::mutex audioMutex_;
    mutable std::mutex audioRouteMutex_;
    mutable std::mutex accountMutex_;
    SipProfile profile_; // default/legacy profile
    std::string defaultAccountId_;
    std::unique_ptr<pj::Endpoint> endpoint_;
    std::map<std::string,AccountState> accounts_;
    std::map<std::string,pj::TransportId> transports_;
    std::unique_ptr<SipWireMonitor> sipMonitor_;
    std::map<int,std::shared_ptr<CallSession>> calls_;
    std::map<int,ArchivedCall> archivedCalls_;
    std::map<std::string,int> callIdIndex_;
    std::map<std::string,std::vector<SipTraceEntry>> pendingSip_;
    int foregroundId_{-1};
    std::string lastSystemAudioRoute_;
    std::uint64_t lastSystemAudioPollMs_{0};
    bool systemAudioWatchUnavailableLogged_{false};
    std::atomic<bool> audioAutoSwitch_{true};
    std::atomic<bool> started_{false},registered_{false},stopping_{false};
    mutable std::mutex regMutex_;
    std::string registrationText_{"Stopped"};
    std::vector<std::string> registrationHistory_;
public:
    std::vector<std::string> registrationHistory()const;
};
}
