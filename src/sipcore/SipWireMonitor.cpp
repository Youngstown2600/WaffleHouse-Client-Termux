#include "trunkmonkey/SipWireMonitor.h"
#include "trunkmonkey/Logger.h"
#include "trunkmonkey/SipEngine.h"
#include "trunkmonkey/SipTrace.h"
#include <pjsua-lib/pjsua.h>
#include <pjsip/sip_msg.h>
#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace trunkmonkey {
SipWireMonitor* SipWireMonitor::active_ = nullptr;
std::mutex SipWireMonitor::activeMutex_;

namespace {
std::string pjstr(const pj_str_t& value)
{
    return (value.ptr && value.slen > 0)
        ? std::string(value.ptr, static_cast<std::size_t>(value.slen))
        : std::string{};
}

std::uint64_t nowMs()
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

constexpr std::size_t kMaxStoredSipMessage = 131072;

std::string printMessage(const pjsip_msg* msg)
{
    std::vector<char> buffer(kMaxStoredSipMessage);
    const auto length = pjsip_msg_print(msg, buffer.data(), buffer.size());
    return length > 0
        ? std::string(buffer.data(), static_cast<std::size_t>(length))
        : std::string{"<SIP message too large to render>\n"};
}

std::string storeRawMessage(const char* raw, std::size_t rawLen, const pjsip_msg* msg)
{
    if (!raw || rawLen == 0) {
        return printMessage(msg);
    }
    const auto stored = std::min(rawLen, kMaxStoredSipMessage);
    std::string value(raw, stored);
    if (stored < rawLen) {
        value += "\n<WaffleHouse-Client: SIP message truncated at 128 KiB>\n";
    }
    return value;
}
}

SipWireMonitor::SipWireMonitor(SipEngine& engine, Logger& logger)
    : engine_(engine), logger_(logger)
{
}

SipWireMonitor::~SipWireMonitor()
{
    stop();
}

void SipWireMonitor::start()
{
    if (running_) {
        return;
    }

    module_ = {};
    module_.name = pj_str(const_cast<char*>("mod-wafflehouse-trace"));
    module_.id = -1;
    module_.priority = PJSIP_MOD_PRIORITY_TRANSPORT_LAYER - 1;
    module_.on_rx_request = &SipWireMonitor::onRxRequest;
    module_.on_rx_response = &SipWireMonitor::onRxResponse;
    module_.on_tx_request = &SipWireMonitor::onTxRequest;
    module_.on_tx_response = &SipWireMonitor::onTxResponse;

    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (active_ && active_ != this) {
            throw std::runtime_error("A SIP wire monitor is already active");
        }
        active_ = this;
    }

    const auto status = pjsip_endpt_register_module(pjsua_get_pjsip_endpt(), &module_);
    if (status != PJ_SUCCESS) {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (active_ == this) {
            active_ = nullptr;
        }
        throw std::runtime_error("Unable to register WaffleHouse-Client SIP monitor");
    }

    running_ = true;
    logger_.info("Per-call SIP wire monitor enabled");
}

void SipWireMonitor::stop()
{
    if (!running_) {
        return;
    }

    // Clear the dispatch target first. Taking this mutex waits for any callback
    // already inside dispatch() to finish before the monitor can be destroyed.
    {
        std::lock_guard<std::mutex> lock(activeMutex_);
        if (active_ == this) {
            active_ = nullptr;
        }
    }

    if (auto* endpoint = pjsua_get_pjsip_endpt()) {
        pjsip_endpt_unregister_module(endpoint, &module_);
    }
    running_ = false;
}

void SipWireMonitor::dispatch(pjsip_msg* msg, bool sent, const char* raw, std::size_t rawLen)
{
    std::lock_guard<std::mutex> lock(activeMutex_);
    if (active_) {
        active_->process(msg, sent, raw, rawLen);
    }
}

pj_bool_t SipWireMonitor::onRxRequest(pjsip_rx_data* rdata)
{
    if (rdata && rdata->msg_info.msg) {
        dispatch(rdata->msg_info.msg, false, rdata->msg_info.msg_buf,
                 static_cast<std::size_t>(rdata->msg_info.len));
    }
    return PJ_FALSE;
}

pj_bool_t SipWireMonitor::onRxResponse(pjsip_rx_data* rdata)
{
    if (rdata && rdata->msg_info.msg) {
        dispatch(rdata->msg_info.msg, false, rdata->msg_info.msg_buf,
                 static_cast<std::size_t>(rdata->msg_info.len));
    }
    return PJ_FALSE;
}

pj_status_t SipWireMonitor::onTxRequest(pjsip_tx_data* tdata)
{
    if (tdata && tdata->msg) {
        const auto length = (tdata->buf.start && tdata->buf.cur && tdata->buf.cur >= tdata->buf.start)
            ? static_cast<std::size_t>(tdata->buf.cur - tdata->buf.start)
            : 0;
        dispatch(tdata->msg, true, tdata->buf.start, length);
    }
    return PJ_SUCCESS;
}

pj_status_t SipWireMonitor::onTxResponse(pjsip_tx_data* tdata)
{
    if (tdata && tdata->msg) {
        const auto length = (tdata->buf.start && tdata->buf.cur && tdata->buf.cur >= tdata->buf.start)
            ? static_cast<std::size_t>(tdata->buf.cur - tdata->buf.start)
            : 0;
        dispatch(tdata->msg, true, tdata->buf.start, length);
    }
    return PJ_SUCCESS;
}

void SipWireMonitor::process(pjsip_msg* msg, bool sent, const char* raw, std::size_t rawLen)
{
    if (!msg) {
        return;
    }

    auto* callId = PJSIP_MSG_CID_HDR(msg);
    auto* cseq = PJSIP_MSG_CSEQ_HDR(msg);
    if (!callId || !cseq) {
        return;
    }

    SipTraceEntry entry;
    entry.timestampMs = nowMs();
    entry.direction = sent ? SipDirection::Sent : SipDirection::Received;
    entry.callIdString = pjstr(callId->id);
    entry.cseq = static_cast<std::uint32_t>(cseq->cseq);
    entry.method = pjstr(cseq->method.name);
    entry.rawMessage = storeRawMessage(raw, rawLen, msg);

    if (msg->type == PJSIP_RESPONSE_MSG) {
        entry.statusCode = msg->line.status.code;
        entry.reason = pjstr(msg->line.status.reason);
    } else {
        if (entry.method.empty()) {
            entry.method = pjstr(msg->line.req.method.name);
        }
        if (entry.method == "INVITE") {
            auto* to = PJSIP_MSG_TO_HDR(msg);
            entry.inDialogRequest = to && to->tag.slen > 0;
        }
    }

    engine_.onSipMessage(std::move(entry));
}
} // namespace trunkmonkey
