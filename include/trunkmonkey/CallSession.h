#pragma once
#include "trunkmonkey/CallSnapshot.h"
#include "trunkmonkey/SipTrace.h"
#include <pjsua2.hpp>
#include <functional>
#include <fstream>
#include <mutex>
#include <memory>
#include <string>
#include <vector>
namespace trunkmonkey {
class Logger;
class CallSession final : public pj::Call, public std::enable_shared_from_this<CallSession> {
public:
    using UpdateCallback=std::function<void(int)>;
    CallSession(pj::Account& account,Logger& logger,CallDirection direction,CallPurpose purpose=CallPurpose::Phone,int callId=PJSUA_INVALID_ID);
    void setUpdateCallback(UpdateCallback cb);
    void setRequestedCallerId(std::string cid);
    void setAccountIdentity(std::string accountId,std::string accountName);
    CallSnapshot snapshot()const;
    void refreshMediaInfo();
    void setForeground(bool enabled);
    bool isForeground()const;
    void answerCall();
    void rejectCall(int code=603);
    void hangupCall();
    void holdCall();
    void resumeCall();
    void sendDtmfDigits(const std::string& digits,unsigned durationMs=0);
    void setMicrophoneMuted(bool muted);
    void attachAudio();
    void detachAudio();
    std::string mediaDump();

    void recordSipMessage(SipTraceEntry entry);
    std::vector<SipTraceEntry> sipTrace()const;
    void startSipTraceFile(const std::string& path);
    void stopSipTraceFile();
    bool sipTraceRecording()const;
    std::string sipTracePath()const;

    void onCallState(pj::OnCallStateParam& prm)override;
    void onCallMediaState(pj::OnCallMediaStateParam& prm)override;
    void onDtmfDigit(pj::OnDtmfDigitParam& prm)override;
private:
    static std::uint64_t nowMs();
    static std::string formatTraceEntry(const SipTraceEntry& entry);
    void notify();
    void syncSnapshot(const pj::CallInfo& info);
    Logger& logger_;
    mutable std::mutex mutex_;
    CallSnapshot snapshot_;
    UpdateCallback updateCallback_;
    bool foreground_{false};
    bool microphoneMuted_{false};
    std::vector<SipTraceEntry> sipTrace_;
    SipTraceClassifierState traceClassifier_;
    std::ofstream sipTraceFile_;
    std::string sipTracePath_;
};
}
