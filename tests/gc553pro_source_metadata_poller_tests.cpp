#include "capture/hdr_source_poller.h"
#include "app/hdmi_source_display.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace NitLink {
struct HDRSourcePollerTestAccess {
    static void SetFactory(HDRSourcePoller& poller,
        std::function<std::unique_ptr<Gc553ProSourceReader>(const std::wstring&)> factory) {
        poller.m_gcReaderFactory = std::move(factory);
    }
    static bool VrrPending(HDRSourcePoller& poller) {
        std::lock_guard<std::mutex> lock(poller.m_metadataMutex);
        return poller.m_hasVrrUpdate;
    }
};
}
using namespace NitLink;
using namespace std::chrono_literals;
namespace {
size_t checks = 0;
void Check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
template<class Predicate> void Wait(Predicate predicate, const char* message) {
    const auto deadline = std::chrono::steady_clock::now() + 4500ms;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error(message);
        std::this_thread::sleep_for(10ms);
    }
    ++checks;
}
struct Event {
    char operation;
    unsigned reader;
    std::thread::id thread;
    std::chrono::steady_clock::time_point at;
};
struct Fixture {
    std::atomic<Gc553ProSourceHdrState> hdr{Gc553ProSourceHdrState::Hdr10Pq};
    std::atomic<HdmiSourceVrrState> vrr{HdmiSourceVrrState::Enabled};
    std::atomic<bool> timingAvailable{true};
    std::atomic<unsigned> created{0}, destroyed{0}, vrrReads{0};
    std::mutex mutex;
    std::vector<Event> events;
    void Record(char operation, unsigned reader) {
        std::lock_guard<std::mutex> lock(mutex);
        events.push_back({operation, reader, std::this_thread::get_id(), std::chrono::steady_clock::now()});
    }
};
class Reader final : public Gc553ProSourceReader {
public:
    explicit Reader(Fixture& fixture) : f(fixture), id(++fixture.created) { f.Record('C', id); }
    ~Reader() override { f.Record('D', id); ++f.destroyed; }
    Gc553ProHdrProbe Read() override {
        f.Record('H', id);
        const auto state = f.hdr.load();
        return {state, static_cast<uint8_t>(state == Gc553ProSourceHdrState::Hdr10Pq ? 2 : 0)};
    }
    HdmiSourceTiming ReadTiming() override {
        f.Record('T', id);
        return f.timingAvailable.load() ? HdmiSourceTiming{1920,1080,6000,true} : HdmiSourceTiming{};
    }
    HdmiSourceVrrState ReadVrr() override {
        f.Record('V', id); ++f.vrrReads;
        return f.vrr.load();
    }
private:
    Fixture& f;
    unsigned id;
};
}
int main() {
    try {
        Fixture fixture;
        // Destroy the poller before the fixture even if an assertion throws.
        HDRSourcePoller poller;
        HDRSourcePollerTestAccess::SetFactory(poller, [&](const std::wstring& name) {
            if (name != L"test GC553Pro") throw std::runtime_error("wrong worker device");
            return std::make_unique<Reader>(fixture);
        });
        poller.StartGc553Pro(L"test GC553Pro", Gc553ProSourceHdrState::Hdr10Pq, HdmiSourceVrrState::Disabled);
        HdmiSourceTiming timing;
        Wait([&] { return poller.AcceptGc553ProTimingUpdate(&timing); }, "initial real timing not published");
        Check(timing == HdmiSourceTiming{1920,1080,6000,true}, "worker publishes source timing");
        HdmiSourceVrrState update{};
        Wait([&] { return poller.AcceptGc553ProVrrUpdate(&update); }, "VRR transition not published");
        Check(update == HdmiSourceVrrState::Enabled && !poller.HasUpdate(),
              "VRR update never sets HDR/capture reconciliation flag");
        auto current = HdmiSourceVrrState::Disabled;
        int refreshes = 0;
        std::wstring title, presence;
        const auto refresh = [&] {
            ++refreshes;
            title = ComposeHdmiWindowTitle(L"Switch 2", FormatHdmiSourceTiming(timing), L"HDR", current);
            presence = ComposeHdmiPresenceSignal(FormatHdmiSourceTiming(timing), L"HDR", current);
        };
        ApplyHdmiSourceVrrMetadata(current, update, refresh);
        Check(refreshes == 1 && title.ends_with(L"[VRR]") && presence.ends_with(L" · VRR"),
              "accepted worker transition refreshes only metadata labels");
        Check(!poller.AcceptGc553ProVrrUpdate(&update) && !poller.AcceptGc553ProTimingUpdate(&timing),
              "notifications consumed once");

        fixture.vrr = HdmiSourceVrrState::Unknown;
        fixture.timingAvailable = false;
        const auto reads = fixture.vrrReads.load();
        Wait([&] { return fixture.vrrReads.load() > reads; }, "invalid observation not sampled");
        Wait([&] { return poller.AcceptGc553ProTimingUpdate(&timing); }, "unavailable timing not published");
        Check(!timing.available && !poller.HasUpdate(), "unavailable timing clears display without HDR notification");
        Check(!poller.AcceptGc553ProVrrUpdate(&update) && current == HdmiSourceVrrState::Enabled && refreshes == 1,
              "invalid VRR preserves last valid and causes no label flicker");

        fixture.hdr = Gc553ProSourceHdrState::Sdr;
        Gc553ProSourceHdrState hdr{};
        Wait([&] { return poller.AcceptGc553ProUpdate(&hdr); }, "existing HDR transition not published");
        Check(hdr == Gc553ProSourceHdrState::Sdr && !poller.AcceptGc553ProVrrUpdate(&update),
              "existing HDR monitor operates independently");
        fixture.hdr = Gc553ProSourceHdrState::Unknown;
        Wait([&] { return fixture.created.load() >= 2; }, "existing five-failure rebind not retained");
        Check(!poller.AcceptGc553ProVrrUpdate(&update) && !poller.HasUpdate(),
              "HDR mailbox rebind does not clear VRR or publish unknown HDR");
        fixture.hdr = Gc553ProSourceHdrState::Sdr;
        fixture.vrr = HdmiSourceVrrState::Disabled;
        Wait([&] { return HDRSourcePollerTestAccess::VrrPending(poller); }, "disabled VRR did not settle");
        Check(!poller.HasUpdate(), "disabled VRR notification cannot request capture reopen");
        const auto stopAt = std::chrono::steady_clock::now();
        poller.Stop();
        Check(std::chrono::steady_clock::now() - stopAt < 500ms && fixture.destroyed == fixture.created,
              "reader destroyed on worker and shutdown remains prompt");

        // Reuse the production poller for a new session with an old unread
        // Disabled notification. No metadata may leak across Start calls.
        fixture.vrr = HdmiSourceVrrState::Unknown;
        const auto newReads = fixture.vrrReads.load();
        poller.StartGc553Pro(L"test GC553Pro", Gc553ProSourceHdrState::Sdr);
        Wait([&] { return fixture.vrrReads.load() > newReads; }, "new session did not start");
        Check(!poller.AcceptGc553ProVrrUpdate(&update) && !poller.HasUpdate(),
              "new unknown session clears old VRR notification and seeds HDR");
        if (poller.AcceptGc553ProTimingUpdate(&timing))
            Check(!timing.available, "new session never exposes previous source timing");
        poller.Stop();

        std::lock_guard<std::mutex> lock(fixture.mutex);
        std::thread::id owner;
        std::chrono::steady_clock::time_point previousVrr{};
        unsigned previousReader = 0;
        for (size_t i = 0; i < fixture.events.size(); ++i) {
            const auto& event = fixture.events[i];
            if (event.operation == 'C') owner = event.thread;
            Check(event.thread == owner && owner != std::this_thread::get_id(),
                  "construct/read/rebind/destruct share worker COM owner");
            if (event.operation == 'T')
                Check(i + 1 < fixture.events.size() && fixture.events[i + 1].operation == 'V' &&
                      fixture.events[i + 1].reader == event.reader, "timing then VTEM serialized on shared mailbox");
            if (event.operation == 'V') {
                if (previousReader == event.reader)
                    Check(event.at - previousVrr >= 990ms, "production metadata cadence is at most 1 Hz");
                previousVrr = event.at; previousReader = event.reader;
            }
        }
        Check(fixture.created == fixture.destroyed, "all worker readers released");
        std::cout << checks << " source metadata worker checks passed (hardware-free, production cadence)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL after " << checks << " checks: " << e.what() << '\n'; return 1;
    }
}
