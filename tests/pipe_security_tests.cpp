#include "discord/pipe_transport.h"
#include "discord/discord_rpc.h"
#include "app/hdmi_source.h"
#include "app/hdmi_source_display.h"
#include <utility>
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace NitLink {
struct DiscordRPCTestAccess {
    static void SetOpener(DiscordRPC& rpc, std::function<HANDLE()> opener) { rpc.m_pipeOpener = std::move(opener); }
};
}
using namespace NitLink;
static void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
struct Pipe {
    HANDLE server = INVALID_HANDLE_VALUE, client = INVALID_HANDLE_VALUE, stop = nullptr;
    Pipe() {
        static unsigned sequence = 0;
        const auto name = L"\\\\.\\pipe\\NitLink-security-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(++sequence);
        server = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 4096, 4096, 0, nullptr);
        Check(server != INVALID_HANDLE_VALUE, "create test pipe");
        client = OpenRpcPipe(name.c_str());
        Check(client != INVALID_HANDLE_VALUE, "open test pipe");
        Check(ConnectNamedPipe(server, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED, "connect test pipe");
        stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        Check(stop != nullptr, "create stop event");
    }
    ~Pipe() { if (client != INVALID_HANDLE_VALUE) CloseHandle(client); if (server != INVALID_HANDLE_VALUE) CloseHandle(server); if (stop) CloseHandle(stop); }
    void Send(const void* data, DWORD size) {
        DWORD written = 0; Check(WriteFile(server, data, size, &written, nullptr) && written == size, "fixture write");
    }
};
static std::string ReceiveServerFrame(Pipe& pipe, uint32_t expectedOp) {
    const auto deadline = GetTickCount64() + 8000;
    DWORD available = 0;
    while (GetTickCount64() < deadline) {
        if (PeekNamedPipe(pipe.server, nullptr, 0, nullptr, &available, nullptr) && available >= 8) break;
        Sleep(5);
    }
    Check(available >= 8, "bounded server wait for header");
    uint32_t header[2]{};
    DWORD got = 0;
    Check(ReadFile(pipe.server, header, sizeof(header), &got, nullptr) && got == sizeof(header) &&
          header[0] == expectedOp && header[1] <= 65536, "server frame header");
    available = 0;
    while (GetTickCount64() < deadline) {
        if (PeekNamedPipe(pipe.server, nullptr, 0, nullptr, &available, nullptr) && available >= header[1]) break;
        Sleep(5);
    }
    Check(available >= header[1], "bounded server wait for body");
    std::string payload(header[1], '\0');
    Check(ReadFile(pipe.server, payload.data(), header[1], &got, nullptr) && got == header[1], "server frame body");
    return payload;
}
static void SourcePresenceTest() {
    Pipe pipe;
    DiscordRPC rpc;
    DiscordRPCTestAccess::SetOpener(rpc, [&] { return std::exchange(pipe.client, INVALID_HANDLE_VALUE); });
    const uint8_t reply[] = {1,0,0,0,2,0,0,0,'{','}'};
    pipe.Send(reply, sizeof(reply));
    Check(rpc.Connect("12345"), "source presence handshake");
    ReceiveServerFrame(pipe, 0);
    const auto started = std::chrono::system_clock::time_point{std::chrono::seconds(1700000000)};
    const auto source = GetEffectiveHdmiSourceLabel("switch2", L"detected PS5", L"SPD");
    const auto signal = FormatHdmiSourceTiming({1920,1080,6000,true});
    auto vrr = HdmiSourceVrrState::Unknown;
    auto publish = [&] {
        rpc.SetActivity(std::wstring(source), ComposeHdmiPresenceSignal(signal, L"HDR", vrr),
                        started, "nitlink-logo", L"NitLink");
    };
    publish();
    auto payload = ReceiveServerFrame(pipe, 1);
    Check(payload.find("\"details\":\"Switch 2\"") != std::string::npos &&
          payload.find("\"state\":\"1920x1080 @ 60Hz · HDR\"") != std::string::npos &&
          payload.find("In NitLink") == std::string::npos && payload.find("Capture Viewer") == std::string::npos,
          "viewer RPC contains separate source and real signal, without repeated application name");
    Check(payload.find("nitlink-logo") != std::string::npos && payload.find("1700000000") != std::string::npos,
          "viewer logo and activity clock retained");
    pipe.Send(reply, sizeof(reply));
    ApplyHdmiSourceVrrMetadata(vrr, HdmiSourceVrrState::Enabled, publish);
    payload = ReceiveServerFrame(pipe, 1);
    Check(payload.find("60Hz · HDR · VRR") != std::string::npos &&
          payload.find("1700000000") != std::string::npos && payload.find("nitlink-logo") != std::string::npos,
          "VRR transition refreshes serialized presence and retains art/timer");
    Check(!ApplyHdmiSourceVrrMetadata(vrr, HdmiSourceVrrState::Unknown, publish) &&
          vrr == HdmiSourceVrrState::Enabled, "invalid VRR read does not clear RPC metadata");
    pipe.Send(reply, sizeof(reply));
    ApplyHdmiSourceVrrMetadata(vrr, HdmiSourceVrrState::Disabled, publish);
    payload = ReceiveServerFrame(pipe, 1);
    Check(payload.find("· VRR") == std::string::npos && payload.find("1700000000") != std::string::npos,
          "valid disabled removes only VRR suffix");
    pipe.Send(reply, sizeof(reply));
    rpc.SetActivity(L"Spider-Man 2", ComposeHdmiPresenceState(source, signal, L"HDR", HdmiSourceVrrState::Enabled),
                    started, "spider-man-2", L"Spider-Man 2");
    payload = ReceiveServerFrame(pipe, 1);
    Check(payload.find("\"details\":\"Spider-Man 2\"") != std::string::npos &&
          payload.find("Switch 2 · 1920x1080 @ 60Hz · HDR · VRR") != std::string::npos &&
          payload.find("\"large_image\":\"spider-man-2\"") != std::string::npos,
          "selected game keeps game title/art and shares source mapping");
    pipe.Send(reply, sizeof(reply));
    rpc.SetActivity(L"", L"", started, "nitlink-logo", L"NitLink");
    payload = ReceiveServerFrame(pipe, 1);
    Check(payload.find("\"details\"") == std::string::npos && payload.find("\"state\"") == std::string::npos &&
          payload.find("In NitLink") == std::string::npos, "unknown source/signal omit empty activity fields");
    rpc.Disconnect();
}
static void ReconnectTest(bool updateWhileDisconnected) {
    Pipe first, second;
    DiscordRPC rpc;
    unsigned opened = 0;
    DiscordRPCTestAccess::SetOpener(rpc, [&] {
        if (opened++ == 0) return std::exchange(first.client, INVALID_HANDLE_VALUE);
        if (opened == 2) return std::exchange(second.client, INVALID_HANDLE_VALUE);
        return INVALID_HANDLE_VALUE;
    });
    const uint8_t reply[] = {1,0,0,0,2,0,0,0,'{','}'};
    first.Send(reply, sizeof(reply));
    second.Send(reply, sizeof(reply));
    Check(rpc.Connect("12345"), "test RPC initial handshake");
    ReceiveServerFrame(first, 0);
    rpc.SetActivity(L"Before disconnection", L"test", std::chrono::system_clock::now());
    ReceiveServerFrame(first, 1);
    CloseHandle(first.server);
    first.server = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + 4000;
    while (rpc.IsConnected() && GetTickCount64() < deadline) Sleep(5);
    Check(!rpc.IsConnected(), "broken pipe closes old stream");
    if (updateWhileDisconnected) rpc.SetActivity(L"Latest while reconnecting", L"test", std::chrono::system_clock::now());
    ReceiveServerFrame(second, 0);
    const auto activity = ReceiveServerFrame(second, 1);
    second.Send(reply, sizeof(reply));
    Check(activity.find(updateWhileDisconnected ? "Latest while reconnecting" : "Before disconnection") != std::string::npos,
          "reconnect replays activity with and without a new update");
    const auto beforeStop = GetTickCount64();
    rpc.Disconnect();
    Check(GetTickCount64() - beforeStop < 1000, "reconnected worker stops promptly");
}

static void SlowReplyTest() {
    Pipe pipe;
    DiscordRPC rpc;
    std::atomic<unsigned> opened = 0;
    DiscordRPCTestAccess::SetOpener(rpc, [&] {
        ++opened;
        return std::exchange(pipe.client, INVALID_HANDLE_VALUE);
    });
    const uint8_t reply[] = {1,0,0,0,2,0,0,0,'{','}'};
    pipe.Send(reply, sizeof(reply));
    Check(rpc.Connect("12345"), "slow-reply handshake");
    ReceiveServerFrame(pipe, 0);
    rpc.SetActivity(L"First", L"test", std::chrono::system_clock::now());
    ReceiveServerFrame(pipe, 1);
    pipe.Send(reply, 9);
    Sleep(1800);
    Check(rpc.IsConnected() && opened == 1, "partial reply beyond old timeout retains connection");
    pipe.Send(reply + 9, 1);
    rpc.SetActivity(L"Second", L"test", std::chrono::system_clock::now());
    Check(ReceiveServerFrame(pipe, 1).find("Second") != std::string::npos, "delayed reply completes on original stream");
    Sleep(1800);
    rpc.SetActivity(L"Third", L"test", std::chrono::system_clock::now());
    Check(ReceiveServerFrame(pipe, 1).find("Third") != std::string::npos &&
          rpc.IsConnected() && opened == 1, "missing reply does not clear presence");
    const uint8_t ping[] = {3,0,0,0,2,0,0,0,'o','k'};
    pipe.Send(ping, sizeof(ping));
    Check(ReceiveServerFrame(pipe, 4) == "ok", "asynchronous ping gets bounded pong");
    const auto stop = GetTickCount64();
    rpc.Disconnect();
    Check(GetTickCount64() - stop < 1000, "missing-reply worker cancels promptly");
}
static void IncrementalReaderTest() {
    Pipe pipe; RpcFrameReader reader;
    uint32_t opcode = 99; std::string payload;
    Check(reader.Poll(pipe.client, pipe.stop, opcode, payload) == RpcReadResult::Pending,
          "no reply is pending rather than failed");
    const uint8_t bytes[] = {1,0,0,0,3,0,0,0,'a','b','c'};
    for (size_t i = 0; i < sizeof(bytes); ++i) {
        pipe.Send(bytes + i, 1);
        const auto result = reader.Poll(pipe.client, pipe.stop, opcode, payload);
        Check(result == (i + 1 == sizeof(bytes) ? RpcReadResult::Frame : RpcReadResult::Pending),
              "incremental header and body retain framing");
    }
    Check(opcode == 1 && payload == "abc", "incremental reader delivers exact frame");
    const uint32_t oversized[] = {1, 65537};
    pipe.Send(oversized, sizeof(oversized));
    Check(reader.Poll(pipe.client, pipe.stop, opcode, payload) == RpcReadResult::Failed &&
          payload.empty(), "incremental reader rejects oversized header");
    pipe.Send(bytes, sizeof(bytes));
    Check(reader.Poll(pipe.client, pipe.stop, opcode, payload) == RpcReadResult::Frame &&
          payload == "abc", "failed reader resets before a subsequent poll");
    const uint32_t badOpcode[] = {999, 3};
    pipe.Send(badOpcode, sizeof(badOpcode));
    Check(reader.Poll(pipe.client, pipe.stop, opcode, payload) == RpcReadResult::Failed,
          "incremental reader rejects opcode");
    pipe.Send(bytes, sizeof(bytes));
    Check(reader.Poll(pipe.client, pipe.stop, opcode, payload) == RpcReadResult::Frame && payload == "abc",
          "invalid opcode also resets reader state");
}

static void BackoffTest() {
    Pipe pipe; DiscordRPC rpc;
    std::mutex mutex;
    std::vector<ULONGLONG> attempts;
    DiscordRPCTestAccess::SetOpener(rpc, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        attempts.push_back(GetTickCount64());
        return std::exchange(pipe.client, INVALID_HANDLE_VALUE);
    });
    const uint8_t reply[] = {1,0,0,0,2,0,0,0,'{','}'};
    pipe.Send(reply, sizeof(reply));
    Check(rpc.Connect("12345"), "backoff fixture handshake");
    ReceiveServerFrame(pipe, 0);
    CloseHandle(pipe.server); pipe.server = INVALID_HANDLE_VALUE;
    const auto deadline = GetTickCount64() + 10000;
    while (GetTickCount64() < deadline) {
        { std::lock_guard<std::mutex> lock(mutex); if (attempts.size() >= 3) break; }
        Sleep(10);
    }
    rpc.Disconnect();
    Check(attempts.size() == 3 && attempts[1] - attempts[0] >= 2000 &&
          attempts[2] - attempts[1] >= 4000, "broken-pipe retries use increasing backoff");
}
int main() {
    try {
        { Pipe p; uint32_t opcode = 0; std::string payload;
          const uint8_t bytes[] = {1,0,0,0,3,0,0,0,'a','b','c'};
          std::thread writer([&] { for (auto byte : bytes) { p.Send(&byte, 1); Sleep(3); } });
          const bool read = ReadRpcFrame(p.client, p.stop, opcode, payload, 1000);
          writer.join(); Check(read && opcode == 1 && payload == "abc", "fragmented header and payload"); }
        for (uint32_t op : {0u, 5u, UINT32_MAX}) {
            Pipe p; uint32_t header[] = {op, 0}, opcode; std::string payload;
            p.Send(header, sizeof(header)); Check(!ReadRpcFrame(p.client, p.stop, opcode, payload, 100), "invalid opcode");
        }
        { Pipe p; uint32_t header[] = {1, 65537}, opcode; std::string payload;
          p.Send(header, sizeof(header)); Check(!ReadRpcFrame(p.client, p.stop, opcode, payload, 100) && payload.empty(), "oversized payload"); }
        { Pipe p; uint32_t header[] = {1, 100}, opcode; std::string payload;
          p.Send(header, sizeof(header));
          const auto start = GetTickCount64();
          Check(!ReadRpcFrame(p.client, p.stop, opcode, payload, 100) && GetTickCount64() - start < 1000 && payload.empty(), "partial payload timeout"); }
        { Pipe p; uint32_t opcode; std::string payload;
          std::thread cancel([&] { Sleep(50); SetEvent(p.stop); });
          const auto start = GetTickCount64();
          const bool read = ReadRpcFrame(p.client, p.stop, opcode, payload, 5000);
          cancel.join(); Check(!read && GetTickCount64() - start < 1000, "pending read cancellation"); }
        { Pipe p; const auto start = GetTickCount64();
          Check(!WriteRpcFrame(p.client, p.stop, 1, std::string(65536, 'x'), 100) && GetTickCount64() - start < 1000, "stalled write timeout"); }
        { Pipe p;
          std::thread cancel([&] { Sleep(50); SetEvent(p.stop); });
          const auto start = GetTickCount64();
          const bool wrote = WriteRpcFrame(p.client, p.stop, 1, std::string(65536, 'x'), 5000);
          cancel.join(); Check(!wrote && GetTickCount64() - start < 1000, "pending write cancellation"); }
        { Pipe p;
          Check(!WriteRpcFrame(p.client, p.stop, 1, std::string(65537, 'x'), 100), "outbound payload cap");
          Check(WriteRpcFrame(p.client, p.stop, 1, "{}", 100), "normal outbound frame");
          char buffer[10]; DWORD got = 0;
          Check(ReadFile(p.server, buffer, sizeof(buffer), &got, nullptr) && got == sizeof(buffer), "server read");
          Check(ImpersonateNamedPipeClient(p.server), "inspect pipe security level");
          HANDLE token = nullptr; SECURITY_IMPERSONATION_LEVEL level = SecurityAnonymous; DWORD bytes = 0;
          const bool inspected = OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token) &&
              GetTokenInformation(token, TokenImpersonationLevel, &level, sizeof(level), &bytes);
          if (token) CloseHandle(token);
          const bool reverted = RevertToSelf() != FALSE;
          Check(reverted && inspected && level == SecurityIdentification, "server cannot use client impersonation token"); }
        IncrementalReaderTest();
        SourcePresenceTest();
        SlowReplyTest();
        ReconnectTest(false);
        ReconnectTest(true);
        BackoffTest();
        std::cout << "IPC fragmentation, deadlines, cancellation, limits, and SQOS tests passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
