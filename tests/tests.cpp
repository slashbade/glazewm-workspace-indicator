#include "../glazewm-workspace-indicator.wh.cpp"
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv) {
    try {
        winrt::init_apartment();
        if (argc == 3 && std::string(argv[1]) == "--stall-port") {
            g_settings.port = std::stoi(argv[2]);
            Monitor stale; stale.device = L"stale";
            g_monitors.push_back(stale);
            g_stop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
            g_network = std::thread(NetworkLoop);
            // A mock server accepts the upgrade but never answers the query.
            for (int i = 0; i < 35; ++i) { Sleep(100); CancelSocket(false); }
            bool cleared;
            { std::lock_guard lock(g_stateMutex); cleared = g_monitors.empty(); }
            auto before = GetTickCount64();
            g_unloading = true; SetEvent(g_stop); CancelSocket(true); g_network.join();
            CloseHandle(g_stop); g_stop = nullptr;
            Check(cleared, "stalled IPC must clear stale workspaces");
            Check(GetTickCount64() - before < 1500, "shutdown must cancel pending receive");
            std::cout << "PASS: stalled receive deadline, stale-state cleanup, pending receive cancellation\n";
            return 0;
        }
        const auto monitors = ParseMonitors(R"({"success":true,"data":{"monitors":[
          {"deviceName":"display1","x":-1920,"y":0,"children":[
            {"type":"workspace","id":"aa6e95c5-ac46-48d7-aeec-e9f7947902e8","name":"1","displayName":"聊天 <&>","hasFocus":true,"isDisplayed":true},
            {"type":"workspace","id":"b977a2b6-9972-444c-8997-6c5438907910","name":"2","displayName":null,"hasFocus":false,"isDisplayed":false},
            {"type":"workspace","id":"b977a2b6-9972-444c-8997-6c5438907910","name":"3","displayName":"","hasFocus":false,"isDisplayed":true}]},
          {"deviceName":"display2","x":0,"y":0,"children":[]}]}})");
        Check(monitors.size() == 2 && monitors[0].x == -1920, "monitor mapping / negative coordinates");
        auto& w = monitors[0].workspaces;
        Check(w.size() == 3 && w[0].label == L"聊天 <&>", "Unicode displayName");
        Check(w[1].label == L"2" && w[2].label.empty(), "Zebar nullish fallback");
        Check(w[0].focused && w[0].displayed && !w[2].focused && w[2].displayed, "focused vs displayed");
        Check(IsUuid(w[0].id) && !IsUuid(L"foo; exit") && !IsUuid(L"zz6e95c5-ac46-48d7-aeec-e9f7947902e8"), "command argument validation");
        bool rejected = false;
        try { ParseMonitors(R"({"success":false,"error":"failed"})"); } catch (...) { rejected = true; }
        Check(rejected, "IPC failure must not become an empty success");
        rejected = false;
        try { ParseMonitors("not json"); } catch (...) { rejected = true; }
        Check(rejected, "malformed JSON rejected");
        FocusWorkspace(w[0].id);
        Check(g_commands.front() == "command focus --container-id aa6e95c5-ac46-48d7-aeec-e9f7947902e8", "focus command");
        g_commands.clear(); // Tests never change the user's focused workspace.
        std::cout << "PASS: parsing, Unicode, state distinctions, command validation\n";

        // Exercise the actual WinHTTP client against the locally running WM.
        g_stop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
        g_network = std::thread(NetworkLoop);
        bool connected = false;
        for (int i = 0; i < 50; ++i) {
            Sleep(100); CancelSocket(false);
            std::lock_guard lock(g_stateMutex);
            if (!g_monitors.empty()) { connected = true; break; }
        }
        g_unloading = true; SetEvent(g_stop); CancelSocket(true); g_network.join(); CloseHandle(g_stop); g_stop = nullptr;
        Check(connected, "live GlazeWM IPC unavailable (start GlazeWM before tests)");
        std::cout << "PASS: live WebSocket query and shutdown\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
      catch (const winrt::hresult_error& error) { std::cerr << winrt::to_string(error.message()) << '\n'; return 1; }
}
