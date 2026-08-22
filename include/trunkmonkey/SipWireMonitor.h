#pragma once
#include <pjsip/sip_module.h>
#include <cstddef>
#include <mutex>

namespace trunkmonkey {
class Logger;
class SipEngine;

class SipWireMonitor {
public:
    SipWireMonitor(SipEngine& engine, Logger& logger);
    ~SipWireMonitor();
    void start();
    void stop();
    bool running() const { return running_; }

private:
    static pj_bool_t onRxRequest(pjsip_rx_data* rdata);
    static pj_bool_t onRxResponse(pjsip_rx_data* rdata);
    static pj_status_t onTxRequest(pjsip_tx_data* tdata);
    static pj_status_t onTxResponse(pjsip_tx_data* tdata);
    static void dispatch(pjsip_msg* msg, bool sent, const char* raw = nullptr, std::size_t rawLen = 0);
    void process(pjsip_msg* msg, bool sent, const char* raw = nullptr, std::size_t rawLen = 0);

    SipEngine& engine_;
    Logger& logger_;
    pjsip_module module_{};
    bool running_{false};
    static SipWireMonitor* active_;
    static std::mutex activeMutex_;
};
}
