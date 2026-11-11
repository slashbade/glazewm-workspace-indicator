// ==WindhawkMod==
// @id              glazewm-workspace-indicator
// @name            GlazeWM Workspace Indicator
// @description     Native workspace buttons immediately to the right of Start on Windows 11
// @version         0.1.0
// @author          slashblade
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -loleaut32 -lruntimeobject -lwinhttp
// @license         GPL-3.0-or-later
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# GlazeWM Workspace Indicator
Windows 11 native XAML workspace buttons beside Start. Requires GlazeWM 3.x.
Each taskbar shows that monitor's active workspaces, using displayName or name,
like Zebar's starter widget. Click to focus a workspace.

Focused workspaces have an accent underline; displayed workspaces have a subtle
background. Native button hover, keyboard focus, tooltips and theme resources.
Connects directly to local GlazeWM IPC; no Zebar or helper process required.

Experimental: Windows taskbar internals vary by build. First enable may require
Microsoft symbol downloads. Horizontal Windows 11 taskbar only. Keep Start
visible. Mods which reposition Start or change its margins can conflict.
Settings changes reload the mod. Disconnected indicators hide automatically.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- port: 6123
  $name: GlazeWM IPC port
- refreshMs: 300
  $name: Refresh interval (ms)
  $description: Clamped to 100–5000 ms. Uses a persistent local WebSocket.
- labelMode: displayName
  $name: Workspace label
  $options:
    - displayName: Display name (Zebar default)
    - name: Workspace name / number
- maxWidth: 280
  $name: Maximum strip width (DIPs)
  $description: Horizontally scrolls when there are too many workspaces.
- buttonHeight: 32
  $name: Button height (DIPs)
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>
#include <winhttp.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#undef GetCurrentTime
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.UI.Xaml.Automation.h>
#include <winrt/Windows.UI.Xaml.Controls.h>
#include <winrt/Windows.UI.Xaml.Controls.Primitives.h>
#include <winrt/Windows.UI.Xaml.Markup.h>
#include <winrt/Windows.UI.Xaml.Media.h>

using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;

struct Workspace {
    std::wstring id, name, label;
    bool focused = false, displayed = false;
    bool operator==(const Workspace&) const = default;
};
struct Monitor {
    std::wstring device;
    int x = 0, y = 0;
    std::vector<Workspace> workspaces;
};
struct Settings { int port = 6123, refresh = 300, maxWidth = 280, height = 32; bool names = false; } g_settings;
std::mutex g_stateMutex;
std::vector<Monitor> g_monitors;
std::deque<std::string> g_commands;
std::atomic<bool> g_unloading = false;
HANDLE g_stop = nullptr;
std::thread g_network, g_refresh;

std::vector<Monitor> ParseMonitors(std::string_view json) {
    auto root = JsonObject::Parse(winrt::to_hstring(json));
    if (!root.GetNamedBoolean(L"success", false)) throw std::runtime_error("IPC request failed");
    std::vector<Monitor> monitors;
    for (auto value : root.GetNamedObject(L"data").GetNamedArray(L"monitors")) {
        auto obj = value.GetObject();
        Monitor monitor;
        monitor.device = obj.GetNamedString(L"deviceName", L"");
        monitor.x = static_cast<int>(obj.GetNamedNumber(L"x"));
        monitor.y = static_cast<int>(obj.GetNamedNumber(L"y"));
        for (auto child : obj.GetNamedArray(L"children")) {
            auto ws = child.GetObject();
            if (ws.GetNamedString(L"type", L"") != L"workspace") continue;
            Workspace w;
            w.id = ws.GetNamedString(L"id");
            w.name = ws.GetNamedString(L"name");
            auto display = ws.GetNamedValue(L"displayName", JsonValue::CreateNullValue());
            w.label = display.ValueType() == JsonValueType::String ? std::wstring(display.GetString()) : w.name;
            w.focused = ws.GetNamedBoolean(L"hasFocus", false);
            w.displayed = ws.GetNamedBoolean(L"isDisplayed", false);
            monitor.workspaces.push_back(std::move(w));
        }
        monitors.push_back(std::move(monitor));
    }
    return monitors;
}

// Only UUIDs from the server are accepted as command arguments. Labels never
// become shell commands, and no shell or CLI subprocess is started.
bool IsUuid(std::wstring_view id) {
    if (id.size() != 36) return false;
    for (size_t i = 0; i < id.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (id[i] != L'-') return false; }
        else if (!((id[i] >= L'0' && id[i] <= L'9') || (id[i] >= L'a' && id[i] <= L'f') || (id[i] >= L'A' && id[i] <= L'F'))) return false;
    }
    return true;
}
void FocusWorkspace(const std::wstring& id) {
    if (g_unloading || !IsUuid(id)) return;
    std::lock_guard lock(g_stateMutex);
    if (g_commands.size() < 16) g_commands.push_back("command focus --container-id " + winrt::to_string(id));
}

struct HttpHandle {
    HINTERNET value = nullptr;
    explicit HttpHandle(HINTERNET h = nullptr) : value(h) {}
    ~HttpHandle() { if (value) WinHttpCloseHandle(value); }
    HttpHandle(const HttpHandle&) = delete;
};
// The refresh thread enforces a deadline even if a WebSocket peer stalls.
// Closing the published handle cancels an outstanding synchronous receive.
std::mutex g_socketMutex;
HINTERNET g_socket = nullptr;
ULONGLONG g_deadline = 0;
void CancelSocket(bool force) {
    std::lock_guard lock(g_socketMutex);
    if (g_socket && (force || (g_deadline && GetTickCount64() >= g_deadline))) {
        WinHttpCloseHandle(g_socket);
        g_socket = nullptr;
        g_deadline = 0;
    }
}
bool Exchange(HINTERNET socket, const std::string& command, std::string& response) {
    {
        std::lock_guard lock(g_socketMutex);
        if (g_socket != socket || g_unloading) return false;
        g_deadline = GetTickCount64() + 2000;
    }
    if (WinHttpWebSocketSend(socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                            const_cast<char*>(command.data()), static_cast<DWORD>(command.size()))) return false;
    response.clear();
    for (;;) {
        char buffer[8192]; DWORD count = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type;
        if (WinHttpWebSocketReceive(socket, buffer, sizeof(buffer), &count, &type)) return false;
        if (type != WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE && type != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) return false;
        if (response.size() + count > 8 * 1024 * 1024) return false;
        response.append(buffer, count);
        if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) break;
    }
    std::lock_guard lock(g_socketMutex);
    g_deadline = 0;
    return g_socket == socket;
}
void NetworkLoop() {
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    while (!g_unloading) {
        try {
            HttpHandle session(WinHttpOpen(L"GlazeWM Windhawk/0.1", WINHTTP_ACCESS_TYPE_NO_PROXY, nullptr, nullptr, 0));
            if (!session.value) throw std::runtime_error("WinHttpOpen");
            WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1000);
            HttpHandle connection(WinHttpConnect(session.value, L"127.0.0.1", g_settings.port, 0));
            HttpHandle request(WinHttpOpenRequest(connection.value, L"GET", L"/", nullptr, nullptr, nullptr, 0));
            if (!request.value || !WinHttpSetOption(request.value, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
                !WinHttpSendRequest(request.value, nullptr, 0, nullptr, 0, 0, 0) || !WinHttpReceiveResponse(request.value, nullptr)) throw std::runtime_error("IPC connection");
            auto socket = WinHttpWebSocketCompleteUpgrade(request.value, 0);
            if (!socket) throw std::runtime_error("IPC upgrade");
            {
                std::lock_guard lock(g_socketMutex);
                if (g_unloading) { WinHttpCloseHandle(socket); break; }
                g_socket = socket;
            }
            while (!g_unloading) {
                std::deque<std::string> commands;
                { std::lock_guard lock(g_stateMutex); commands.swap(g_commands); }
                std::string response;
                for (auto& command : commands) {
                    if (!Exchange(socket, command, response)) throw std::runtime_error("IPC command");
                    if (!JsonObject::Parse(winrt::to_hstring(response)).GetNamedBoolean(L"success", false)) Wh_Log(L"GlazeWM rejected focus command");
                }
                if (!Exchange(socket, "query monitors", response)) throw std::runtime_error("IPC query");
                auto monitors = ParseMonitors(response);
                { std::lock_guard lock(g_stateMutex); g_monitors = std::move(monitors); }
                if (WaitForSingleObject(g_stop, g_settings.refresh) != WAIT_TIMEOUT) break;
            }
        } catch (...) { /* GlazeWM absent/restarting: hide stale data, then retry. */ }
        CancelSocket(true);
        { std::lock_guard lock(g_stateMutex); g_monitors.clear(); g_commands.clear(); }
        if (WaitForSingleObject(g_stop, 1000) != WAIT_TIMEOUT) break;
    }
    winrt::uninit_apartment();
}

template <typename F>
FrameworkElement EnumChildElements(FrameworkElement element, F callback) {
    for (int i = 0; i < Media::VisualTreeHelper::GetChildrenCount(element); ++i) {
        auto child = Media::VisualTreeHelper::GetChild(element, i).try_as<FrameworkElement>();
        if (child && callback(child)) return child;
    }
    return nullptr;
}
FrameworkElement FindChildByName(FrameworkElement parent, PCWSTR name) {
    return EnumChildElements(parent, [=](auto child) { return child.Name() == name; });
}

struct Strip {
    HWND hwnd;
    FrameworkElement start{nullptr};
    Controls::Grid root{nullptr};
    Controls::ScrollViewer view{nullptr};
    Controls::StackPanel panel{nullptr};
    Thickness originalMargin{};
    winrt::Windows::Foundation::IInspectable originalLocalMargin{nullptr};
    std::vector<std::pair<Controls::Button, winrt::event_token>> handlers;
    std::vector<Workspace> shown;
    double reserved = 0;
};
// Accessed only on the taskbar UI thread.
thread_local std::vector<Strip> g_strips;
void ClearButtons(Strip& strip) {
    for (auto& [button, token] : strip.handlers) button.Click(token);
    strip.handlers.clear();
    strip.panel.Children().Clear();
}
void RemoveStrip(Strip& strip) {
    ClearButtons(strip);
    uint32_t index;
    if (strip.root.Children().IndexOf(strip.view, index)) strip.root.Children().RemoveAt(index);
    if (strip.originalLocalMargin == DependencyProperty::UnsetValue()) strip.start.ClearValue(FrameworkElement::MarginProperty());
    else strip.start.SetValue(FrameworkElement::MarginProperty(), strip.originalLocalMargin);
}

Controls::Button MakeButton(const Workspace& workspace) {
    // ThemeResource bindings remain live when Windows changes theme or enters
    // high contrast. User labels are assigned as Text, never parsed as XAML.
    auto button = Markup::XamlReader::Load(LR"(<Button xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        Padding="10,0" MinWidth="32" Margin="2,0" CornerRadius="6" BorderThickness="0"
        Background="Transparent" FontFamily="Segoe UI Variable Text, Segoe UI" FontSize="12"
        Foreground="{ThemeResource SystemControlForegroundBaseHighBrush}" VerticalAlignment="Center" HorizontalContentAlignment="Stretch">
        <Grid MinWidth="16"><TextBlock x:Name="Label" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" VerticalAlignment="Center" TextAlignment="Center" TextTrimming="CharacterEllipsis" MaxWidth="120"/>
        <Border x:Name="ActiveLine" xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml" Height="3" Width="16" CornerRadius="1.5" HorizontalAlignment="Center" VerticalAlignment="Bottom" Margin="0,0,0,1" Background="{ThemeResource SystemControlHighlightAccentBrush}"/>
        </Grid></Button>)").as<Controls::Button>();
    button.Height(g_settings.height);
    auto grid = button.Content().as<Controls::Grid>();
    // Keep room for the 16-DIP underline even when collapsed, so short labels
    // stay centered and the button does not grow when it gains focus.
    grid.Height(g_settings.height);
    auto label = grid.FindName(L"Label").as<Controls::TextBlock>();
    label.Text(g_settings.names ? workspace.name : workspace.label);
    grid.FindName(L"ActiveLine").as<Controls::Border>().Visibility(workspace.focused ? Visibility::Visible : Visibility::Collapsed);
    if (workspace.displayed) {
        auto background = Markup::XamlReader::Load(LR"(<Border xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" CornerRadius="6" Background="{ThemeResource SystemControlBackgroundListLowBrush}"/>)").as<Controls::Border>();
        background.Margin({-10, 0, -10, 0});
        background.IsHitTestVisible(false);
        grid.Children().InsertAt(0, background);
    }
    std::wstring accessible = L"GlazeWM " + workspace.name + L": " + workspace.label;
    if (workspace.focused) accessible += L" (focused)";
    else if (workspace.displayed) accessible += L" (displayed)";
    Automation::AutomationProperties::SetName(button, accessible);
    Controls::ToolTipService::SetToolTip(button, winrt::box_value(accessible));
    return button;
}

bool ApplyStyle(XamlRoot xamlRoot, HWND hwnd) {
    auto content = xamlRoot.Content().try_as<FrameworkElement>();
    if (!content) return false;
    auto frame = EnumChildElements(content, [](auto child) { return winrt::get_class_name(child) == L"Taskbar.TaskbarFrame"; });
    if (!frame) return false;
    auto rootElement = FindChildByName(frame, L"RootGrid");
    auto root = rootElement ? rootElement.try_as<Controls::Grid>() : nullptr;
    if (!root) return false;
    auto repeater = FindChildByName(root, L"TaskbarFrameRepeater");
    if (!repeater) return false;
    auto start = EnumChildElements(repeater, [](auto child) { return Automation::AutomationProperties::GetAutomationId(child) == L"StartButton"; });
    if (!start) return false;
    auto it = std::find_if(g_strips.begin(), g_strips.end(), [&](auto& s) { return s.hwnd == hwnd; });
    if (it != g_strips.end() && (it->start != start || it->root != root)) {
        RemoveStrip(*it); g_strips.erase(it); it = g_strips.end();
    }
    if (it == g_strips.end()) {
        Strip s;
        s.hwnd = hwnd; s.start = start; s.root = root; s.originalMargin = start.Margin();
        s.originalLocalMargin = start.ReadLocalValue(FrameworkElement::MarginProperty());
        s.panel = Controls::StackPanel(); s.panel.Orientation(Controls::Orientation::Horizontal);
        s.view = Controls::ScrollViewer(); s.view.Content(s.panel);
        s.view.HorizontalScrollMode(Controls::ScrollMode::Enabled);
        s.view.HorizontalScrollBarVisibility(Controls::ScrollBarVisibility::Hidden);
        s.view.VerticalScrollMode(Controls::ScrollMode::Disabled);
        s.view.VerticalScrollBarVisibility(Controls::ScrollBarVisibility::Disabled);
        s.view.HorizontalAlignment(HorizontalAlignment::Left);
        s.view.VerticalAlignment(VerticalAlignment::Top);
        Controls::Grid::SetColumnSpan(s.view, std::max(1u, root.ColumnDefinitions().Size()));
        Controls::Grid::SetRowSpan(s.view, std::max(1u, root.RowDefinitions().Size()));
        Controls::Canvas::SetZIndex(s.view, 100);
        root.Children().Append(s.view);
        g_strips.push_back(std::move(s)); it = std::prev(g_strips.end());
    }
    auto& s = *it;
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info);
    std::vector<Workspace> workspaces;
    {
        std::lock_guard lock(g_stateMutex);
        auto monitor = std::find_if(g_monitors.begin(), g_monitors.end(), [&](auto& m) { return _wcsicmp(m.device.c_str(), info.szDevice) == 0; });
        if (monitor == g_monitors.end()) monitor = std::find_if(g_monitors.begin(), g_monitors.end(), [&](auto& m) { return m.x == info.rcMonitor.left && m.y == info.rcMonitor.top; });
        if (monitor != g_monitors.end()) workspaces = monitor->workspaces;
    }
    // Unsupported vertical/hidden Start layouts hide without reserving space.
    if (root.ActualHeight() > root.ActualWidth() || start.Visibility() != Visibility::Visible) workspaces.clear();
    if (s.shown != workspaces) {
        ClearButtons(s);
        for (auto& workspace : workspaces) {
            auto button = MakeButton(workspace);
            auto token = button.Click([id = workspace.id](auto&&, auto&&) { FocusWorkspace(id); });
            s.handlers.emplace_back(button, token);
            s.panel.Children().Append(button);
        }
        s.shown = workspaces;
    }
    s.panel.Measure({10000, static_cast<float>(g_settings.height)});
    double width = workspaces.empty() ? 0 : std::min({static_cast<double>(s.panel.DesiredSize().Width), static_cast<double>(g_settings.maxWidth), root.ActualWidth() * .35});
    double reserve = width ? width + 8 : 0;
    if (s.reserved != reserve) {
        auto margin = s.originalMargin; margin.Right += reserve;
        start.Margin(margin); s.reserved = reserve;
    }
    if (s.view.Width() != width) s.view.Width(width);
    if (s.view.Height() != g_settings.height) s.view.Height(g_settings.height);
    auto position = start.TransformToVisual(root).TransformPoint({0, 0});
    Thickness placement{position.X + start.ActualWidth() + 4, std::max(0.0, position.Y + (start.ActualHeight() - g_settings.height) / 2), 0, 0};
    if (s.view.Margin() != placement) s.view.Margin(placement);
    auto visibility = width > 0 ? Visibility::Visible : Visibility::Collapsed;
    if (s.view.Visibility() != visibility) s.view.Visibility(visibility);
    return true;
}

// Taskbar host discovery / UI-thread bridge adapted from m417z's
// taskbar-start-button-position and ptrkhh's hide-start-button (GPL-3.0).
void* CTaskBand_ITaskListWndSite_vftable;
void* CSecondaryTaskBand_ITaskListWndSite_vftable;

using CTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis, void** result);
CTaskBand_GetTaskbarHost_t CTaskBand_GetTaskbarHost_Original;

void* TaskbarHost_FrameHeight_Original;

using CSecondaryTaskBand_GetTaskbarHost_t = void*(WINAPI*)(void* pThis,
                                                           void** result);
CSecondaryTaskBand_GetTaskbarHost_t CSecondaryTaskBand_GetTaskbarHost_Original;

using std__Ref_count_base__Decref_t = void(WINAPI*)(void* pThis);
std__Ref_count_base__Decref_t std__Ref_count_base__Decref_Original;

XamlRoot XamlRootFromTaskbarHostSharedPtr(void* taskbarHostSharedPtr[2]) {
    if (!taskbarHostSharedPtr[0] && !taskbarHostSharedPtr[1])
        return nullptr;

    size_t offset;
    const BYTE* b = (const BYTE*)TaskbarHost_FrameHeight_Original;
    if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 &&
        b[5] == 0x83 && b[6] == 0xC1 && b[7] <= 0x7F)
        offset = b[7];
    else {
        Wh_Log(L"Unsupported TaskbarHost::FrameHeight; refusing to guess an offset");
        std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);
        return nullptr;
    }

    auto* unk = *(::IUnknown**)((BYTE*)taskbarHostSharedPtr[0] + offset);
    FrameworkElement fe = nullptr;
    if (unk) unk->QueryInterface(winrt::guid_of<FrameworkElement>(),
                        winrt::put_abi(fe));

    auto result = fe ? fe.XamlRoot() : nullptr;
    std__Ref_count_base__Decref_Original(taskbarHostSharedPtr[1]);
    return result;
}

XamlRoot GetTaskbarXamlRoot(HWND hWnd, bool isSecondary) {
    HWND hTaskSwWnd = isSecondary
        ? (HWND)FindWindowEx(hWnd, nullptr, L"WorkerW", nullptr)
        : (HWND)GetProp(hWnd, L"TaskbandHWND");
    if (!hTaskSwWnd)
        return nullptr;

    void* vftable = isSecondary ? CSecondaryTaskBand_ITaskListWndSite_vftable
                                : CTaskBand_ITaskListWndSite_vftable;
    void* p = (void*)GetWindowLongPtr(hTaskSwWnd, 0); if (!p) return nullptr;
    for (int i = 0; *(void**)p != vftable; i++) {
        if (i == 20)
            return nullptr;
        p = (void**)p + 1;
    }

    void* sharedPtr[2]{};
    if (isSecondary)
        CSecondaryTaskBand_GetTaskbarHost_Original(p, sharedPtr);
    else
        CTaskBand_GetTaskbarHost_Original(p, sharedPtr);
    return XamlRootFromTaskbarHostSharedPtr(sharedPtr);
}

HWND FindCurrentProcessTaskbarWnd() {
    HWND hWnd = nullptr;
    while ((hWnd = FindWindowEx(nullptr, hWnd, L"Shell_TrayWnd", nullptr))) {
        DWORD pid;
        GetWindowThreadProcessId(hWnd, &pid);
        if (pid == GetCurrentProcessId())
            return hWnd;
    }
    return nullptr;
}

void ApplySettingsFromTaskbarThread() {
    if (g_unloading) {
        for (auto& strip : g_strips) {
            try { RemoveStrip(strip); } catch (...) { Wh_Log(L"Strip cleanup failed"); }
        }
        g_strips.clear();
        return;
    }
    for (auto it = g_strips.begin(); it != g_strips.end();) {
        if (!IsWindow(it->hwnd)) {
            try { RemoveStrip(*it); } catch (...) {}
            it = g_strips.erase(it);
        } else ++it;
    }
    EnumThreadWindows(
        GetCurrentThreadId(),
        [](HWND hWnd, LPARAM) -> BOOL {
            WCHAR cls[32];
            if (GetClassName(hWnd, cls, ARRAYSIZE(cls)) == 0)
                return TRUE;

            try {
                XamlRoot xamlRoot = nullptr;
                if (_wcsicmp(cls, L"Shell_TrayWnd") == 0)
                    xamlRoot = GetTaskbarXamlRoot(hWnd, false);
                else if (_wcsicmp(cls, L"Shell_SecondaryTrayWnd") == 0)
                    xamlRoot = GetTaskbarXamlRoot(hWnd, true);

                if (xamlRoot) ApplyStyle(xamlRoot, hWnd);
            } catch (...) { Wh_Log(L"Taskbar layout update failed"); }
            return TRUE;
        },
        0);
}

void ApplySettings(HWND hTaskbarWnd) {
    static const UINT msg =
        RegisterWindowMessage(L"Windhawk_RunFromWindowThread_" WH_MOD_ID);

    DWORD threadId = GetWindowThreadProcessId(hTaskbarWnd, nullptr);
    if (!threadId)
        return;

    if (threadId == GetCurrentThreadId()) {
        ApplySettingsFromTaskbarThread();
        return;
    }

    HHOOK hook = SetWindowsHookEx(
        WH_CALLWNDPROC,
        [](int nCode, WPARAM wParam, LPARAM lParam) -> LRESULT {
            if (nCode == HC_ACTION &&
                ((const CWPSTRUCT*)lParam)->message == msg)
                ApplySettingsFromTaskbarThread();
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        },
        nullptr, threadId);
    if (!hook)
        return;

    SendMessage(hTaskbarWnd, msg, 0, 0);
    UnhookWindowsHookEx(hook);
}

bool HookTaskbarDllSymbols() {
    HMODULE module =
        LoadLibraryEx(L"taskbar.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module)
        return false;

    WindhawkUtils::SYMBOL_HOOK taskbarDllHooks[] = {
        {{LR"(const CTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &CTaskBand_ITaskListWndSite_vftable},
        {{LR"(const CSecondaryTaskBand::`vftable'{for `ITaskListWndSite'})"},
         &CSecondaryTaskBand_ITaskListWndSite_vftable},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CTaskBand::GetTaskbarHost(void)const )"},
         &CTaskBand_GetTaskbarHost_Original},
        {{LR"(public: int __cdecl TaskbarHost::FrameHeight(void)const )"},
         &TaskbarHost_FrameHeight_Original},
        {{LR"(public: virtual class std::shared_ptr<class TaskbarHost> __cdecl CSecondaryTaskBand::GetTaskbarHost(void)const )"},
         &CSecondaryTaskBand_GetTaskbarHost_Original},
        {{LR"(public: void __cdecl std::_Ref_count_base::_Decref(void))"},
         &std__Ref_count_base__Decref_Original},
    };
    return HookSymbols(module, taskbarDllHooks, ARRAYSIZE(taskbarDllHooks));
}

void RefreshLoop() {
    ULONGLONG lastRefresh = 0;
    while (!g_unloading) {
        CancelSocket(false);
        auto now = GetTickCount64();
        if (now - lastRefresh >= static_cast<ULONGLONG>(std::min(g_settings.refresh, 500))) {
            if (auto taskbar = FindCurrentProcessTaskbarWnd()) ApplySettings(taskbar);
            lastRefresh = now;
        }
        if (WaitForSingleObject(g_stop, 100) != WAIT_TIMEOUT) break;
    }
}
BOOL Wh_ModInit() {
    g_settings.port = std::clamp(Wh_GetIntSetting(L"port"), 1, 65535);
    g_settings.refresh = std::clamp(Wh_GetIntSetting(L"refreshMs"), 100, 5000);
    g_settings.maxWidth = std::clamp(Wh_GetIntSetting(L"maxWidth"), 48, 800);
    g_settings.height = std::clamp(Wh_GetIntSetting(L"buttonHeight"), 24, 48);
    auto mode = Wh_GetStringSetting(L"labelMode");
    g_settings.names = mode && std::wstring_view(mode) == L"name";
    Wh_FreeStringSetting(mode);
    if (!HookTaskbarDllSymbols()) return FALSE;
    g_stop = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    return g_stop != nullptr;
}
void Wh_ModAfterInit() {
    try { g_network = std::thread(NetworkLoop); g_refresh = std::thread(RefreshLoop); }
    catch (...) {
        g_unloading = true; SetEvent(g_stop); CancelSocket(true);
        if (g_network.joinable()) g_network.join();
        Wh_Log(L"Could not start background threads");
    }
}
void Wh_ModSettingsChanged(BOOL* reload) { *reload = TRUE; }
void Wh_ModBeforeUninit() {
    g_unloading = true;
    if (g_stop) SetEvent(g_stop);
    CancelSocket(true);
    if (g_network.joinable()) g_network.join();
    if (g_refresh.joinable()) g_refresh.join();
    if (auto taskbar = FindCurrentProcessTaskbarWnd()) ApplySettings(taskbar);
}
void Wh_ModUninit() {
    if (g_stop) { CloseHandle(g_stop); g_stop = nullptr; }
}
