#include "trunkmonkey/SipEngine.h"
#include "trunkmonkey/CallSession.h"
#include "trunkmonkey/Logger.h"
#include "trunkmonkey/RuntimePaths.h"
#include "trunkmonkey/SipAccount.h"
#include "trunkmonkey/SipWireMonitor.h"
#include "trunkmonkey/Version.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <thread>
#include <pj/log.h>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace trunkmonkey {
static_assert(PJSUA_MAX_ACC >= 32,
              "WaffleHouse-Client requires PJSIP built with PJSUA_MAX_ACC >= 32 for multi-account SIP. Use scripts/build-pjsip.sh.");
static_assert(PJSUA_MAX_CALLS >= 50,
              "WaffleHouse-Client requires PJSIP built with PJSUA_MAX_CALLS >= 50. Use scripts/build-pjsip.sh.");
static_assert(PJ_IOQUEUE_MAX_HANDLES >= 192,
              "WaffleHouse-Client requires PJ_IOQUEUE_MAX_HANDLES >= 192 for 64-call PJSIP. Rebuild PJSIP with scripts/build-pjsip.sh.");

namespace {
class PjBootstrapLogSilencer
{
public:
    PjBootstrapLogSilencer()
        : previousLevel_(pj_log_get_level())
    {
        // PJSIP emits several pjlib/pjsip startup messages from libCreate()
        // before EpConfig::logConfig is installed by libInit(). In a curses
        // frontend those raw console writes can bleed through between the
        // splash screen and the first dashboard repaint. Silence only this
        // pre-configuration bootstrap window; libInit() immediately installs
        // WaffleHouse's normal file-backed PJSIP logger (consoleLevel=0).
        pj_log_set_level(0);
    }

    void release() noexcept { restoreOnExit_ = false; }

    ~PjBootstrapLogSilencer()
    {
        // If libCreate()/libInit() throws before PJSIP installs its configured
        // logger, restore the process-global level we inherited.
        if (restoreOnExit_) pj_log_set_level(previousLevel_);
    }

private:
    int previousLevel_{0};
    bool restoreOnExit_{true};
};

std::string trim(std::string value)
{
    const auto isSpace=[](unsigned char c){ return std::isspace(c)!=0; };
    while(!value.empty() && isSpace(static_cast<unsigned char>(value.front()))) value.erase(value.begin());
    while(!value.empty() && isSpace(static_cast<unsigned char>(value.back()))) value.pop_back();
    return value;
}

std::string quotedDisplayName(std::string value)
{
    std::string escaped;
    escaped.reserve(value.size()+2);
    for(char c:value){
        if(c=='\\' || c=='"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return "\""+escaped+"\"";
}

std::string nameAddr(const std::string& value)
{
    return value.find('<')!=std::string::npos ? value : "<"+value+">";
}

struct AudioDeviceKey {
    std::string driver;
    std::string name;
    bool valid{false};
};

AudioDeviceKey deviceKey(const pj::AudioDevInfoVector2& devices,int id)
{
    if(id<0 || static_cast<std::size_t>(id)>=devices.size()) return {};
    return {devices[static_cast<std::size_t>(id)].driver,devices[static_cast<std::size_t>(id)].name,true};
}

int resolveDeviceKey(const pj::AudioDevInfoVector2& devices,const AudioDeviceKey& key,bool capture)
{
    if(!key.valid) return capture ? PJMEDIA_AUD_DEFAULT_CAPTURE_DEV : PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV;
    for(std::size_t i=0;i<devices.size();++i){
        const auto& d=devices[i];
        if(d.driver==key.driver && d.name==key.name){
            if(capture && d.inputCount<=0) break;
            if(!capture && d.outputCount<=0) break;
            return static_cast<int>(i);
        }
    }
    // Device disappeared (common with USB headset removal). Do not reuse the
    // old numeric index because refreshDevs() may have assigned that index to a
    // completely different device. Fall back to PJSIP's system default.
    return capture ? PJMEDIA_AUD_DEFAULT_CAPTURE_DEV : PJMEDIA_AUD_DEFAULT_PLAYBACK_DEV;
}

std::string audioDeviceLabel(const pj::AudioDevInfoVector2& devices,int id)
{
    if(id<0) return "PJSIP system default ("+std::to_string(id)+")";
    if(static_cast<std::size_t>(id)>=devices.size()) return "device "+std::to_string(id)+" (index unavailable)";
    const auto& d=devices[static_cast<std::size_t>(id)];
    return "["+std::to_string(id)+"] "+d.driver+" / "+d.name;
}

#if defined(__linux__) || defined(__FreeBSD__)
std::string runCommandText(const char* command)
{
    std::string result;
    FILE* pipe=::popen(command,"r");
    if(!pipe) return {};
    char buffer[512];
    while(std::fgets(buffer,sizeof(buffer),pipe)) result+=buffer;
    const int rc=::pclose(pipe);
    if(rc!=0) return {};
    return trim(result);
}

std::string pulseDefaultFromInfo(const std::string& field)
{
    const auto text=runCommandText("pactl info 2>/dev/null");
    if(text.empty()) return {};
    std::istringstream in(text);
    std::string line;
    while(std::getline(in,line)){
        const auto clean=trim(line);
        if(clean.rfind(field,0)==0) return trim(clean.substr(field.size()));
    }
    return {};
}

std::string activePulsePort(const char* listCommand,const std::string& nodeName)
{
    if(nodeName.empty()) return {};
    const auto text=runCommandText(listCommand);
    if(text.empty()) return {};
    std::istringstream in(text);
    std::string line;
    bool inTarget=false;
    while(std::getline(in,line)){
        const auto clean=trim(line);
        if(clean.rfind("Name:",0)==0){
            inTarget=trim(clean.substr(5))==nodeName;
            continue;
        }
        if(inTarget && clean.rfind("Active Port:",0)==0) return trim(clean.substr(12));
    }
    return {};
}

struct SystemAudioRouteInfo {
    std::string signature;
    std::string display;
    std::string backend;
    bool available{false};
};

SystemAudioRouteInfo pulseSystemAudioRoute(const std::string& backend)
{
    // pactl speaks to PipeWire's PulseAudio compatibility layer on modern Linux
    // desktops and to a native PulseAudio daemon where one is used on FreeBSD.
    // Older PulseAudio versions may not implement get-default-{sink,source}, so
    // fall back to parsing `pactl info`.
    auto sink=runCommandText("pactl get-default-sink 2>/dev/null");
    auto source=runCommandText("pactl get-default-source 2>/dev/null");
    if(sink.empty()) sink=pulseDefaultFromInfo("Default Sink:");
    if(source.empty()) source=pulseDefaultFromInfo("Default Source:");
    if(sink.empty() && source.empty()) return {};
    const auto sinkPort=activePulsePort("pactl list sinks 2>/dev/null",sink);
    const auto sourcePort=activePulsePort("pactl list sources 2>/dev/null",source);
    SystemAudioRouteInfo info;
    info.available=true;
    info.backend=backend;
    info.signature="sink="+sink+";port="+sinkPort+";source="+source+";port="+sourcePort;
    info.display=backend+": sink="+sink+";port="+(sinkPort.empty()?"<unknown>":sinkPort)+
                 ";source="+source+";port="+(sourcePort.empty()?"<unknown>":sourcePort);
    return info;
}

#ifdef __FreeBSD__
std::string freebsdPcmLine(const std::string& sndstat,const std::string& unit)
{
    if(unit.empty()) return {};
    const std::string prefix="pcm"+unit+":";
    std::istringstream in(sndstat);
    std::string line;
    while(std::getline(in,line)){
        const auto clean=trim(line);
        if(clean.rfind(prefix,0)==0) return clean;
    }
    return {};
}

bool digitsOnly(const std::string& value)
{
    return !value.empty() && std::all_of(value.begin(),value.end(),[](unsigned char c){return std::isdigit(c)!=0;});
}

SystemAudioRouteInfo freebsdNativeAudioRoute()
{
    // FreeBSD does not have one universal desktop audio policy daemon. snd_hda
    // normally performs speaker/headphone automute itself when a Headphones pin
    // uses seq=15 in the same association. For application-level recovery we
    // watch the pieces exposed to unprivileged userland: default PCM unit,
    // installed PCM devices, and the active recording source. The latter changes
    // when dev.pcm.N.rec.autosrc follows a newly inserted headset microphone.
    const auto unit=runCommandText("sysctl -n hw.snd.default_unit 2>/dev/null");
    const auto defaultAuto=runCommandText("sysctl -n hw.snd.default_auto 2>/dev/null");
    const auto sndstat=runCommandText("cat /dev/sndstat 2>/dev/null");
    if(unit.empty() && sndstat.empty()) return {};

    std::string recsrc;
    if(digitsOnly(unit)){
        const std::string command="mixer -d "+unit+" -s 2>/dev/null";
        recsrc=runCommandText(command.c_str());
    }
    const auto pcm=freebsdPcmLine(sndstat,unit);

    // Keep all pcm description lines in the signature so USB/other PCM
    // attach/detach is noticed, but keep the human-facing display compact.
    std::ostringstream pcmSignature;
    std::istringstream in(sndstat);
    std::string line;
    while(std::getline(in,line)){
        const auto clean=trim(line);
        if(clean.rfind("pcm",0)==0) pcmSignature<<clean<<'|';
    }

    SystemAudioRouteInfo info;
    info.available=true;
    info.backend="FreeBSD OSS/snd_hda";
    info.signature="default="+unit+";default_auto="+defaultAuto+";recsrc="+recsrc+";pcms="+pcmSignature.str();
    info.display=info.backend+": default="+(unit.empty()?"<unknown>":"pcm"+unit);
    if(!defaultAuto.empty()) info.display+=";default_auto="+defaultAuto;
    if(!recsrc.empty()) info.display+=";recsrc="+recsrc;
    if(!pcm.empty()) info.display+=";device="+pcm;
    return info;
}
#endif

SystemAudioRouteInfo systemAudioRoute()
{
#ifdef __linux__
    return pulseSystemAudioRoute("PipeWire/PulseAudio");
#elif defined(__FreeBSD__)
    // FreeBSD may run PulseAudio on top of OSS, but PJSIP/PortAudio can still be
    // opening OSS directly. Combine both views so a PulseAudio port change OR a
    // native default-unit/PCM/recsrc change can trigger recovery.
    const auto pulse=pulseSystemAudioRoute("PulseAudio (FreeBSD)");
    const auto native=freebsdNativeAudioRoute();
    if(pulse.available && native.available){
        SystemAudioRouteInfo info;
        info.available=true;
        info.backend="PulseAudio + FreeBSD OSS/snd_hda";
        info.signature="pulse{"+pulse.signature+"};native{"+native.signature+"}";
        info.display=pulse.display+" | "+native.display;
        return info;
    }
    if(pulse.available) return pulse;
    return native;
#else
    return {};
#endif
}
#endif
}

SipEngine::SipEngine(Logger& logger):logger_(logger){}
SipEngine::~SipEngine(){ stop(); }

void SipEngine::start(const SipProfile& p,unsigned maxCalls)
{
    start(std::vector<std::pair<std::string,SipProfile>>{{"default",p}},maxCalls);
    setAccountRegistration("default",true);
}

namespace {
std::string sipTransportKey(const SipProfile& p)
{
    return toString(p.transport)+":"+std::to_string(p.localSipPort);
}
}

pj::TransportId SipEngine::ensureTransport(const SipProfile& p)
{
    if(!endpoint_) throw std::runtime_error("SIP endpoint is not initialized");
    const auto key=sipTransportKey(p);
    const auto existing=transports_.find(key);
    if(existing!=transports_.end()) return existing->second;
    pj::TransportConfig tc;
    tc.port=p.localSipPort;
    pjsip_transport_type_e type=PJSIP_TRANSPORT_UDP;
    if(p.transport==Transport::Tcp) type=PJSIP_TRANSPORT_TCP;
    else if(p.transport==Transport::Tls) type=PJSIP_TRANSPORT_TLS;
    const pj::TransportId id=endpoint_->transportCreate(type,tc);
    transports_[key]=id;
    return id;
}

void SipEngine::createAccount(const std::string& accountId,const SipProfile& input,bool registerNow)
{
    if(accountId.empty()) throw std::runtime_error("SIP account id is empty");
    if(!endpoint_ || !started_ || stopping_) throw std::runtime_error("SIP endpoint is not running");
    ProfileStore::validate(input);
    SipProfile p=input;
    if(p.registrar.empty()) p.registrar="sip:"+p.sipDomain;
    if(p.authUsername.empty()) p.authUsername=p.username;
    if(p.callerIdDomain.empty()) p.callerIdDomain=p.sipDomain;

    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        if(accounts_.find(accountId)!=accounts_.end()) throw std::runtime_error("SIP account already exists: "+accountId);
        if(accounts_.size()>=static_cast<std::size_t>(PJSUA_MAX_ACC))
            throw std::runtime_error("Maximum SIP account count reached ("+std::to_string(PJSUA_MAX_ACC)+")");
    }
    const auto transportId=ensureTransport(p);

    AccountState state;
    state.profile=p;
    state.transportId=transportId;
    state.registrationEnabled=registerNow;
    state.registrationText=registerNow?"Registering":"Offline";
    state.dialPrefix=p.dialPrefix;
    state.account=std::make_unique<SipAccount>(*this,logger_,accountId);

    pj::AccountConfig ac;
    ac.idUri=quotedDisplayName(p.displayName)+" <sip:"+p.username+"@"+p.sipDomain+">";
    ac.regConfig.registrarUri=p.registrar;
    ac.regConfig.timeoutSec=p.registrationExpires;
    ac.regConfig.registerOnAdd=registerNow;
    ac.sipConfig.transportId=transportId;
    const auto authUser=p.authUsername.empty()?p.username:p.authUsername;
    ac.sipConfig.authCreds.emplace_back("digest","*",authUser,0,p.password);
    if(!p.outboundProxy.empty()) ac.sipConfig.proxies.push_back(p.outboundProxy);
    ac.natConfig.iceEnabled=p.useIce;
    if(p.enableSrtp) ac.mediaConfig.srtpUse=PJMEDIA_SRTP_OPTIONAL;

    SipAccount* accountPtr=state.account.get();
    const bool makeDefault=[&]{std::lock_guard<std::mutex> lock(accountMutex_);return accounts_.empty();}();
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        accounts_.emplace(accountId,std::move(state));
        if(defaultAccountId_.empty()){
            defaultAccountId_=accountId;
            profile_=p;
        }
    }
    try{
        accountPtr->create(ac,makeDefault);
    }catch(...){
        std::lock_guard<std::mutex> lock(accountMutex_);
        accounts_.erase(accountId);
        if(defaultAccountId_==accountId) defaultAccountId_.clear();
        throw;
    }
    refreshStunServers();
    logger_.info("SIP account added: "+accountId+" "+p.username+"@"+p.sipDomain+
                 " transport="+toString(p.transport)+(registerNow?" registering":" offline"));
    updateAggregateRegistration();
}

void SipEngine::refreshStunServers()
{
    if(!endpoint_ || !started_) return;
    pj::StringVector servers;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        for(const auto& item:accounts_){
            const auto& stun=item.second.profile.stunServer;
            if(!stun.empty() && std::find(servers.begin(),servers.end(),stun)==servers.end())
                servers.push_back(stun);
        }
    }
    // PJSIP 2.17's pjsua_update_stun_servers() asserts when called with
    // count == 0 / srv == nullptr. An empty STUN list is a perfectly valid
    // WaffleHouse configuration, so there is nothing to update in that case.
    // The endpoint was already initialized with an empty UaConfig::stunServer
    // list when no account requested STUN.
    if(servers.empty()) {
        logger_.info("Runtime STUN refresh skipped: no SIP account has a STUN server configured.");
        return;
    }
    try{
        endpoint_->natUpdateStunServers(servers,false);
    }catch(const pj::Error& e){
        logger_.warn("Unable to refresh runtime STUN server list: "+e.info());
    }
}

void SipEngine::start(const std::vector<std::pair<std::string,SipProfile>>& initialAccounts,unsigned maxCalls)
{
    if(stopping_) throw std::runtime_error("SIP engine is still shutting down");
    if(started_) return;
    if(maxCalls<1 || maxCalls>50) throw std::runtime_error("maxCalls must be 1-50");

    registered_=false;
    {
        std::lock_guard<std::mutex> lock(regMutex_);
        registrationText_="Starting";
    }

    try{
        endpoint_=std::make_unique<pj::Endpoint>();
        PjBootstrapLogSilencer bootstrapLogSilencer;
        endpoint_->libCreate();

        pj::EpConfig ec;
        ec.uaConfig.maxCalls=maxCalls;
        ec.uaConfig.userAgent=WAFFLEHOUSE_SOFTPHONE_USER_AGENT;
        ec.uaConfig.threadCnt=2;
        ec.logConfig.level=5;
        ec.logConfig.consoleLevel=0;
        ec.logConfig.msgLogging=1;
        ec.logConfig.filename=runtime::pjsipLogPath().string();
        std::vector<std::string> stunServers;
        for(const auto& item:initialAccounts){
            const auto& stun=item.second.stunServer;
            if(!stun.empty() && std::find(stunServers.begin(),stunServers.end(),stun)==stunServers.end())
                stunServers.push_back(stun);
        }
        ec.uaConfig.stunServer=stunServers;
        endpoint_->libInit(ec);
        // libInit() has now installed ec.logConfig, including consoleLevel=0
        // and the normal pjsip-engine.log destination. Do not restore the old
        // bootstrap level after this point.
        bootstrapLogSilencer.release();

        sipMonitor_=std::make_unique<SipWireMonitor>(*this,logger_);
        sipMonitor_->start();
        endpoint_->libStart();
        stopping_=false;
        started_=true;

        // Enumerate audio devices after PJSIP has started. Capture and playback
        // are intentionally selected independently: FreeBSD laptops commonly
        // expose an internal duplex codec plus a dedicated headset-mic capture
        // device. Leaving both on the system default can silently route calls
        // through the internal microphone even when the headset mic works.
        try {
            auto& audio=endpoint_->audDevManager();
            const auto devices=audio.enumDev2();
            logger_.info("PJSIP audio devices detected: "+std::to_string(devices.size()));
            for(std::size_t i=0;i<devices.size();++i){
                const auto& d=devices[i];
                logger_.info("Audio device ["+std::to_string(i)+"] driver=\""+d.driver+
                             "\" name=\""+d.name+"\" inputs="+std::to_string(d.inputCount)+
                             " outputs="+std::to_string(d.outputCount));
            }

            auto requestedDevice=[&](const char* envName,bool capture)->int {
                const char* raw=std::getenv(envName);
                if(raw==nullptr || *raw=='\0') return -1;
                char* end=nullptr;
                const long id=std::strtol(raw,&end,10);
                if(end==raw || *end!='\0' || id<0 || static_cast<std::size_t>(id)>=devices.size()){
                    logger_.warn(std::string(envName)+" must be a valid numeric PJSIP audio device ID; ignoring value "+raw);
                    return -1;
                }
                const auto& d=devices[static_cast<std::size_t>(id)];
                if(capture && d.inputCount<=0){
                    logger_.warn(std::string(envName)+" selects a device with no capture channels; ignoring it");
                    return -1;
                }
                if(!capture && d.outputCount<=0){
                    logger_.warn(std::string(envName)+" selects a device with no playback channels; ignoring it");
                    return -1;
                }
                return static_cast<int>(id);
            };

            int captureId=requestedDevice("WAFFLEHOUSE_CAPTURE_DEVICE",true);
            if(captureId<0) captureId=requestedDevice("SIPCLIENT_CAPTURE_DEVICE",true);
            if(captureId<0) captureId=requestedDevice("TRUNKMONKEY_CAPTURE_DEVICE",true);
            int playbackId=requestedDevice("WAFFLEHOUSE_PLAYBACK_DEVICE",false);
            if(playbackId<0) playbackId=requestedDevice("SIPCLIENT_PLAYBACK_DEVICE",false);
            if(playbackId<0) playbackId=requestedDevice("TRUNKMONKEY_PLAYBACK_DEVICE",false);

#ifdef __FreeBSD__
            // FreeBSD snd_hda/OSS commonly exposes a laptop as a duplex pcm0
            // plus a dedicated combo-jack/headset microphone. PortAudio device
            // metadata is not perfectly consistent across releases, so score
            // capture candidates instead of relying only on outputCount==0.
            if(captureId<0){
                int bestCapture=-1;
                int bestScore=-1;
                bool tie=false;
                for(std::size_t i=0;i<devices.size();++i){
                    const auto& d=devices[i];
                    if(d.inputCount<=0) continue;
                    std::string name=d.name;
                    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                    int score=0;
                    if(d.outputCount==0) score+=40;
                    if(name.find("headset")!=std::string::npos) score+=80;
                    if(name.find("right analog mic")!=std::string::npos) score+=90;
                    if(name.find("microphone")!=std::string::npos) score+=55;
                    if(name.find(" mic")!=std::string::npos || name.rfind("mic",0)==0) score+=50;
                    if(name.find("internal")!=std::string::npos) score-=50;
                    if(score>bestScore){bestScore=score;bestCapture=static_cast<int>(i);tie=false;}
                    else if(score==bestScore && score>0){tie=true;}
                }
                if(bestCapture>=0 && bestScore>=40 && !tie){
                    captureId=bestCapture;
                    logger_.info("FreeBSD audio routing: selecting preferred microphone device ["+
                                 std::to_string(captureId)+"] "+devices[static_cast<std::size_t>(captureId)].name+
                                 " score="+std::to_string(bestScore));
                }else if(tie){
                    logger_.warn("FreeBSD audio routing: microphone candidates tied; keeping PJSIP default. "
                                 "Set WAFFLEHOUSE_CAPTURE_DEVICE to the desired numeric device ID.");
                }
            }
#endif

#ifdef __linux__
            // Prefer the ALSA "pipewire" PCM on Linux desktops when the user
            // has not explicitly selected a device. This keeps WaffleHouse-Client
            // inside PipeWire/WirePlumber policy instead of pinning a raw hw: PCM.
            if(captureId<0 || playbackId<0){
                for(std::size_t i=0;i<devices.size();++i){
                    std::string driver=devices[i].driver,name=devices[i].name;
                    std::transform(driver.begin(),driver.end(),driver.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                    std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
                    if(name=="pipewire" || (driver=="alsa" && name.find("pipewire")!=std::string::npos)){
                        if(captureId<0 && devices[i].inputCount>0) captureId=static_cast<int>(i);
                        if(playbackId<0 && devices[i].outputCount>0) playbackId=static_cast<int>(i);
                    }
                }
            }
#endif
            if(captureId>=0) audio.setCaptureDev(captureId);
            if(playbackId>=0) audio.setPlaybackDev(playbackId);

            logger_.info("Active PJSIP capture device ID: "+std::to_string(audio.getCaptureDev()));
            logger_.info("Active PJSIP playback device ID: "+std::to_string(audio.getPlaybackDev()));
#if defined(__linux__) || defined(__FreeBSD__)
            {
                const auto route=systemAudioRoute();
                lastSystemAudioRoute_=route.signature;
                if(route.available) logger_.info("[AUDIO] System route baseline ("+route.backend+"): "+route.display);
            }
#endif
        } catch(const pj::Error& e){
            logger_.warn("Unable to enumerate/select PJSIP audio devices: "+e.info());
        }


        for(const auto& item:initialAccounts) createAccount(item.first,item.second,false);
        updateAggregateRegistration();
        logger_.info("WaffleHouse-Client multi-account SIP endpoint started; accounts="+
                     std::to_string(initialAccounts.size())+" pjsip_log="+runtime::pjsipLogPath().string());
    }catch(...){
        stop();
        throw;
    }
}

void SipEngine::addAccount(const std::string& accountId,const SipProfile& profile,bool registerNow)
{
    std::lock_guard<std::mutex> creationLock(callCreateMutex_);
    if(!started_) throw std::runtime_error("Start the SIP endpoint before adding accounts");
    createAccount(accountId,profile,registerNow);
}

bool SipEngine::accountHasLiveCalls(const std::string& accountId)const
{
    std::lock_guard<std::mutex> lock(mutex_);
    for(const auto& item:calls_){
        const auto snap=item.second->snapshot();
        if(snap.accountId==accountId && !snap.disconnected) return true;
    }
    return false;
}

void SipEngine::updateAccount(const std::string& accountId,const SipProfile& profile,bool preserveRegistration)
{
    std::lock_guard<std::mutex> creationLock(callCreateMutex_);
    if(accountHasLiveCalls(accountId)) throw std::runtime_error("Cannot reconfigure a SIP account while it has an active call");
    bool reg=false;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        auto it=accounts_.find(accountId);
        if(it==accounts_.end()) throw std::runtime_error("SIP account not found: "+accountId);
        reg=preserveRegistration && it->second.registrationEnabled;
        if(it->second.account && it->second.account->isValid()){
            try{it->second.account->setRegistration(false);}catch(...){}
            try{it->second.account->shutdown();}catch(...){}
        }
        accounts_.erase(it);
    }
    createAccount(accountId,profile,reg);
    if(defaultAccountId_==accountId) profile_=profile;
}

void SipEngine::removeAccount(const std::string& accountId)
{
    std::lock_guard<std::mutex> creationLock(callCreateMutex_);
    if(accountHasLiveCalls(accountId)) throw std::runtime_error("Cannot remove a SIP account while it has an active call");
    std::unique_ptr<SipAccount> account;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        auto it=accounts_.find(accountId);
        if(it==accounts_.end()) return;
        account=std::move(it->second.account);
        accounts_.erase(it);
        if(defaultAccountId_==accountId){
            defaultAccountId_=accounts_.empty()?std::string{}:accounts_.begin()->first;
            if(!defaultAccountId_.empty()) profile_=accounts_.begin()->second.profile;
        }
    }
    if(account && account->isValid()){
        try{account->setRegistration(false);}catch(...){}
        try{account->shutdown();}catch(...){}
    }
    account.reset();
    refreshStunServers();
    updateAggregateRegistration();
}

void SipEngine::setAccountRegistration(const std::string& accountId,bool enabled)
{
    SipAccount* account=nullptr;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        auto it=accounts_.find(accountId);
        if(it==accounts_.end() || !it->second.account) throw std::runtime_error("SIP account not found: "+accountId);
        it->second.registrationEnabled=enabled;
        it->second.registrationText=enabled?"Registering":"Offline";
        if(!enabled) it->second.registered=false;
        account=it->second.account.get();
    }
    account->setRegistration(enabled);
    updateAggregateRegistration();
}

bool SipEngine::accountRegistered(const std::string& accountId)const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    const auto it=accounts_.find(accountId); return it!=accounts_.end() && it->second.registered;
}

bool SipEngine::accountRegistrationEnabled(const std::string& accountId)const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    const auto it=accounts_.find(accountId); return it!=accounts_.end() && it->second.registrationEnabled;
}

std::string SipEngine::accountRegistrationText(const std::string& accountId)const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    const auto it=accounts_.find(accountId); return it==accounts_.end()?"Unknown account":it->second.registrationText;
}

SipProfile SipEngine::accountProfile(const std::string& accountId)const
{
    return profileForAccount(accountId);
}

SipProfile SipEngine::profileForAccount(const std::string& accountId)const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    const auto it=accounts_.find(accountId);
    if(it==accounts_.end()) throw std::runtime_error("SIP account not found: "+accountId);
    return it->second.profile;
}

std::vector<std::string> SipEngine::accountIds()const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    std::vector<std::string> out;out.reserve(accounts_.size());
    for(const auto& item:accounts_) out.push_back(item.first);
    return out;
}

std::vector<SipAccountStatus> SipEngine::accountStatuses()const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    std::vector<SipAccountStatus> out;out.reserve(accounts_.size());
    for(const auto& item:accounts_){
        SipAccountStatus st;st.id=item.first;st.name=item.second.profile.name;
        st.identity=item.second.profile.username+"@"+item.second.profile.sipDomain;
        st.registered=item.second.registered;st.registrationEnabled=item.second.registrationEnabled;
        st.registrationText=item.second.registrationText;out.push_back(std::move(st));
    }
    return out;
}

void SipEngine::updateAggregateRegistration()
{
    int total=0,enabled=0,reg=0;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        total=static_cast<int>(accounts_.size());
        for(const auto& item:accounts_){
            if(item.second.registrationEnabled) ++enabled;
            if(item.second.registered) ++reg;
        }
    }
    registered_=reg>0;
    std::lock_guard<std::mutex> lock(regMutex_);
    if(!started_) registrationText_="Stopped";
    else if(total==0) registrationText_="No SIP accounts configured";
    else registrationText_=std::to_string(reg)+"/"+std::to_string(total)+" registered"+
        (enabled<total?" ("+std::to_string(enabled)+" enabled)":"");
}

void SipEngine::stop()
{
    if(stopping_.exchange(true)) return;
    started_=false;
    registered_=false;

    // Barrier against an outgoing/incoming CallSession that passed its first
    // state check just before stopping_ became true. Once this lock has been
    // acquired and released, no call-creation path can still be in progress.
    {
        std::lock_guard<std::mutex> creationBarrier(callCreateMutex_);
    }


    // Stop new wire-monitor callbacks before draining call/account state.
    if(sipMonitor_){
        try{ sipMonitor_->stop(); }catch(...){}
        sipMonitor_.reset();
    }

    if(endpoint_){
        try{ hangupAll(); }catch(...){}

        // PJSIP 2.17 Account::shutdown2(force=false) intentionally rejects
        // account deletion while calls are active. Give DISCONNECTED callbacks
        // a bounded window to complete before releasing Call wrappers.
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds(1500);
        for(;;){
            bool active=false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for(const auto& item:calls_){
                    if(!item.second->snapshot().disconnected){ active=true; break; }
                }
            }
            if(!active || std::chrono::steady_clock::now()>=deadline) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
    }

    // pj::Call destructors touch PJSUA call user-data. Release every wrapper
    // while PJSUA-LIB and the Account still exist, never after libDestroy().
    std::vector<std::shared_ptr<CallSession>> drainingCalls;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        drainingCalls.reserve(calls_.size());
        for(auto& item:calls_) drainingCalls.push_back(std::move(item.second));
        calls_.clear();
        callIdIndex_.clear();
        pendingSip_.clear();
        foregroundId_=-1;
    }
    // Destroy wrappers without holding mutex_. pj::Call teardown can touch
    // PJSUA-LIB and must never be allowed to re-enter our call map lock.
    drainingCalls.clear();

    std::vector<std::unique_ptr<SipAccount>> drainingAccounts;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        drainingAccounts.reserve(accounts_.size());
        for(auto& item:accounts_) drainingAccounts.push_back(std::move(item.second.account));
        accounts_.clear();
        transports_.clear();
        defaultAccountId_.clear();
    }
    for(auto& account:drainingAccounts){
        if(!account || !account->isValid()) continue;
        pj::AccountShutdownParam prm;
        prm.force=false;
        try{
            account->shutdown2(prm);
        }catch(const pj::Error& e){
            logger_.warn("Graceful SIP account shutdown was busy/failed; forcing final account cleanup: "+e.info());
            try{ prm.force=true; account->shutdown2(prm); }
            catch(const pj::Error& forced){ logger_.warn("Forced SIP account shutdown failed: "+forced.info()); }
        }
    }
    drainingAccounts.clear();

    if(endpoint_){
        try{ endpoint_->libDestroy(); }
        catch(const pj::Error& e){ logger_.warn("PJSUA2 endpoint shutdown: "+e.info()); }
        catch(...){ logger_.warn("PJSUA2 endpoint shutdown raised an unknown error"); }
    }
    endpoint_.reset();
    {
        std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
        lastSystemAudioRoute_.clear();
        lastSystemAudioPollMs_=0;
        systemAudioWatchUnavailableLogged_=false;
    }

    {
        std::lock_guard<std::mutex> lock(regMutex_);
        registrationText_="Stopped";
    }
    stopping_=false;
}

bool SipEngine::started()const{return started_;}
bool SipEngine::registered()const{return registered_;}
std::string SipEngine::registrationText()const{std::lock_guard<std::mutex> lock(regMutex_);return registrationText_;}
const SipProfile& SipEngine::profile()const{return profile_;}

void SipEngine::setDialPrefix(const std::string& accountId,const std::string& prefix)
{
    const auto clean=trim(prefix);
    if(clean.find_first_of(" \t\r\n@<>:")!=std::string::npos)
        throw std::runtime_error("dial prefix must be a plain dial-string prefix (for example 9 or 4071)");
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        auto it=accounts_.find(accountId);
        if(it==accounts_.end()) throw std::runtime_error("SIP account not found: "+accountId);
        it->second.dialPrefix=clean;
    }
    logger_.info("["+accountId+"] Runtime dial prefix set to "+(clean.empty()?std::string("<none>"):clean));
}

std::string SipEngine::dialPrefix(const std::string& accountId)const
{
    std::lock_guard<std::mutex> lock(accountMutex_);
    const auto it=accounts_.find(accountId);
    if(it==accounts_.end()) throw std::runtime_error("SIP account not found: "+accountId);
    return it->second.dialPrefix;
}

void SipEngine::setDialPrefix(const std::string& prefix)
{
    if(defaultAccountId_.empty()) throw std::runtime_error("No SIP account selected");
    setDialPrefix(defaultAccountId_,prefix);
}

std::string SipEngine::dialPrefix()const
{
    if(defaultAccountId_.empty()) return profile_.dialPrefix;
    return dialPrefix(defaultAccountId_);
}

std::string SipEngine::normalizeDestination(const SipProfile& profile,const std::string& value,bool applyDialPrefix)const
{
    const auto v=trim(value);
    if(v.empty()) throw std::runtime_error("Destination is empty");
    if(v.rfind("sip:",0)==0 || v.rfind("sips:",0)==0 || v.find('<')!=std::string::npos) return v;
    if(v.find('@')!=std::string::npos) return "sip:"+v;
    const auto user=(applyDialPrefix && !profile.dialPrefix.empty()) ? profile.dialPrefix+v : v;
    return "sip:"+user+"@"+profile.sipDomain;
}

static std::string normalizeDestinationWithRuntimePrefix(const SipProfile& profile,const std::string& runtimePrefix,const std::string& value,bool applyDialPrefix)
{
    const auto v=trim(value);
    if(v.empty()) throw std::runtime_error("Destination is empty");
    if(v.rfind("sip:",0)==0 || v.rfind("sips:",0)==0 || v.find('<')!=std::string::npos) return v;
    if(v.find('@')!=std::string::npos) return "sip:"+v;
    const auto user=(applyDialPrefix && !runtimePrefix.empty()) ? runtimePrefix+v : v;
    return "sip:"+user+"@"+profile.sipDomain;
}

std::string SipEngine::normalizeDestination(const std::string& value,bool applyDialPrefix)const
{
    if(defaultAccountId_.empty()) return normalizeDestination(profile_,value,applyDialPrefix);
    return normalizeDestination(defaultAccountId_,value,applyDialPrefix);
}

std::string SipEngine::normalizeDestination(const std::string& accountId,const std::string& value,bool applyDialPrefix)const
{
    SipProfile profile;
    std::string activePrefix;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        const auto it=accounts_.find(accountId);
        if(it==accounts_.end()) throw std::runtime_error("SIP account not found: "+accountId);
        profile=it->second.profile;
        activePrefix=it->second.dialPrefix;
    }
    return normalizeDestinationWithRuntimePrefix(profile,activePrefix,value,applyDialPrefix);
}

std::string SipEngine::callerIdentityUri(const SipProfile& profile,const std::string& value)const
{
    const auto v=trim(value);
    if(v.empty()) return {};
    if(v.rfind("sip:",0)==0 || v.rfind("sips:",0)==0 || v.find('<')!=std::string::npos) return v;
    if(v.find('@')!=std::string::npos) return "sip:"+v;
    const auto domain=profile.callerIdDomain.empty()?profile.sipDomain:profile.callerIdDomain;
    return "sip:"+v+"@"+domain;
}

std::string SipEngine::callerIdentityUri(const std::string& value)const
{
    if(defaultAccountId_.empty()) return callerIdentityUri(profile_,value);
    return callerIdentityUri(profileForAccount(defaultAccountId_),value);
}

std::string SipEngine::callerIdentityUri(const std::string& accountId,const std::string& value)const
{
    return callerIdentityUri(profileForAccount(accountId),value);
}

void SipEngine::configureIdentity(const SipProfile& profile,pj::CallOpParam& param,const std::string& callerId)const
{
    if(callerId.empty()) return;
    const auto uri=callerIdentityUri(profile,callerId);
    if(profile.identityMode==IdentityMode::From || profile.identityMode==IdentityMode::FromAndPai){
        param.txOption.localUri=uri;
    }
    if(profile.identityMode==IdentityMode::Pai || profile.identityMode==IdentityMode::FromAndPai){
        pj::SipHeader header;
        header.hName="P-Asserted-Identity";
        header.hValue=nameAddr(uri);
        param.txOption.headers.push_back(header);
    }
    if(profile.identityMode==IdentityMode::Rpid){
        pj::SipHeader header;
        header.hName="Remote-Party-ID";
        header.hValue=nameAddr(uri)+";party=calling;screen=yes;privacy=off";
        param.txOption.headers.push_back(header);
    }
}

int SipEngine::makeCall(const std::string& destination,const std::string& callerId,bool makeForeground,CallPurpose purpose,bool applyDialPrefix)
{
    if(defaultAccountId_.empty()) throw std::runtime_error("No SIP account selected");
    return makeCall(defaultAccountId_,destination,callerId,makeForeground,purpose,applyDialPrefix);
}

int SipEngine::makeCall(const std::string& accountId,const std::string& destination,const std::string& callerId,bool makeForeground,CallPurpose purpose,bool applyDialPrefix)
{
    std::lock_guard<std::mutex> creationLock(callCreateMutex_);
    if(!started_ || stopping_) throw std::runtime_error("SIP endpoint is not running");
    SipAccount* account=nullptr;
    SipProfile profile;
    std::string activePrefix;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        const auto it=accounts_.find(accountId);
        if(it==accounts_.end() || !it->second.account) throw std::runtime_error("SIP account not found: "+accountId);
        account=it->second.account.get();
        profile=it->second.profile;
        activePrefix=it->second.dialPrefix;
    }
    auto call=std::make_shared<CallSession>(*account,logger_,CallDirection::Outgoing,purpose);
    call->setAccountIdentity(accountId,profile.name);
    call->setRequestedCallerId(callerId);
    call->setUpdateCallback([this](int id){ onCallUpdated(id); });
    pj::CallOpParam param(true);
    configureIdentity(profile,param,callerId);
    const auto uri=normalizeDestinationWithRuntimePrefix(profile,activePrefix,destination,applyDialPrefix);
    call->makeCall(uri,param);

    const auto afterMake=call->snapshot();
    const int id=afterMake.id!=PJSUA_INVALID_ID ? afterMake.id : call->getId();
    const bool live=addCall(call);
    logger_.info("Outgoing call "+std::to_string(id)+" account="+accountId+" -> "+uri+
                 (callerId.empty()?"":" CID="+callerId)+
                 (applyDialPrefix && !activePrefix.empty()?" dial-prefix="+activePrefix:""));
    if(makeForeground && live) setForeground(id);
    return id;
}

bool SipEngine::addCall(const std::shared_ptr<CallSession>& call)
{
    auto initial=call->snapshot();
    if(initial.disconnected){
        archiveDisconnectedCall(call,initial);
        return false;
    }

    std::string sipCallId;
    try{
        const auto info=call->getInfo();
        sipCallId=info.callIdString;
        call->refreshMediaInfo();
    }catch(...){
        // If the call disconnected during the getInfo/media refresh race,
        // preserve its final snapshot without invoking more live PJSUA2 APIs.
        initial=call->snapshot();
        if(initial.disconnected){
            archiveDisconnectedCall(call,initial);
            return false;
        }
    }

    std::vector<SipTraceEntry> pending;
    const auto current=call->snapshot();
    const int id=current.id!=PJSUA_INVALID_ID ? current.id : call->getId();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // PJSUA call slot numbers are reusable. Remove any stale SIP Call-ID
        // mapping that still points at this numeric slot before replacing it.
        for(auto it=callIdIndex_.begin();it!=callIdIndex_.end();){
            if(it->second==id) it=callIdIndex_.erase(it); else ++it;
        }
        // Numeric PJSUA call slots are reusable. A new live call supersedes
        // any archived diagnostics that used the same slot ID.
        archivedCalls_.erase(id);
        calls_[id]=call;
        if(!sipCallId.empty()){
            callIdIndex_[sipCallId]=id;
            const auto it=pendingSip_.find(sipCallId);
            if(it!=pendingSip_.end()){
                if(call->snapshot().purpose==CallPurpose::Phone) pending=std::move(it->second);
                pendingSip_.erase(it);
            }
        }
    }
    if(call->snapshot().purpose==CallPurpose::Phone){
        for(auto& entry:pending) call->recordSipMessage(std::move(entry));
    }
    return true;
}

std::shared_ptr<CallSession> SipEngine::findCall(int id)const
{
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it=calls_.find(id);
    return it==calls_.end()?nullptr:it->second;
}

const SipEngine::ArchivedCall* SipEngine::findArchivedCallLocked(int id)const
{
    const auto it=archivedCalls_.find(id);
    return it==archivedCalls_.end()?nullptr:&it->second;
}

std::shared_ptr<CallSession> SipEngine::requirePhoneCall(int id)const
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().purpose!=CallPurpose::Phone)
        throw std::runtime_error("Operation is limited to normal Phone calls.");
    return call;
}

void SipEngine::answer(int id)
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected) throw std::runtime_error("Call is disconnected");
    setForeground(id);
    call->answerCall();
}

void SipEngine::reject(int id,int code)
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected) throw std::runtime_error("Call is disconnected");
    if(code<300 || code>699) throw std::runtime_error("Reject code must be 300-699");
    call->rejectCall(code);
}

void SipEngine::hangup(int id)
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected) throw std::runtime_error("Call is already disconnected");
    call->hangupCall();
}

void SipEngine::hold(int id)
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected) throw std::runtime_error("Call is disconnected");
    call->holdCall();
}

void SipEngine::resume(int id)
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected) throw std::runtime_error("Call is disconnected");
    call->resumeCall();
}

void SipEngine::sendDtmf(int id,const std::string& digits,unsigned durationMs)
{
    auto call=findCall(id);
    if(!call) throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected) throw std::runtime_error("Call is disconnected");
    if(digits.empty()) throw std::runtime_error("DTMF digits required");
    call->sendDtmfDigits(digits,durationMs);
}

void SipEngine::setMicrophoneMuted(int id,bool muted)
{
    auto call=findCall(id);
    if(!call)throw std::runtime_error("Call not found");
    if(call->snapshot().disconnected)throw std::runtime_error("Call is disconnected");
    call->setMicrophoneMuted(muted);
}

std::vector<AudioDeviceInfo> SipEngine::audioDevices()const
{
    if(!endpoint_)return {};
    std::lock_guard<std::mutex> audioLock(audioMutex_);
    std::vector<AudioDeviceInfo> out;
    const auto devices=endpoint_->audDevManager().enumDev2();out.reserve(devices.size());
    for(std::size_t i=0;i<devices.size();++i){const auto&d=devices[i];out.push_back({static_cast<int>(i),d.driver,d.name,static_cast<unsigned>(d.inputCount),static_cast<unsigned>(d.outputCount)});}
    return out;
}

int SipEngine::activeCaptureDevice()const
{
    if(!endpoint_) return -1;
    std::lock_guard<std::mutex> audioLock(audioMutex_);
    return endpoint_->audDevManager().getCaptureDev();
}

int SipEngine::activePlaybackDevice()const
{
    if(!endpoint_) return -1;
    std::lock_guard<std::mutex> audioLock(audioMutex_);
    return endpoint_->audDevManager().getPlaybackDev();
}

AudioStatusInfo SipEngine::audioStatus()const
{
    AudioStatusInfo status;
    if(!endpoint_) return status;
    std::lock_guard<std::mutex> audioLock(audioMutex_);
    auto& audio=endpoint_->audDevManager();
    const auto devices=audio.enumDev2();
    status.captureId=audio.getCaptureDev();
    status.playbackId=audio.getPlaybackDev();
    status.soundActive=audio.sndIsActive();
    status.captureDevice=audioDeviceLabel(devices,status.captureId);
    status.playbackDevice=audioDeviceLabel(devices,status.playbackId);
#if defined(__linux__) || defined(__FreeBSD__)
    const auto route=systemAudioRoute();
    status.systemRoute=route.display;
    status.hotplugWatchAvailable=route.available;
    status.hotplugBackend=route.backend;
#endif
    status.autoSwitchEnabled=audioAutoSwitch_.load();
    return status;
}

void SipEngine::reopenAudioDevices()
{
    if(!endpoint_)throw std::runtime_error("SIP engine is not started");

    std::shared_ptr<CallSession> foreground;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it=calls_.find(foregroundId_);
        if(it!=calls_.end() && !it->second->snapshot().disconnected) foreground=it->second;
    }

    // Detach while the old sound-device media objects are still valid.
    if(foreground) foreground->detachAudio();

    try{
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        auto& audio=endpoint_->audDevManager();
        const auto before=audio.enumDev2();
        const int oldCapture=audio.getCaptureDev();
        const int oldPlayback=audio.getPlaybackDev();
        const auto captureKey=deviceKey(before,oldCapture);
        const auto playbackKey=deviceKey(before,oldPlayback);

        logger_.info("[AUDIO] Reopen requested: capture="+audioDeviceLabel(before,oldCapture)+
                     " playback="+audioDeviceLabel(before,oldPlayback));

        // setCaptureDev()/setPlaybackDev() alone do NOT reopen an already-open
        // PJSIP device. Force the conference bridge off the hardware first.
        audio.setNoDev();
        logger_.info("[AUDIO] Existing PJSIP sound device closed");

        // Refresh may change numeric indexes, so resolve the saved driver/name
        // identities again before assigning IDs.
        audio.refreshDevs();
        const auto refreshed=audio.enumDev2();
        const int capture=resolveDeviceKey(refreshed,captureKey,true);
        const int playback=resolveDeviceKey(refreshed,playbackKey,false);

        if(capture>=0){
            if(static_cast<std::size_t>(capture)>=refreshed.size() || refreshed[static_cast<std::size_t>(capture)].inputCount<=0)
                throw std::runtime_error("Selected capture device disappeared during audio refresh");
        }
        if(playback>=0){
            if(static_cast<std::size_t>(playback)>=refreshed.size() || refreshed[static_cast<std::size_t>(playback)].outputCount<=0)
                throw std::runtime_error("Selected playback device disappeared during audio refresh");
        }

        audio.setCaptureDev(capture);
        audio.setPlaybackDev(playback);
        // Mode 0 is normal full-duplex immediate-open: neither SPEAKER_ONLY nor
        // NO_IMMEDIATE_OPEN is set. This is the step r10 was missing.
        audio.setSndDevMode(0);
        if(!audio.sndIsActive()) throw std::runtime_error("PJSIP did not report the reopened sound device as active");

        logger_.info("[AUDIO] Reopened: capture="+audioDeviceLabel(refreshed,audio.getCaptureDev())+
                     " playback="+audioDeviceLabel(refreshed,audio.getPlaybackDev())+" status=ACTIVE");
    }catch(const pj::Error& e){
        logger_.warn("[AUDIO] PJSIP reopen failed: "+e.info());
        if(foreground){try{foreground->attachAudio();}catch(...){}}
        throw;
    }catch(const std::exception& e){
        logger_.warn(std::string("[AUDIO] Reopen failed: ")+e.what());
        if(foreground){try{foreground->attachAudio();}catch(...){}}
        throw;
    }

    if(foreground){
        foreground->attachAudio();
        logger_.info("[AUDIO] Foreground call "+std::to_string(foreground->getId())+" reattached after sound-device reopen");
    }
#if defined(__linux__) || defined(__FreeBSD__)
    {
        const auto route=systemAudioRoute();
        if(route.available){
            std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
            lastSystemAudioRoute_=route.signature;
        }
    }
#endif
}

void SipEngine::refreshAudioDevices()
{
    if(!endpoint_)throw std::runtime_error("SIP engine is not started");
    // A refresh invalidates numeric indices. Reopen preserves selections by
    // driver/name and is therefore the safe public refresh operation.
    reopenAudioDevices();
}

void SipEngine::selectAudioDevices(int captureId,int playbackId)
{
    if(!endpoint_)throw std::runtime_error("SIP engine is not started");
    {
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        auto& audio=endpoint_->audDevManager();const auto devices=audio.enumDev2();
        if(captureId<0||static_cast<std::size_t>(captureId)>=devices.size()||devices[static_cast<std::size_t>(captureId)].inputCount<=0)throw std::runtime_error("Invalid capture device ID");
        if(playbackId<0||static_cast<std::size_t>(playbackId)>=devices.size()||devices[static_cast<std::size_t>(playbackId)].outputCount<=0)throw std::runtime_error("Invalid playback device ID");
        audio.setCaptureDev(captureId);audio.setPlaybackDev(playbackId);
        logger_.info("[AUDIO] Devices selected; forcing reopen: capture="+std::to_string(captureId)+" playback="+std::to_string(playbackId));
    }
    reopenAudioDevices();
}

void SipEngine::selectPlaybackDevice(int playbackId)
{
    if(!endpoint_)throw std::runtime_error("SIP engine is not started");
    {
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        auto& audio=endpoint_->audDevManager();
        const auto devices=audio.enumDev2();
        if(playbackId<0 || static_cast<std::size_t>(playbackId)>=devices.size() ||
           devices[static_cast<std::size_t>(playbackId)].outputCount<=0)
            throw std::runtime_error("Invalid playback device ID");
        audio.setPlaybackDev(playbackId);
        logger_.info("[AUDIO] Output selected; forcing reopen: playback="+std::to_string(playbackId)+
                     " driver=\""+devices[static_cast<std::size_t>(playbackId)].driver+
                     "\" name=\""+devices[static_cast<std::size_t>(playbackId)].name+"\"");
    }
    reopenAudioDevices();
}

bool SipEngine::pollSystemAudioRoute()
{
#if defined(__linux__) || defined(__FreeBSD__)
    if(!endpoint_ || !started_ || !audioAutoSwitch_.load()) return false;
    const auto now=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    {
        std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
        if(lastSystemAudioPollMs_!=0 && now-lastSystemAudioPollMs_<750) return false;
        lastSystemAudioPollMs_=now;
    }

    const auto current=systemAudioRoute();
    if(!current.available){
        std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
        if(!systemAudioWatchUnavailableLogged_){
            systemAudioWatchUnavailableLogged_=true;
            logger_.warn("[AUDIO] Automatic hot-plug watcher unavailable on this Unix host. Manual audio-reopen remains available.");
        }
        return false;
    }

    std::string previous;
    {
        std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
        systemAudioWatchUnavailableLogged_=false;
        previous=lastSystemAudioRoute_;
        if(previous.empty()){lastSystemAudioRoute_=current.signature;return false;}
        if(previous==current.signature) return false;
        lastSystemAudioRoute_=current.signature;
    }

#ifdef __linux__
    // On Linux only auto-reopen when WaffleHouse-Client is following the desktop
    // PipeWire/default ALSA path. Do not override a deliberately pinned hw:/HDMI
    // device merely because the desktop's default port changed.
    bool followsSystem=false;
    {
        std::lock_guard<std::mutex> audioLock(audioMutex_);
        auto& audio=endpoint_->audDevManager();
        const auto devices=audio.enumDev2();
        const int cap=audio.getCaptureDev(),play=audio.getPlaybackDev();
        auto systemish=[&](int id){
            if(id<0 || static_cast<std::size_t>(id)>=devices.size()) return true;
            std::string name=devices[static_cast<std::size_t>(id)].name;
            std::transform(name.begin(),name.end(),name.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
            return name=="pipewire" || name=="default" || name.find("pipewire")!=std::string::npos;
        };
        followsSystem=systemish(cap) && systemish(play);
    }
    if(!followsSystem){
        logger_.info("[AUDIO] Linux system route changed but direct/manual audio devices are selected; no automatic reopen");
        return false;
    }
#endif

    logger_.info("[AUDIO] Automatic route change detected ("+current.backend+"): "+current.display);
    try{
        reopenAudioDevices();
    }catch(...){
        // Keep retrying on the next poll if a jack/device transition temporarily
        // made the audio device unavailable during insertion/removal.
        std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
        lastSystemAudioRoute_=previous;
        throw;
    }
    return true;
#else
    return false;
#endif
}

void SipEngine::setAudioAutoSwitch(bool enabled)
{
    audioAutoSwitch_.store(enabled);
#if defined(__linux__) || defined(__FreeBSD__)
    if(enabled){
        const auto route=systemAudioRoute();
        std::lock_guard<std::mutex> routeLock(audioRouteMutex_);
        lastSystemAudioRoute_=route.signature;
        lastSystemAudioPollMs_=0;
        systemAudioWatchUnavailableLogged_=false;
        logger_.info(std::string("[AUDIO] Automatic headset/device switching enabled")+
                     (route.available?" via "+route.backend:" (route watcher currently unavailable)"));
    }else{
        logger_.info("[AUDIO] Automatic headset/device switching disabled");
    }
#else
    (void)enabled;
#endif
}

bool SipEngine::audioAutoSwitchEnabled() const
{
    return audioAutoSwitch_.load();
}


void SipEngine::hangupAll()
{
    std::vector<std::shared_ptr<CallSession>> activeCalls;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for(const auto& item:calls_){
            if(!item.second->snapshot().disconnected) activeCalls.push_back(item.second);
        }
    }
    for(auto& call:activeCalls){
        try{ call->hangupCall(); }catch(...){}
    }
}

void SipEngine::setForeground(int id)
{
    std::shared_ptr<CallSession> oldCall,newCall;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if(foregroundId_==id) return;
        const auto oldIt=calls_.find(foregroundId_);
        if(oldIt!=calls_.end()) oldCall=oldIt->second;
        const auto newIt=calls_.find(id);
        if(newIt==calls_.end()) throw std::runtime_error("Call not found");
        if(newIt->second->snapshot().disconnected) throw std::runtime_error("Cannot foreground a disconnected call");
        newCall=newIt->second;
        foregroundId_=id;
    }
    if(oldCall) oldCall->setForeground(false);
    if(newCall) newCall->setForeground(true);
}

void SipEngine::clearForeground()
{
    std::shared_ptr<CallSession> oldCall;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it=calls_.find(foregroundId_);
        if(it!=calls_.end()) oldCall=it->second;
        foregroundId_=-1;
    }
    if(oldCall) oldCall->setForeground(false);
}

std::vector<CallSnapshot> SipEngine::calls()const
{
    std::vector<std::shared_ptr<CallSession>> callObjects;
    std::vector<CallSnapshot> snapshots;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshots.reserve(archivedCalls_.size()+calls_.size());
        for(const auto& item:archivedCalls_) snapshots.push_back(item.second.snapshot);
        for(const auto& item:calls_) callObjects.push_back(item.second);
    }
    for(auto& call:callObjects){
        auto state=call->snapshot();
        if(!stopping_ && !state.disconnected){
            call->refreshMediaInfo();
            state=call->snapshot();
        }
        snapshots.push_back(std::move(state));
    }
    std::sort(snapshots.begin(),snapshots.end(),[](const CallSnapshot& a,const CallSnapshot& b){
        if(a.createdMs!=b.createdMs) return a.createdMs<b.createdMs;
        return a.id<b.id;
    });
    return snapshots;
}

CallSnapshot SipEngine::callSnapshot(int id)const
{
    auto call=findCall(id);
    if(call){
        auto state=call->snapshot();
        if(!stopping_ && !state.disconnected){
            call->refreshMediaInfo();
            state=call->snapshot();
        }
        return state;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if(const auto* archived=findArchivedCallLocked(id)) return archived->snapshot;
    throw std::runtime_error("Call not found");
}

std::string SipEngine::mediaDump(int id)const
{
    if(stopping_) return "SIP engine is shutting down; live PJSIP media dump is unavailable.";
    auto call=findCall(id);
    if(!call){
        CallSnapshot state;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto* archived=findArchivedCallLocked(id);
            if(!archived) throw std::runtime_error("Call not found");
            state=archived->snapshot;
        }
        std::ostringstream out;
        out << "Call " << id << " is disconnected; live PJSIP media dump is unavailable.\n"
            << "Last media: codec=" << (state.codecName.empty()?"unknown":state.codecName);
        if(state.codecClockRate) out << "/" << state.codecClockRate;
        out << " remote=" << (state.remoteRtpAddress.empty()?"unknown":state.remoteRtpAddress)
            << " source=" << (state.sourceRtpAddress.empty()?"unknown":state.sourceRtpAddress)
            << " local=" << (state.localRtpAddress.empty()?"unknown":state.localRtpAddress);
        return out.str();
    }
    const auto state=call->snapshot();
    if(state.disconnected) return "Call is disconnecting; live PJSIP media dump is unavailable.";
    call->refreshMediaInfo();
    return call->mediaDump();
}

std::string SipEngine::sipLadder(int id)const
{
    const auto trace=sipTrace(id);std::ostringstream out;
    out<<"WaffleHouse-Client SIP ladder — call "<<id<<"\n"
         <<"LOCAL                                      REMOTE\n"
         <<"  |                                           |\n";
    for(const auto&e:trace){
        std::ostringstream label;label<<e.label;if(e.statusCode)label<<" ["<<e.statusCode<<"]";
        auto text=label.str();if(text.size()>34)text=text.substr(0,31)+"...";
        if(e.direction==SipDirection::Sent)out<<"  |---- "<<std::left<<std::setw(27)<<text<<" [SENT] -->|\n";
        else out<<"  |<--- "<<std::left<<std::setw(27)<<text<<" [RECEIVED] ---|\n";
    }
    out<<"  |                                           |\n";return out.str();
}

std::string SipEngine::callReport(int id)const
{
    const auto c=callSnapshot(id);std::ostringstream o;
    const double rxDen=static_cast<double>(c.rtpRxPackets+c.rtpRxLoss);const double lossPct=rxDen>0?100.0*c.rtpRxLoss/rxDen:0.0;
    o<<"WaffleHouse-Client 1.0.0 — SIP Inspection, Protocol Handling, Enumeration & Recon\nCALL DIAGNOSTIC REPORT\n\n"
     <<"Call ID:        "<<id<<"\nSIP Call-ID:    "<<c.callIdString<<"\nRemote URI:     "<<c.remoteUri<<"\nCaller ID:      "<<c.callerId<<"\nState:          "<<c.state<<"\nLast SIP:       "<<c.lastStatusCode<<" "<<c.lastReason<<"\n"
     <<"Codec:          "<<c.codecName<<(c.codecClockRate?"/"+std::to_string(c.codecClockRate):std::string{})<<"\n"
     <<"Microphone:     "<<(c.microphoneMuted?"MUTED":"live")<<"\n"
     <<"Audio devices:  capture="<<activeCaptureDevice()<<" playback="<<activePlaybackDevice()<<"\n"
     <<"Local RTP:      "<<c.localRtpAddress<<"\nRemote RTP:     "<<c.remoteRtpAddress<<"\nSource RTP:     "<<c.sourceRtpAddress<<"\n"
     <<"RTP TX:         "<<c.rtpTxPackets<<" packets / "<<c.rtpTxBytes<<" bytes / loss="<<c.rtpTxLoss<<" discard="<<c.rtpTxDiscard<<"\n"
     <<"RTP RX:         "<<c.rtpRxPackets<<" packets / "<<c.rtpRxBytes<<" bytes / loss="<<c.rtpRxLoss<<" discard="<<c.rtpRxDiscard<<"\n"
     <<"RX loss:        "<<std::fixed<<std::setprecision(2)<<lossPct<<"%\n"
     <<"Jitter TX/RX:   "<<std::setprecision(1)<<c.txJitterMs<<" / "<<c.rxJitterMs<<" ms\n"
     <<"RTT:            "<<c.rttMs<<" ms\nJitter buffer:  "<<c.jitterBufferDelayMs<<" ms\n"
     <<"Est. R-factor:  "<<c.estimatedRFactor<<"\nEst. MOS:       "<<c.estimatedMos<<" (engineering estimate; not PESQ/POLQA)\n\n";
    if(c.purpose==CallPurpose::Phone){try{o<<sipLadder(id)<<"\n";}catch(...){} }
    return o.str();
}

void SipEngine::exportCallReport(int id,const std::string& path)const
{
    const std::filesystem::path p(path);if(p.has_parent_path()){std::error_code ec;std::filesystem::create_directories(p.parent_path(),ec);if(ec&&!std::filesystem::is_directory(p.parent_path()))throw std::runtime_error("Unable to create report directory: "+ec.message());}
    std::ofstream out(path,std::ios::trunc);if(!out)throw std::runtime_error("Unable to create call report: "+path);out<<callReport(id);out.close();
#ifndef _WIN32
    (void)::chmod(path.c_str(),S_IRUSR|S_IWUSR);
#endif
}

std::vector<SipTraceEntry> SipEngine::sipTrace(int id)const
{
    if(auto call=findCall(id)){
        if(call->snapshot().purpose!=CallPurpose::Phone) throw std::runtime_error("SIP trace is limited to normal Phone calls");
        return call->sipTrace();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* archived=findArchivedCallLocked(id);
    if(!archived) throw std::runtime_error("Call not found");
    if(archived->snapshot.purpose!=CallPurpose::Phone) throw std::runtime_error("SIP trace is limited to normal Phone calls");
    return archived->sipTrace;
}
void SipEngine::startSipTraceFile(int id,const std::string& path){requirePhoneCall(id)->startSipTraceFile(path);logger_.info("SIP trace file started: "+path);}
void SipEngine::stopSipTraceFile(int id){auto c=requirePhoneCall(id);auto p=c->sipTracePath();c->stopSipTraceFile();logger_.info("SIP trace file stopped"+(p.empty()?std::string{}:": "+p));}
bool SipEngine::sipTraceRecording(int id)const
{
    if(auto call=findCall(id)) return call->snapshot().purpose==CallPurpose::Phone && call->sipTraceRecording();
    std::lock_guard<std::mutex> lock(mutex_);
    if(findArchivedCallLocked(id)) return false;
    throw std::runtime_error("Call not found");
}
std::string SipEngine::sipTracePath(int id)const
{
    if(auto call=findCall(id)) return call->snapshot().purpose==CallPurpose::Phone ? call->sipTracePath() : std::string{};
    std::lock_guard<std::mutex> lock(mutex_);
    const auto* archived=findArchivedCallLocked(id);
    if(!archived) throw std::runtime_error("Call not found");
    return archived->sipTracePath;
}

void SipEngine::onSipMessage(SipTraceEntry entry)
{
    std::shared_ptr<CallSession> call;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto mapped=callIdIndex_.find(entry.callIdString);
        if(mapped!=callIdIndex_.end()){
            const auto it=calls_.find(mapped->second);
            if(it!=calls_.end()) call=it->second;
        }
        if(!call){
            // The monitor can observe the initial INVITE before makeCall()/the
            // incoming-call callback has registered its CallSession. Buffer
            // only INVITE transactions for that short race window; buffering
            // arbitrary unmatched OPTIONS/NOTIFY/etc. would retain unrelated
            // raw SIP traffic and grow memory unnecessarily.
            if(entry.method=="INVITE" && !entry.callIdString.empty()){
                auto& queue=pendingSip_[entry.callIdString];
                if(queue.size()<32) queue.push_back(std::move(entry));
                if(pendingSip_.size()>64) pendingSip_.erase(pendingSip_.begin());
            }
            return;
        }
    }
    if(call->snapshot().purpose==CallPurpose::Phone) call->recordSipMessage(std::move(entry));
}

void SipEngine::onIncomingCall(const std::string& accountId,int id)
{
    std::lock_guard<std::mutex> creationLock(callCreateMutex_);
    if(stopping_ || !started_){
        logger_.warn("Ignored incoming call during SIP engine shutdown: account="+accountId+" id="+std::to_string(id));
        return;
    }
    SipAccount* account=nullptr;
    SipProfile profile;
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        const auto it=accounts_.find(accountId);
        if(it==accounts_.end() || !it->second.account){
            logger_.warn("Ignored incoming call for unknown SIP account: "+accountId);
            return;
        }
        account=it->second.account.get();
        profile=it->second.profile;
    }
    auto call=std::make_shared<CallSession>(*account,logger_,CallDirection::Incoming,CallPurpose::Phone,id);
    call->setAccountIdentity(accountId,profile.name);
    call->setUpdateCallback([this](int callId){ onCallUpdated(callId); });
    addCall(call);
    try{ logger_.info("Incoming call "+std::to_string(id)+" account="+accountId+" from "+call->getInfo().remoteUri); }catch(...){}
}

void SipEngine::onRegistrationState(const std::string& accountId,bool active,int code,const std::string& reason)
{
    if(stopping_) return;
    std::ostringstream status;
    status<<(active?"Registered":"Not registered")<<" ("<<code<<" "<<reason<<")";
    const auto now=std::chrono::system_clock::now();const auto tt=std::chrono::system_clock::to_time_t(now);std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm,&tt);
#else
    localtime_r(&tt,&tm);
#endif
    std::ostringstream hist;hist<<std::put_time(&tm,"%Y-%m-%d %H:%M:%S")<<"  ["<<accountId<<"] "<<status.str();
    {
        std::lock_guard<std::mutex> lock(accountMutex_);
        const auto it=accounts_.find(accountId);
        if(it!=accounts_.end()){
            it->second.registered=active;
            it->second.registrationText=status.str();
            it->second.registrationHistory.push_back(hist.str());
            if(it->second.registrationHistory.size()>100)
                it->second.registrationHistory.erase(it->second.registrationHistory.begin(),it->second.registrationHistory.begin()+25);
        }
    }
    {
        std::lock_guard<std::mutex> lock(regMutex_);
        registrationHistory_.push_back(hist.str());
        if(registrationHistory_.size()>250) registrationHistory_.erase(registrationHistory_.begin(),registrationHistory_.begin()+50);
    }
    updateAggregateRegistration();
    logger_.info("["+accountId+"] "+status.str());
}

std::vector<std::string> SipEngine::registrationHistory()const
{
    std::lock_guard<std::mutex> lock(regMutex_);return registrationHistory_;
}

void SipEngine::archiveDisconnectedCall(const std::shared_ptr<CallSession>& call,const CallSnapshot& state)
{
    ArchivedCall archived;
    archived.snapshot=state;
    if(state.purpose==CallPurpose::Phone){
        archived.sipTrace=call->sipTrace();
        archived.sipTracePath=call->sipTracePath();
        if(call->sipTraceRecording()) call->stopSipTraceFile();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if(state.id!=PJSUA_INVALID_ID) archivedCalls_[state.id]=std::move(archived);
    for(auto it=callIdIndex_.begin();it!=callIdIndex_.end();){
        if(it->second==state.id) it=callIdIndex_.erase(it); else ++it;
    }
    calls_.erase(state.id);
    if(foregroundId_==state.id) foregroundId_=-1;
}

void SipEngine::onCallUpdated(int id)
{
    auto call=findCall(id);
    if(!call) return;
    auto state=call->snapshot();
    if(!state.disconnected){
        call->refreshMediaInfo();
        state=call->snapshot();
    }

    std::vector<SipTraceEntry> pending;
    if(!state.callIdString.empty()){
        std::lock_guard<std::mutex> lock(mutex_);
        // A Call-ID can become available after the CallSession was inserted.
        // Keep the wire-monitor lookup synchronized with the latest snapshot.
        for(auto it=callIdIndex_.begin();it!=callIdIndex_.end();){
            if(it->second==id && it->first!=state.callIdString) it=callIdIndex_.erase(it);
            else ++it;
        }
        callIdIndex_[state.callIdString]=id;
        const auto it=pendingSip_.find(state.callIdString);
        if(it!=pendingSip_.end()){
            if(state.purpose==CallPurpose::Phone) pending=std::move(it->second);
            pendingSip_.erase(it);
        }
    }
    if(state.purpose==CallPurpose::Phone){
        for(auto& entry:pending) call->recordSipMessage(std::move(entry));
    }

    if(state.disconnected){
        archiveDisconnectedCall(call,state);
        // PJSIP has already removed its Call user-data association before the
        // DISCONNECTED callback. Keep only WaffleHouse-Client's immutable diagnostics;
        // the live wrapper is allowed to die without any further PJSUA2 calls.
    }
}
} // namespace trunkmonkey
