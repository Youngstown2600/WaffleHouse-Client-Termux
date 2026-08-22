#include "trunkmonkey/CallSession.h"
#include "trunkmonkey/Logger.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <utility>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace trunkmonkey {
namespace {
void protectFile(const std::string& path)
{
#ifndef _WIN32
    (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)path;
#endif
}


double clampValue(double v,double lo,double hi){return std::max(lo,std::min(hi,v));}

std::pair<double,double> estimateVoiceQuality(double rttMs,double jitterMs,double lossPercent)
{
    // Lightweight engineering estimate for quick triage, not a substitute for
    // a standards-certified PESQ/POLQA measurement. Delay and packet loss are
    // intentionally penalized conservatively so the dashboard highlights bad paths.
    const double oneWayDelay=std::max(0.0,rttMs/2.0+jitterMs);
    double delayImpairment=0.024*oneWayDelay;
    if(oneWayDelay>177.3)delayImpairment+=0.11*(oneWayDelay-177.3);
    const double lossImpairment=2.5*std::max(0.0,lossPercent);
    const double r=clampValue(93.2-delayImpairment-lossImpairment,0.0,100.0);
    double mos=1.0;
    if(r>0.0&&r<100.0)mos=1.0+0.035*r+r*(r-60.0)*(100.0-r)*0.000007;
    else if(r>=100.0)mos=4.5;
    return {r,clampValue(mos,1.0,4.5)};
}
}

CallSession::CallSession(pj::Account& account, Logger& logger, CallDirection direction,
                         CallPurpose purpose, int id)
    : pj::Call(account, id), logger_(logger)
{
    snapshot_.id = id;
    snapshot_.direction = direction;
    snapshot_.purpose = purpose;
    snapshot_.createdMs = nowMs();
    if (id != PJSUA_INVALID_ID) {
        try {
            syncSnapshot(getInfo());
            refreshMediaInfo();
        } catch (...) {
        }
    }
}

std::uint64_t CallSession::nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

void CallSession::setUpdateCallback(UpdateCallback cb)
{
    std::lock_guard<std::mutex> lock(mutex_);
    updateCallback_ = std::move(cb);
}

void CallSession::setRequestedCallerId(std::string callerId)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.callerId = std::move(callerId);
}

void CallSession::setAccountIdentity(std::string accountId,std::string accountName)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.accountId = std::move(accountId);
    snapshot_.accountName = std::move(accountName);
}

CallSnapshot CallSession::snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return snapshot_;
}

bool CallSession::isForeground() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return foreground_;
}

void CallSession::setForeground(bool enabled)
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        foreground_ = enabled;
        snapshot_.foreground = enabled;
    }
    if (enabled) {
        attachAudio();
    } else {
        detachAudio();
    }
    notify();
}

void CallSession::answerCall()
{
    pj::CallOpParam param(true);
    param.statusCode = PJSIP_SC_OK;
    answer(param);
}

void CallSession::rejectCall(int code)
{
    pj::CallOpParam param;
    param.statusCode = static_cast<pjsip_status_code>(code);
    answer(param);
}

void CallSession::hangupCall()
{
    pj::CallOpParam param;
    hangup(param);
}

void CallSession::holdCall()
{
    pj::CallOpParam param;
    setHold(param);
}

void CallSession::resumeCall()
{
    pj::CallOpParam param(true);
    param.opt.flag |= PJSUA_CALL_UNHOLD;
    reinvite(param);
}

void CallSession::sendDtmfDigits(const std::string& digits,unsigned durationMs)
{
    if(durationMs==0){
        dialDtmf(digits);
        return;
    }
    pj::CallSendDtmfParam param;
    param.method=PJSUA_DTMF_METHOD_RFC2833;
    param.digits=digits;
    param.duration=durationMs;
    pj::Call::sendDtmf(param);
}

void CallSession::setMicrophoneMuted(bool muted)
{
    bool foreground=false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        microphoneMuted_=muted;
        snapshot_.microphoneMuted=muted;
        foreground=foreground_;
    }
    if(!foreground){notify();return;}
    try{
        const auto info=getInfo();
        auto& audio=pj::Endpoint::instance().audDevManager();
        for(unsigned i=0;i<info.media.size();++i){
            if(info.media[i].type!=PJMEDIA_TYPE_AUDIO)continue;
            const auto status=info.media[i].status;
            if(status!=PJSUA_CALL_MEDIA_ACTIVE&&status!=PJSUA_CALL_MEDIA_REMOTE_HOLD)continue;
            auto media=getAudioMedia(static_cast<int>(i));
            if(muted){try{audio.getCaptureDevMedia().stopTransmit(media);}catch(...){}}
            else if(status==PJSUA_CALL_MEDIA_ACTIVE){audio.getCaptureDevMedia().startTransmit(media);}
            break;
        }
        logger_.info(std::string("Foreground microphone ")+(muted?"muted":"unmuted")+" for call "+std::to_string(getId()));
    }catch(const pj::Error&e){logger_.warn("Unable to change microphone mute state: "+e.info());}
    notify();
}


void CallSession::attachAudio()
{
    // Media objects may be replaced after a re-INVITE. Remove stale bridge
    // connections before attaching the current foreground stream.
    detachAudio();
    try {
        const auto info = getInfo();
        auto& audio = pj::Endpoint::instance().audDevManager();
#ifdef __ANDROID__
        bool speakerOnly = false;
        // Build 0.8 deliberately starts Termux on PJSIP's null sound device so
        // SIP signaling and RTP creation can never be vetoed by Android audio.
        // Once a call has negotiated audio, try full duplex. If OpenSL capture
        // cannot start (normally missing RECORD_AUDIO permission), keep the call
        // alive and fall back to playback-only rather than failing the INVITE.
        if (!audio.sndIsActive()) {
            try {
                audio.setNoDev();
                audio.setCaptureDev(PJMEDIA_AUD_DEFAULT_CAPTURE_DEV);
                audio.setPlaybackDev(PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV);
                audio.setSndDevMode(0);
                logger_.info("[AUDIO] Termux full-duplex sound device activated for call " + std::to_string(getId()));
            } catch (const pj::Error& fullDuplexError) {
                logger_.warn("[AUDIO] Termux microphone/full-duplex open failed; preserving call and trying speaker-only: " + fullDuplexError.info());
                try {
                    audio.setNoDev();
                    audio.setPlaybackDev(PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV);
                    audio.setSndDevMode(PJSUA_SND_DEV_SPEAKER_ONLY);
                    speakerOnly = true;
                    logger_.warn("[AUDIO] Termux call is playback-only. Install the matching Termux:API Android add-on and grant Microphone permission, then use /audio-reopen.");
                } catch (const pj::Error& playbackError) {
                    try { audio.setNullDev(); } catch (...) {}
                    logger_.warn("[AUDIO] Termux playback device also failed; RTP remains active with null audio: " + playbackError.info());
                    return;
                }
            }
        }
#endif
        for (unsigned i = 0; i < info.media.size(); ++i) {
            if (info.media[i].type != PJMEDIA_TYPE_AUDIO) {
                continue;
            }
            const auto status = info.media[i].status;
            if (status != PJSUA_CALL_MEDIA_ACTIVE && status != PJSUA_CALL_MEDIA_REMOTE_HOLD) {
                continue;
            }

            auto media = getAudioMedia(static_cast<int>(i));
            try {
                media.startTransmit(audio.getPlaybackDevMedia());
            } catch (const pj::Error& e) {
                logger_.warn("Unable to attach call playback audio: " + e.info());
            }
            bool muted=false;{std::lock_guard<std::mutex> lock(mutex_);muted=microphoneMuted_;}
            if (status == PJSUA_CALL_MEDIA_ACTIVE && !muted) {
#ifdef __ANDROID__
                if (!speakerOnly) {
#endif
                    try {
                        audio.getCaptureDevMedia().startTransmit(media);
                    } catch (const pj::Error& e) {
                        // Capture failure must never tear down an otherwise valid
                        // SIP/RTP call. The user may still receive audio and can
                        // recover the mic later with /audio-reopen.
                        logger_.warn("Unable to attach call capture audio; call remains active: " + e.info());
                    }
#ifdef __ANDROID__
                }
#endif
            }
            logger_.info("Audio attached to foreground call " + std::to_string(getId()));
            break;
        }
    } catch (const pj::Error& e) {
        logger_.warn("Unable to attach call audio: " + e.info());
    }
}

void CallSession::detachAudio()
{
    try {
        const auto info = getInfo();
        auto& audio = pj::Endpoint::instance().audDevManager();
        for (unsigned i = 0; i < info.media.size(); ++i) {
            if (info.media[i].type != PJMEDIA_TYPE_AUDIO) {
                continue;
            }
            try {
                auto media = getAudioMedia(static_cast<int>(i));
                audio.getCaptureDevMedia().stopTransmit(media);
                media.stopTransmit(audio.getPlaybackDevMedia());
            } catch (...) {
            }
        }
    } catch (...) {
    }
    // Do not change snapshot_.mediaActive here. That field represents the
    // negotiated PJSIP media state, not whether this call is routed locally
    // to the headset. A background/held call can still have negotiated media.
}

std::string CallSession::mediaDump()
{
    try {
        return dump(true, "  ");
    } catch (const pj::Error& e) {
        return e.info();
    }
}

void CallSession::syncSnapshot(const pj::CallInfo& info)
{
    std::lock_guard<std::mutex> lock(mutex_);
    snapshot_.id = info.id;
    snapshot_.callIdString = info.callIdString;
    snapshot_.remoteUri = info.remoteUri;
    snapshot_.state = info.stateText;
    snapshot_.lastStatusCode = static_cast<int>(info.lastStatusCode);
    snapshot_.lastReason = info.lastReason;
    snapshot_.connected = info.state == PJSIP_INV_STATE_CONFIRMED;
    snapshot_.disconnected = info.state == PJSIP_INV_STATE_DISCONNECTED;
    snapshot_.foreground = foreground_;
    snapshot_.microphoneMuted = microphoneMuted_;
    if (snapshot_.connected && snapshot_.connectedMs == 0) {
        snapshot_.connectedMs = nowMs();
    }
    if (snapshot_.disconnected) {
        snapshot_.mediaActive = false;
        if (snapshot_.disconnectedMs == 0) {
            snapshot_.disconnectedMs = nowMs();
        }
    }
}

void CallSession::refreshMediaInfo()
{
    try {
        const auto info = getInfo();
        std::string localRtp, localRtcp, remoteRtp, remoteRtcp, sourceRtp, sourceRtcp, codec;
        unsigned rate = 0;
        bool mediaActive = false;
        std::uint64_t txPackets=0,rxPackets=0,txBytes=0,rxBytes=0,txLoss=0,rxLoss=0,txDiscard=0,rxDiscard=0;
        double txJitterMs=0.0,rxJitterMs=0.0,rttMs=0.0;
        unsigned jbDelayMs=0;

        for (unsigned i = 0; i < info.media.size(); ++i) {
            if (info.media[i].type != PJMEDIA_TYPE_AUDIO) continue;
            const auto status = info.media[i].status;
            mediaActive = status == PJSUA_CALL_MEDIA_ACTIVE || status == PJSUA_CALL_MEDIA_LOCAL_HOLD || status == PJSUA_CALL_MEDIA_REMOTE_HOLD;
            try {
                const auto stream = getStreamInfo(i);
                remoteRtp = stream.remoteRtpAddress; remoteRtcp = stream.remoteRtcpAddress; codec = stream.codecName; rate = stream.codecClockRate;
            } catch (...) {}
            try {
                const auto st=getStreamStat(i);
                txPackets=st.rtcp.txStat.pkt;rxPackets=st.rtcp.rxStat.pkt;
                txBytes=st.rtcp.txStat.bytes;rxBytes=st.rtcp.rxStat.bytes;
                txLoss=st.rtcp.txStat.loss;rxLoss=st.rtcp.rxStat.loss;
                txDiscard=st.rtcp.txStat.discard;rxDiscard=st.rtcp.rxStat.discard;
                txJitterMs=st.rtcp.txStat.jitterUsec.mean/1000.0;
                rxJitterMs=st.rtcp.rxStat.jitterUsec.mean/1000.0;
                rttMs=st.rtcp.rttUsec.mean/1000.0;
                jbDelayMs=st.jbuf.avgDelayMsec;
            } catch (...) {}
            try {
                const auto transport = getMedTransportInfo(i);
                localRtp = transport.localRtpName; localRtcp = transport.localRtcpName; sourceRtp = transport.srcRtpName; sourceRtcp = transport.srcRtcpName;
            } catch (...) {}
            break;
        }
        const double denom=static_cast<double>(rxPackets+rxLoss);
        const double lossPct=denom>0.0?100.0*static_cast<double>(rxLoss)/denom:0.0;
        const auto quality=estimateVoiceQuality(rttMs,std::max(rxJitterMs,static_cast<double>(jbDelayMs)),lossPct);

        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_.mediaActive = mediaActive && !snapshot_.disconnected;
        snapshot_.localRtpAddress = std::move(localRtp); snapshot_.localRtcpAddress = std::move(localRtcp);
        snapshot_.remoteRtpAddress = std::move(remoteRtp); snapshot_.remoteRtcpAddress = std::move(remoteRtcp);
        snapshot_.sourceRtpAddress = std::move(sourceRtp); snapshot_.sourceRtcpAddress = std::move(sourceRtcp);
        snapshot_.codecName = std::move(codec); snapshot_.codecClockRate = rate;
        snapshot_.rtpTxPackets=txPackets;snapshot_.rtpRxPackets=rxPackets;snapshot_.rtpTxBytes=txBytes;snapshot_.rtpRxBytes=rxBytes;
        snapshot_.rtpTxLoss=txLoss;snapshot_.rtpRxLoss=rxLoss;snapshot_.rtpTxDiscard=txDiscard;snapshot_.rtpRxDiscard=rxDiscard;
        snapshot_.txJitterMs=txJitterMs;snapshot_.rxJitterMs=rxJitterMs;snapshot_.rttMs=rttMs;snapshot_.jitterBufferDelayMs=jbDelayMs;
        snapshot_.estimatedRFactor=quality.first;snapshot_.estimatedMos=quality.second;
    } catch (...) {}
}

std::string CallSession::formatTraceEntry(const SipTraceEntry& entry)
{
    const auto point = std::chrono::system_clock::time_point(std::chrono::milliseconds(entry.timestampMs));
    const auto timestamp = std::chrono::system_clock::to_time_t(point);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &timestamp);
#else
    localtime_r(&timestamp, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S") << "."
        << std::setfill('0') << std::setw(3) << (entry.timestampMs % 1000)
        << " [" << (entry.direction == SipDirection::Sent ? "SENT ->" : "<- RECEIVED") << "] "
        << entry.label << " CSeq=" << entry.cseq << " Call-ID=" << entry.callIdString << "\n";
    out << entry.rawMessage;
    if (entry.rawMessage.empty() || entry.rawMessage.back() != '\n') {
        out << '\n';
    }
    out << "\n";
    return out.str();
}

void CallSession::recordSipMessage(SipTraceEntry entry)
{
    std::lock_guard<std::mutex> lock(mutex_);
    entry.callId = snapshot_.id;
    if (entry.callIdString.empty()) {
        entry.callIdString = snapshot_.callIdString;
    }
    classifySipTraceEntry(entry, traceClassifier_);
    sipTrace_.push_back(entry);
    if (sipTrace_.size() > 2000) {
        sipTrace_.erase(sipTrace_.begin(), sipTrace_.begin() + 500);
    }
    if (sipTraceFile_) {
        sipTraceFile_ << formatTraceEntry(entry);
        sipTraceFile_.flush();
    }
}

std::vector<SipTraceEntry> CallSession::sipTrace() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sipTrace_;
}

void CallSession::startSipTraceFile(const std::string& path)
{
    const std::filesystem::path filePath(path);
    if (filePath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(filePath.parent_path(), ec);
        if (ec && !std::filesystem::is_directory(filePath.parent_path())) {
            throw std::runtime_error("Unable to create SIP trace directory: " +
                                     filePath.parent_path().string());
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    sipTraceFile_.close();
    sipTraceFile_.clear();
    sipTraceFile_.open(path, std::ios::out | std::ios::trunc);
    if (!sipTraceFile_) {
        throw std::runtime_error("Unable to create SIP trace: " + path);
    }
    protectFile(path);
    sipTracePath_ = path;
    sipTraceFile_ << "# WaffleHouse-Client 1.0.0 single-call SIP trace\n# Call-ID: "
                  << snapshot_.callIdString << "\n\n";
    for (const auto& entry : sipTrace_) {
        sipTraceFile_ << formatTraceEntry(entry);
    }
    sipTraceFile_.flush();
}

void CallSession::stopSipTraceFile()
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (sipTraceFile_) {
        sipTraceFile_.flush();
    }
    sipTraceFile_.close();
    sipTracePath_.clear();
}

bool CallSession::sipTraceRecording() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sipTraceFile_.is_open();
}

std::string CallSession::sipTracePath() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return sipTracePath_;
}

void CallSession::notify()
{
    UpdateCallback callback;
    int id = PJSUA_INVALID_ID;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = updateCallback_;
        id = snapshot_.id;
    }
    if (callback) {
        callback(id);
    }
}

void CallSession::onCallState(pj::OnCallStateParam&)
{
    // Keep the wrapper alive until this callback actually returns. PJSIP 2.17
    // removes the Call user-data association before invoking DISCONNECTED, and
    // the engine may archive/erase its owner during notify(). The managed PJSIP
    // compatibility guard makes any later destructor safe if another thread
    // temporarily retains this shared_ptr.
    auto callbackLifetime = weak_from_this().lock();
    (void)callbackLifetime;
    try {
        const auto info = getInfo();
        syncSnapshot(info);
        refreshMediaInfo();
        std::ostringstream out;
        out << "Call " << info.id << " " << info.stateText << " SIP="
            << static_cast<int>(info.lastStatusCode) << " " << info.lastReason;
        logger_.info(out.str());
        if (info.state == PJSIP_INV_STATE_DISCONNECTED) {
            detachAudio();
        }
        notify();
    } catch (const pj::Error& e) {
        logger_.warn("onCallState: " + e.info());
    }
}

void CallSession::onCallMediaState(pj::OnCallMediaStateParam&)
{
    refreshMediaInfo();
    bool foreground = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        foreground = foreground_;
    }
    if (foreground) attachAudio();
    notify();
}

void CallSession::onDtmfDigit(pj::OnDtmfDigitParam& param)
{
    logger_.info("Call " + std::to_string(getId()) + " received DTMF: " + param.digit);
}
} // namespace trunkmonkey
