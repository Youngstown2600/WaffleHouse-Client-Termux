#pragma once
#include <cstdint>
#include <string>
namespace trunkmonkey {
enum class CallDirection { Incoming, Outgoing };
enum class CallPurpose { Phone };
struct CallSnapshot {
    int id{-1};
    CallDirection direction{CallDirection::Outgoing};
    CallPurpose purpose{CallPurpose::Phone};
    std::string accountId;
    std::string accountName;
    std::string callIdString;
    std::string remoteUri;
    std::string callerId;
    std::string state{"UNKNOWN"};
    int lastStatusCode{0};
    std::string lastReason;
    bool connected{false};
    bool disconnected{false};
    bool foreground{false};
    bool mediaActive{false};
    bool microphoneMuted{false};
    std::string localRtpAddress;
    std::string localRtcpAddress;
    std::string remoteRtpAddress;
    std::string remoteRtcpAddress;
    std::string sourceRtpAddress;
    std::string sourceRtcpAddress;
    std::string codecName;
    unsigned codecClockRate{0};
    std::uint64_t rtpTxPackets{0}, rtpRxPackets{0};
    std::uint64_t rtpTxBytes{0}, rtpRxBytes{0};
    std::uint64_t rtpTxLoss{0}, rtpRxLoss{0};
    std::uint64_t rtpTxDiscard{0}, rtpRxDiscard{0};
    double txJitterMs{0.0}, rxJitterMs{0.0}, rttMs{0.0};
    unsigned jitterBufferDelayMs{0};
    double estimatedRFactor{0.0}, estimatedMos{0.0};
    std::uint64_t createdMs{0}, connectedMs{0}, disconnectedMs{0};
};
}
