#pragma once
#include <cstdint>
#include <set>
#include <string>
namespace trunkmonkey {
enum class SipDirection { Sent, Received };
struct SipTraceEntry {
    std::uint64_t timestampMs{0};
    int callId{-1};
    std::string callIdString;
    SipDirection direction{SipDirection::Sent};
    std::string method;
    std::string label;
    int statusCode{0};
    std::string reason;
    std::uint32_t cseq{0};
    // True only for an INVITE request that already has a To-tag, i.e. an
    // in-dialog INVITE. This lets WaffleHouse distinguish a real re-INVITE
    // from an authenticated/retried initial INVITE whose CSeq changed.
    bool inDialogRequest{false};
    std::string rawMessage;
};
struct SipTraceClassifierState {
    std::uint32_t initialInviteCseq{0};
    std::set<std::uint32_t> reInviteCseqs;
};
void classifySipTraceEntry(SipTraceEntry& entry,SipTraceClassifierState& state);
}
