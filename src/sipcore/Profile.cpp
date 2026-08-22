#include "trunkmonkey/Profile.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace trunkmonkey {
namespace {
std::string trim(std::string s)
{
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!s.empty() && space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

bool parseBool(const std::string& key, const std::string& value)
{
    const auto v = lower(trim(value));
    if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
    if (v == "0" || v == "false" || v == "no" || v == "off") return false;
    throw std::runtime_error("Invalid boolean for " + key + ": " + value);
}

unsigned long long parseUnsigned(const std::string& key, const std::string& value,
                                 unsigned long long minValue, unsigned long long maxValue)
{
    const auto v = trim(value);
    if (v.empty()) throw std::runtime_error("Missing numeric value for " + key);
    std::size_t used = 0;
    unsigned long long n = 0;
    try { n = std::stoull(v, &used, 10); }
    catch (...) { throw std::runtime_error("Invalid numeric value for " + key + ": " + value); }
    if (used != v.size() || n < minValue || n > maxValue)
        throw std::runtime_error("Out-of-range numeric value for " + key + ": " + value);
    return n;
}

std::string ensureSipUri(const std::string& value)
{
    const auto v = trim(value);
    if (v.empty()) return {};
    if (v.rfind("sip:", 0) == 0 || v.rfind("sips:", 0) == 0) return v;
    return "sip:" + v;
}

std::unordered_map<std::string,std::string> readKeyValues(const std::string& path)
{
    std::ifstream in(path);
    if (!in) throw std::runtime_error("Unable to open profile: " + path);
    std::unordered_map<std::string,std::string> kv;
    std::string line;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        kv[lower(trim(line.substr(0, pos)))] = trim(line.substr(pos + 1));
    }
    return kv;
}
}

std::string toString(Transport v)
{
    switch (v) { case Transport::Udp: return "udp"; case Transport::Tcp: return "tcp"; case Transport::Tls: return "tls"; }
    return "udp";
}
std::string toString(IdentityMode v)
{
    switch (v) { case IdentityMode::From: return "from"; case IdentityMode::Pai: return "pai"; case IdentityMode::Rpid: return "rpid"; case IdentityMode::FromAndPai: return "from+pai"; }
    return "from";
}
Transport transportFromString(const std::string& value)
{
    const auto v=lower(trim(value));
    if(v=="udp") return Transport::Udp;
    if(v=="tcp") return Transport::Tcp;
    if(v=="tls") return Transport::Tls;
    throw std::runtime_error("Invalid transport: " + value + " (expected udp, tcp, or tls)");
}
IdentityMode identityModeFromString(const std::string& value)
{
    const auto v=lower(trim(value));
    if(v=="from") return IdentityMode::From;
    if(v=="pai") return IdentityMode::Pai;
    if(v=="rpid") return IdentityMode::Rpid;
    if(v=="from+pai" || v=="frompai") return IdentityMode::FromAndPai;
    throw std::runtime_error("Invalid identity_mode: " + value + " (expected from, pai, rpid, or from+pai)");
}

SipProfile ProfileStore::defaults()
{
    return SipProfile{};
}

SipProfile ProfileStore::loadDraft(const std::string& path)
{
    const auto kv = readKeyValues(path);
    const auto get = [&](const char* key, const std::string& fallback = std::string{}) {
        const auto it = kv.find(key);
        return it == kv.end() ? fallback : it->second;
    };

    SipProfile p = defaults();
    p.name = get("name", p.name);
    p.sipDomain = get("sip_domain");
    p.registrar = ensureSipUri(get("registrar"));
    p.username = get("username");
    p.authUsername = get("auth_username");
    p.password = get("password");
    p.displayName = get("display_name", p.displayName);
    p.outboundProxy = ensureSipUri(get("outbound_proxy"));
    p.callerIdDomain = get("caller_id_domain");
    p.dialPrefix = trim(get("dial_prefix"));
    p.stunServer = get("stun_server");
    p.transport = transportFromString(get("transport", "udp"));
    p.identityMode = identityModeFromString(get("identity_mode", "from"));
    p.localSipPort = static_cast<std::uint16_t>(parseUnsigned("local_sip_port", get("local_sip_port", "5060"), 1, 65535));
    p.registrationExpires = static_cast<unsigned>(parseUnsigned(
        "registration_expires", get("registration_expires", "300"), 1,
        std::numeric_limits<unsigned>::max()));
    p.useIce = parseBool("use_ice", get("use_ice", "false"));
    p.enableSrtp = parseBool("enable_srtp", get("enable_srtp", "false"));
    return p;
}

void ProfileStore::validate(const SipProfile& p)
{
    if (trim(p.sipDomain).empty()) throw std::runtime_error("profile missing sip_domain");
    if (trim(p.username).empty()) throw std::runtime_error("profile missing username");
    if (p.localSipPort == 0) throw std::runtime_error("local_sip_port must be 1-65535");
    if (p.registrationExpires == 0) throw std::runtime_error("registration_expires must be greater than zero");
    if (p.dialPrefix.find_first_of(" \t\r\n@<>:") != std::string::npos)
        throw std::runtime_error("dial_prefix must be a plain dial-string prefix (for example 9 or 4071)");
}

bool ProfileStore::isConfigured(const SipProfile& p) noexcept
{
    try {
        validate(p);
        const auto domain=lower(trim(p.sipDomain));
        const auto password=trim(p.password);
        if(domain=="pbx.example.net" || password=="CHANGE_ME") return false;
        return true;
    } catch (...) { return false; }
}

SipProfile ProfileStore::load(const std::string& path)
{
    auto p=loadDraft(path);
    validate(p);
    if (p.registrar.empty()) p.registrar = "sip:" + p.sipDomain;
    if (p.authUsername.empty()) p.authUsername = p.username;
    if (p.callerIdDomain.empty()) p.callerIdDomain = p.sipDomain;
    return p;
}

bool ProfileStore::createDefaultIfMissing(const std::string& path)
{
    if (std::filesystem::exists(path)) return false;
    save(defaults(), path);
    return true;
}

void ProfileStore::save(const SipProfile& p, const std::string& path)
{
    const std::filesystem::path filePath(path);
    if (filePath.has_parent_path()) {
        std::error_code ec;
        std::filesystem::create_directories(filePath.parent_path(), ec);
        if (ec && !std::filesystem::is_directory(filePath.parent_path()))
            throw std::runtime_error("Unable to create profile directory: " + filePath.parent_path().string());
#ifndef _WIN32
        (void)::chmod(filePath.parent_path().c_str(), S_IRWXU);
#endif
    }

    // Write through a sibling temporary file so an interrupted save does not
    // leave a half-written credential/profile file behind.
    const auto tempPath=filePath.string()+".tmp";
    {
        std::ofstream out(tempPath, std::ios::trunc);
        if (!out) throw std::runtime_error("Unable to write profile: " + path);
#ifndef _WIN32
        (void)::chmod(tempPath.c_str(), S_IRUSR | S_IWUSR);
#endif
        out << "# WaffleHouse-Client SIP profile\n"
            << "name=" << p.name << "\n"
            << "sip_domain=" << p.sipDomain << "\n"
            << "registrar=" << p.registrar << "\n"
            << "username=" << p.username << "\n"
            << "auth_username=" << p.authUsername << "\n"
            << "password=" << p.password << "\n"
            << "display_name=" << p.displayName << "\n"
            << "outbound_proxy=" << p.outboundProxy << "\n"
            << "caller_id_domain=" << p.callerIdDomain << "\n"
            << "dial_prefix=" << p.dialPrefix << "\n"
            << "stun_server=" << p.stunServer << "\n"
            << "transport=" << toString(p.transport) << "\n"
            << "identity_mode=" << toString(p.identityMode) << "\n"
            << "local_sip_port=" << p.localSipPort << "\n"
            << "registration_expires=" << p.registrationExpires << "\n"
            << "use_ice=" << (p.useIce ? "true" : "false") << "\n"
            << "enable_srtp=" << (p.enableSrtp ? "true" : "false") << "\n";
        out.flush();
        if (!out) throw std::runtime_error("Unable to finish writing profile: " + path);
    }

    std::error_code ec;
    std::filesystem::rename(tempPath, filePath, ec);
#ifdef _WIN32
    if (ec) {
        // Windows may refuse to replace an existing destination. This bundle
        // targets Unix, but keep the shared profile helper portable.
        std::error_code removeError;
        std::filesystem::remove(filePath, removeError);
        ec.clear();
        std::filesystem::rename(tempPath, filePath, ec);
    }
#endif
    if (ec) {
        const auto message=ec.message();
        std::filesystem::remove(tempPath);
        throw std::runtime_error("Unable to replace profile: " + path + ": " + message);
    }
#ifndef _WIN32
    (void)::chmod(path.c_str(), S_IRUSR | S_IWUSR);
#endif
}
} // namespace trunkmonkey
