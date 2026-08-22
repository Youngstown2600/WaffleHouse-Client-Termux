#pragma once
#include <pjsua2.hpp>
#include <string>
namespace trunkmonkey {
class SipEngine; class Logger;
class SipAccount final:public pj::Account {
public:
    SipAccount(SipEngine& engine,Logger& logger,std::string accountId);
    ~SipAccount()override;
    const std::string& accountId()const{return accountId_;}
    void onRegState(pj::OnRegStateParam& prm)override;
    void onIncomingCall(pj::OnIncomingCallParam& prm)override;
private:
    SipEngine& engine_; Logger& logger_; std::string accountId_;
};
}
