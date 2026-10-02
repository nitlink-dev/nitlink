#include "pipe_transport.h"
#include <algorithm>
#include "discord_rpc.h"
#include <debugapi.h>
#include <sstream>
#include <vector>
#include <chrono>
#include <thread>

namespace NitLink {

static void RPCLog(const std::wstring& msg) {
    OutputDebugStringW((L"[NitLink/Discord] " + msg + L"\n").c_str());
}

DiscordRPC::DiscordRPC() : m_stopEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

DiscordRPC::~DiscordRPC()
{
    Disconnect();
    if (m_stopEvent) CloseHandle(m_stopEvent);
}

std::string DiscordRPC::WideToUtf8(const std::wstring& w)
{
    if (w.empty() || w.size() > 1024) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string DiscordRPC::EscapeJson(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool DiscordRPC::Connect(const std::string& applicationId)
{
    Disconnect();
    if (!m_stopEvent || !ResetEvent(m_stopEvent) || applicationId.empty() || applicationId.size() > 32 ||
        !std::all_of(applicationId.begin(), applicationId.end(), [](char c) { return c >= '0' && c <= '9'; }))
        return false;

    m_applicationId = applicationId;

    if (!OpenConnection()) return false;
    m_running = true;
    m_worker = std::thread(&DiscordRPC::WorkerLoop, this);
    return true;
}

bool DiscordRPC::OpenConnection()
{
    m_connected = false;
    if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
    m_pipe = INVALID_HANDLE_VALUE;
    if (WaitForSingleObject(m_stopEvent, 0) != WAIT_TIMEOUT) return false;
    if (m_pipeOpener) {
        m_pipe = m_pipeOpener();
    } else {
        for (int i = 0; i < 10; ++i) {
            wchar_t name[64];
            swprintf(name, 64, L"\\\\.\\pipe\\discord-ipc-%d", i);
            m_pipe = OpenRpcPipe(name);
            if (m_pipe != INVALID_HANDLE_VALUE) break;
        }
    }
    if (m_pipe == INVALID_HANDLE_VALUE) return false;
    const std::string handshake = "{\"v\":1,\"client_id\":\"" +
        EscapeJson(m_applicationId) + "\"}";
    Opcode op{};
    std::string payload;
    if (!SendFrame(Opcode::Handshake, handshake) || !ReadFrame(op, payload) || op != Opcode::Frame) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        RPCLog(L"Handshake failed; connection closed");
        return false;
    }
    m_connected = true;
    RPCLog(L"Connect: success");
    return true;
}

void DiscordRPC::Disconnect()
{
    m_running = false;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_worker.joinable()) m_worker.join();

    if (m_pipe != INVALID_HANDLE_VALUE) {
        CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
    }
    m_connected = false;
    RPCLog(L"Disconnected");
}

bool DiscordRPC::SendFrame(Opcode op, const std::string& payload)
{
    return WriteRpcFrame(m_pipe, m_stopEvent, static_cast<uint32_t>(op), payload);
}

bool DiscordRPC::ReadFrame(Opcode& op, std::string& payload)
{
    uint32_t opcode = 0;
    if (!ReadRpcFrame(m_pipe, m_stopEvent, opcode, payload)) return false;
    op = static_cast<Opcode>(opcode);
    return op != Opcode::Close;
}

void DiscordRPC::SetActivity(const std::wstring& details,
                              const std::wstring& state,
                              std::chrono::system_clock::time_point startTime,
                              const std::string& largeImageKey,
                              const std::wstring& largeImageText)
{
    if (details.size() > 1024 || state.size() > 1024 || largeImageText.size() > 1024 || largeImageKey.size() > 1024)
        return;
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_pendingDetails        = details;
    m_pendingState          = state;
    m_pendingStartTimeUnix  = std::chrono::duration_cast<std::chrono::seconds>(
        startTime.time_since_epoch()).count();
    m_pendingLargeImageKey  = largeImageKey;
    m_pendingLargeImageText = largeImageText;
    m_activityDirty         = true;
    m_clearRequested        = false;
}

void DiscordRPC::ClearActivity()
{
    std::lock_guard<std::mutex> lock(m_activityMutex);
    m_clearRequested = true;
    m_activityDirty  = false;
}

void DiscordRPC::WorkerLoop()
{
    // Replies are drained independently of the two-second activity send limit.
    auto lastSend = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    uint64_t nonce = 1000;
    RpcFrameReader reader;
    DWORD reconnectDelay = 2000;
    auto nextReconnect = std::chrono::steady_clock::now();
    auto connectedSince = nextReconnect;
    bool sentActivity = false, lastWasClear = false;
    const auto disconnected = [&](const wchar_t* reason) {
        if (WaitForSingleObject(m_stopEvent, 0) != WAIT_TIMEOUT) return;
        RPCLog(std::wstring(reason) + L"; retry in " + std::to_wstring(reconnectDelay / 1000) + L" seconds");
        m_connected = false;
        if (m_pipe != INVALID_HANDLE_VALUE) CloseHandle(m_pipe);
        m_pipe = INVALID_HANDLE_VALUE;
        reader.Reset();
        nextReconnect = std::chrono::steady_clock::now() + std::chrono::milliseconds(reconnectDelay);
        reconnectDelay = std::min<DWORD>(reconnectDelay * 2, 30000);
        std::lock_guard<std::mutex> lock(m_activityMutex);
        if (sentActivity && !m_activityDirty && !m_clearRequested) {
            m_clearRequested = lastWasClear;
            m_activityDirty = !lastWasClear;
        }
    };

    while (m_running) {
        if (WaitForSingleObject(m_stopEvent, 500) != WAIT_TIMEOUT) break;
        auto now = std::chrono::steady_clock::now();
        if (!m_connected) {
            if (now < nextReconnect) continue;
            if (!OpenConnection()) { disconnected(L"Discord reconnect unavailable"); continue; }
            reader.Reset();
            connectedSince = std::chrono::steady_clock::now();
        }
        // A slow or absent reply retains presence. Only complete frames are
        // interpreted, with at most four bounded frames drained per pass.
        for (unsigned count = 0; count < 4; ++count) {
            uint32_t opcode = 0;
            std::string payload;
            const auto result = reader.Poll(m_pipe, m_stopEvent, opcode, payload);
            if (result == RpcReadResult::Pending) break;
            if (result == RpcReadResult::Failed || opcode == static_cast<uint32_t>(Opcode::Close) ||
                (opcode == static_cast<uint32_t>(Opcode::Ping) && !SendFrame(Opcode::Pong, payload))) {
                disconnected(L"Discord stream closed or invalid");
                break;
            }
        }
        if (!m_connected) continue;
        now = std::chrono::steady_clock::now();
        if (now - connectedSince >= std::chrono::seconds(60)) reconnectDelay = 2000;
        if (now - lastSend < std::chrono::seconds(2)) continue;

        std::wstring details, state, largeText;
        std::string  largeKey;
        int64_t      startUnix = 0;
        bool         dirty = false, doClear = false;
        {
            std::lock_guard<std::mutex> lock(m_activityMutex);
            dirty   = m_activityDirty;
            doClear = m_clearRequested;
            if (dirty) {
                details   = m_pendingDetails;
                state     = m_pendingState;
                startUnix = m_pendingStartTimeUnix;
                largeKey  = m_pendingLargeImageKey;
                largeText = m_pendingLargeImageText;
                m_activityDirty = false;
            }
            m_clearRequested = false;
        }

        if (!dirty && !doClear) continue;

        DWORD pid = GetCurrentProcessId();
        std::stringstream cmd;
        cmd << "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"" << (nonce++) << "\","
            << "\"args\":{\"pid\":" << pid;
        if (doClear) {
            cmd << ",\"activity\":null}}";
        } else {
            cmd << ",\"activity\":{\"timestamps\":{\"start\":" << startUnix << "}";
            // Match Discord's optional-string serialization: omit unavailable
            // source/signal fields rather than sending empty text or filler.
            if (!details.empty())
                cmd << ",\"details\":\"" << EscapeJson(WideToUtf8(details)) << "\"";
            if (!state.empty())
                cmd << ",\"state\":\"" << EscapeJson(WideToUtf8(state)) << "\"";
            if (!largeKey.empty()) {
                cmd << ",\"assets\":{"
                    << "\"large_image\":\"" << EscapeJson(largeKey) << "\","
                    << "\"large_text\":\""  << EscapeJson(WideToUtf8(largeText)) << "\""
                    << "}";
            }
            cmd << "}}}";
        }

        sentActivity = true;
        lastWasClear = doClear;
        if (!SendFrame(Opcode::Frame, cmd.str())) disconnected(L"Discord activity write failed");
        lastSend = std::chrono::steady_clock::now();
    }
    m_connected = false;
    m_running = false;
}

} // namespace NitLink
