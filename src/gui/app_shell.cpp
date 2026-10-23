#include "platform/clock.h"
#include "gui/app.h"
#include "gui/localization.h"
#include "gui/model_picker.h"
#include "gui/theme.h"
#include "gui/window_geometry_windows.h"

#include "core/context_compactor.h"
#include "core/external_agents.h"
#include "core/session_exchange.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include <commdlg.h>
#include <objidl.h>
#include <propidl.h>
#include "platform/gdiplus_headers.h"
#include <imm.h>
#include <shellapi.h>
#include <shlobj.h>
#include <windowsx.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include "platform/filesystem.h"
#include <fstream>
#include <map>
#include <limits>
#include <set>
#include <sstream>

namespace spark {
namespace {

ULONG_PTR g_gdiplus_token = 0;

constexpr int kSidebarTitle = 501;
constexpr int kNewConversation = 502;
constexpr int kProjectLabel = 503;
constexpr int kRecentLabel = 505;
constexpr int kHistoryList = 506;
constexpr int kSettingsButton = 507;
constexpr int kModelCombo = 508;
constexpr int kChat = 509;
constexpr int kComposer = 510;
constexpr int kSend = 511;
constexpr int kWorkspaceLabel = 512;
constexpr int kStatusLabel = 513;
constexpr int kEmptyMark = 514;
constexpr int kEmptyTitle = 515;
constexpr int kMainTitle = 516;
constexpr int kContextLabel = 517;
constexpr int kPermissionButton = 518;
constexpr int kPlusButton = 519;
constexpr int kModelLabel = 520;
constexpr int kProjectsList = 521;
constexpr int kModelButton = 522;
constexpr int kProjectMenuButton = 523;
constexpr int kProjectAddButton = 524;
constexpr int kRecentMenuButton = 525;
constexpr int kRecentNewButton = 526;
constexpr int kSidebarScrollbar = 527;
constexpr int kSummaryButton = 528;
constexpr int kSubAgentButton = 529;
constexpr int kSideChat = 530;
constexpr int kSideTitle = 531;
constexpr int kSideClose = 532;

constexpr int kPermissionAsk = 701;
constexpr int kPermissionApprove = 702;
constexpr int kPermissionFull = 703;

constexpr int kModelHeaderBase = 900;
constexpr int kModelItemBase = 1000;
constexpr int kReasoningItemBase = 1100;
constexpr int kModelReasoningEntry = 898;

constexpr int kHistoryDelete = 801;
constexpr int kHistoryPin = 802;
constexpr int kHistoryArchive = 803;

constexpr int kOrganizeSidebar = 1500;
constexpr int kChatSort = 1501;
constexpr int kOrganizeByProject = 1510;
constexpr int kOrganizeByList = 1511;
constexpr int kSortPriority = 1520;
constexpr int kSortRecent = 1521;
constexpr int kSortManual = 1522;
constexpr int kImportCodexRecent = 1530;
constexpr int kSubAgentMenuBase = 1800;
constexpr int kSourceMenuBase = 1900;

constexpr int kProjectPin = 1600;
constexpr int kProjectEdit = 1601;
constexpr int kProjectOpenFolder = 1602;
constexpr int kProjectArchive = 1603;
constexpr int kProjectRemove = 1604;
constexpr int kProjectImportCodex = 1605;

constexpr int kHistoryPinAction = 1700;
constexpr int kHistoryArchiveAction = 1701;
constexpr int kHistoryDeleteAction = 1702;
constexpr int kHistoryRenameAction = 1703;
constexpr int kHistoryExportCodexAction = 1704;

constexpr UINT kUiEventMessage = WM_APP + 1;
constexpr UINT kApprovalMessage = WM_APP + 2;
constexpr UINT kTrayMessage = WM_APP + 4;
constexpr UINT kSettingsClosedMessage = WM_APP + 5;
constexpr UINT kEmptyStateRefreshMessage = WM_APP + 7;
constexpr UINT kActivateSessionMessage = WM_APP + 8;
UINT TaskbarCreatedMessage() {
    static const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
    return message;
}
constexpr UINT_PTR kComposerTimer = 1;
constexpr UINT_PTR kActivityTimer = 3;
constexpr UINT_PTR kExternalPollTimer = 4;
// 100 ms keeps the status-line shimmer and the sidebar spinner smooth while limiting
// repaint work on older Win7 machines.
constexpr UINT kActivityIntervalMs = 100;
constexpr UINT kTraySettings = 1800;
constexpr UINT kTrayExit = 1801;
constexpr UINT kTrayIconId = 1;
constexpr int kAppIconResource = 101;

std::string StartupMarkerPath() {
    return JoinPath(JoinPath(GetExecutableDirectory(), "data"), ".running");
}
// Four simultaneous turns keeps Win7 memory and thread pressure bounded while
// still allowing normal multi-conversation work.
constexpr size_t kMaxConcurrentTurns = 4;

COLORREF kBackground = RGB(28, 29, 31);
COLORREF kSidebar = RGB(37, 38, 40);
COLORREF kPanel = RGB(43, 44, 47);
COLORREF kInput = RGB(48, 49, 52);
COLORREF kSelection = RGB(61, 62, 66);
COLORREF kBorder = RGB(67, 68, 72);
COLORREF kText = RGB(236, 237, 240);
COLORREF kMuted = RGB(150, 153, 160);
COLORREF kSubtle = RGB(112, 115, 122);
COLORREF kAccent = RGB(91, 99, 112);
COLORREF kAccentPressed = RGB(108, 117, 132);
COLORREF kError = RGB(242, 128, 128);
COLORREF kWarning = RGB(222, 146, 75);
COLORREF kSuccess = RGB(111, 190, 140);
COLORREF kRailIdle = RGB(74, 76, 82);
COLORREF kRailHover = RGB(244, 245, 248);
COLORREF kSidebarHover = RGB(44, 45, 48);
COLORREF kSidebarHoverText = RGB(250, 250, 250);

using DwmSetWindowAttributeFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);

struct SessionSummary {
    std::string path;
    std::string title;
    std::string workspace;
    std::string scope;
    uint64_t updated_at = 0;
    bool pinned = false;
    bool has_messages = false;
};

struct CachedSessionSummary {
    ULONGLONG size = 0;
    FILETIME write_time{};
    SessionSummary summary;
};

std::map<std::string, CachedSessionSummary>& SessionSummaryCache() {
    static std::map<std::string, CachedSessionSummary> cache;
    return cache;
}

bool SameFileStamp(const WIN32_FILE_ATTRIBUTE_DATA& data, const CachedSessionSummary& cached) {
    ULARGE_INTEGER size{};
    size.LowPart = data.nFileSizeLow;
    size.HighPart = data.nFileSizeHigh;
    return size.QuadPart == cached.size && CompareFileTime(&data.ftLastWriteTime, &cached.write_time) == 0;
}

bool IsPlaceholderSession(const SessionSummary& summary) {
    // Conversations are materialized on the first message, so a session without messages is
    // still a hidden draft and must never appear in the sidebar.
    return !summary.has_messages;
}

std::wstring TrimForDisplay(std::wstring value, size_t limit) {
    if (value.size() <= limit) return value;
    value.resize(limit);
    value += L"...";
    return value;
}

void ApplyDarkTitleBar(HWND window) {
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm == nullptr) return;
    const auto set_attribute = reinterpret_cast<DwmSetWindowAttributeFn>(GetProcAddress(dwm, "DwmSetWindowAttribute"));
    if (set_attribute != nullptr) {
        const BOOL enabled = TRUE;
        set_attribute(window, 20, &enabled, sizeof(enabled));
        set_attribute(window, 19, &enabled, sizeof(enabled));
    }
    FreeLibrary(dwm);
}

HFONT MakeFont(int points, const wchar_t* face, int weight = FW_NORMAL) {
    HDC screen = GetDC(nullptr);
    const int height = -MulDiv(points, GetDeviceCaps(screen, LOGPIXELSY), 72);
    ReleaseDC(nullptr, screen);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

HMENU ControlId(int id) { return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)); }

// Every layout constant is written in 96-DPI units and scaled through these helpers, so the
// window and the spacing follow the system resolution instead of hardcoding pixels.
int SystemDpi() {
    HDC screen = GetDC(nullptr);
    const int dpi = screen != nullptr ? GetDeviceCaps(screen, LOGPIXELSY) : 96;
    if (screen != nullptr) ReleaseDC(nullptr, screen);
    return dpi > 0 ? dpi : 96;
}

int ScaleForSystemDpi(int pixels) { return MulDiv(pixels, SystemDpi(), 96); }

HBRUSH CachedBrush(COLORREF color);

COLORREF BlendColor(COLORREF from, COLORREF to, int amount) {
    amount = std::max(0, std::min(255, amount));
    const int inverse = 255 - amount;
    return RGB((GetRValue(from) * inverse + GetRValue(to) * amount) / 255,
               (GetGValue(from) * inverse + GetGValue(to) * amount) / 255,
               (GetBValue(from) * inverse + GetBValue(to) * amount) / 255);
}

void DrawActivityGlyph(HDC device, int center_x, int center_y, int radius, int frame, COLORREF color,
                       COLORREF muted) {
    constexpr int kSegments = 8;
    const int dot = std::max(2, ScaleForSystemDpi(2));
    for (int index = 0; index < kSegments; ++index) {
        const int distance = (index - frame + kSegments * 4) % kSegments;
        const int strength = distance == 0 ? 255 : distance == 1 ? 170 : distance == 2 ? 95 : 42;
        const int angle = index * 45;
        const double radians = static_cast<double>(angle) * 3.14159265358979323846 / 180.0;
        const int x = center_x + static_cast<int>(radius * std::cos(radians) + 0.5);
        const int y = center_y + static_cast<int>(radius * std::sin(radians) + 0.5);
        const COLORREF dot_color = BlendColor(muted, color, strength);
        HBRUSH brush = CreateSolidBrush(dot_color);
        RECT rect{x - dot / 2, y - dot / 2, x - dot / 2 + dot, y - dot / 2 + dot};
        FillRect(device, &rect, brush);
        DeleteObject(brush);
    }
}

int ComboIndex(HWND combo) { return static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)); }

HBRUSH CachedBrush(COLORREF color) {
    static std::map<COLORREF, HBRUSH> brushes;
    const auto iterator = brushes.find(color);
    if (iterator != brushes.end()) return iterator->second;
    HBRUSH brush = CreateSolidBrush(color);
    brushes[color] = brush;
    return brush;
}

uint64_t ParseTokenCount(const std::string& text) {
    if (text.empty()) return 0;
    uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') return value;
        value = value * 10 + static_cast<uint64_t>(character - '0');
    }
    return value;
}

// Reads the context size recorded for the most recent turn of a session journal. Used when the
// conversation is opened again (for example after a restart) before any new provider call
// publishes a live number.
uint64_t LastRecordedContextTokens(const SessionState& state) {
    if (!state.metadata.is_object()) return 0;
    const auto usages = state.metadata.find("turn_usages");
    if (usages == state.metadata.end() || !usages->is_array()) return 0;
    for (auto iterator = usages->rbegin(); iterator != usages->rend(); ++iterator) {
        if (!iterator->is_object()) continue;
        const uint64_t tokens = iterator->value("context_tokens", uint64_t{0});
        if (tokens > 0) return tokens;
    }
    return 0;
}

void ApplyDarkMenuStyle(HMENU menu) {
    MENUINFO info{};
    info.cbSize = sizeof(info);
    info.fMask = MIM_BACKGROUND | MIM_APPLYTOSUBMENUS;
    info.hbrBack = CachedBrush(kPanel);
    SetMenuInfo(menu, &info);
}

std::vector<std::string> SplitLines(const std::string& input) {
    std::vector<std::string> lines;
    std::istringstream stream(input);
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(std::move(line));
    }
    return lines;
}

bool ProcessIsAlive(DWORD process_id) {
    if (process_id == 0) return false;
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, process_id);
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    const DWORD wait = WaitForSingleObject(process, 0);
    CloseHandle(process);
    return wait == WAIT_TIMEOUT;
}

bool ReadFileStamp(const std::string& path, ULONGLONG* size, FILETIME* write_time) {
    if (size == nullptr || write_time == nullptr) return false;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &attributes)) return false;
    ULARGE_INTEGER value{};
    value.LowPart = attributes.nFileSizeLow;
    value.HighPart = attributes.nFileSizeHigh;
    *size = value.QuadPart;
    *write_time = attributes.ftLastWriteTime;
    return true;
}

void CollectExternalRunningSessions(const std::string& data_directory, std::set<std::string>* sessions,
                                    std::map<std::string, nlohmann::json>* details) {
    if (sessions == nullptr || details == nullptr) return;
    sessions->clear();
    details->clear();
    const std::string directory = JoinPath(data_directory, "external_agents");
    WIN32_FIND_DATAW find_data{};
    const std::wstring pattern = Utf8ToWide(JoinPath(directory, "process-*.json"));
    HANDLE find = FindFirstFileW(pattern.c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        const std::string path = JoinPath(directory, WideToUtf8(find_data.cFileName));
        const auto contents = ReadFileUtf8(path);
        if (!contents.has_value()) continue;
        try {
            const nlohmann::json document = nlohmann::json::parse(*contents);
            if (!IsExternalAgentRegistryFresh(document, UnixTimeSeconds())) continue;
            const DWORD process_id = static_cast<DWORD>(document.value("process_id", uint64_t{0}));
            if (!ProcessIsAlive(process_id)) continue;
            const auto runs = document.find("runs");
            if (runs == document.end() || !runs->is_array()) continue;
            for (const auto& run : *runs) {
                if (!run.is_object()) continue;
                const std::string state = LowerAscii(Trim(run.value("state", std::string{})));
                if (state != "running" && state != "queued" && state != "awaiting_approval" &&
                    state != "cancelling") {
                    continue;
                }
                const std::string session_path = run.value("session_path", std::string{});
                if (session_path.empty()) continue;
                const std::string key = NormalizePathForComparison(session_path);
                sessions->insert(key);
                (*details)[key] = run;
            }
        } catch (const nlohmann::json::exception&) {
        }
    } while (FindNextFileW(find, &find_data) != FALSE);
    FindClose(find);
}

SessionSummary ReadSessionSummary(const std::string& path) {
    SessionSummary summary;
    summary.path = path;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if (!GetFileAttributesExW(Utf8ToWide(path).c_str(), GetFileExInfoStandard, &attributes)) return summary;
    auto& cache = SessionSummaryCache();
    const auto cached = cache.find(path);
    if (cached != cache.end() && SameFileStamp(attributes, cached->second)) return cached->second.summary;

    spark::fs_ifstream stream(spark::fs::path(Utf8ToWide(path)), std::ios::binary);
    if (!stream) return summary;
    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        try {
            const nlohmann::json event = nlohmann::json::parse(line);
            const std::string type = event.value("type", std::string{});
            const nlohmann::json payload = event.value("payload", nlohmann::json::object());
            summary.updated_at = event.value("timestamp", summary.updated_at);
            if (type == "session.created") {
                summary.title = payload.value("title", std::string("新对话"));
                summary.workspace = payload.value("workspace", std::string{});
            } else if (type == "session.title") {
                summary.title = payload.value("title", summary.title);
            } else if (type == "session.pinned") {
                summary.pinned = payload.value("pinned", summary.pinned);
            } else if (type == "session.scope") {
                summary.scope = payload.value("scope", summary.scope);
            } else if (type == "message") {
                summary.has_messages = true;
            } else if (type == "snapshot") {
                summary.title = payload.value("title", summary.title);
                summary.workspace = payload.value("workspace", summary.workspace);
                if (payload.contains("messages") && payload["messages"].is_array()) {
                    // A conversation that has ever produced messages must stay visible even if
                    // a later edit or maintenance snapshot temporarily reduces it to empty.
                    summary.has_messages = summary.has_messages || !payload["messages"].empty();
                }
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    if (summary.title.empty()) summary.title = "新对话";
    CachedSessionSummary entry;
    ULARGE_INTEGER size{};
    size.LowPart = attributes.nFileSizeLow;
    size.HighPart = attributes.nFileSizeHigh;
    entry.size = size.QuadPart;
    entry.write_time = attributes.ftLastWriteTime;
    entry.summary = summary;
    cache[path] = std::move(entry);
    return summary;
}

std::wstring FormatTokenCount(uint64_t tokens) {
    if (tokens < 1000) return std::to_wstring(tokens);
    std::wstring value = std::to_wstring((tokens + 500) / 1000);
    value += L"K";
    return value;
}

std::wstring WorkspaceName(const std::string& workspace) {
    if (workspace.empty()) return i18n::Text(L"No project selected");
    const spark::fs::path path = spark::fs::path(Utf8ToWide(workspace));
    const std::wstring name = path.filename().wstring();
    return name.empty() ? Utf8ToWide(workspace) : name;
}

std::string SessionTitleFromMessage(const std::string& message) {
    std::string title = Trim(message);
    const size_t newline = title.find_first_of("\r\n");
    if (newline != std::string::npos) title.resize(newline);
    if (title.size() > 42) {
        title.resize(42);
        title += "...";
    }
    return title.empty() ? "新对话" : title;
}

bool AppendUtf8Line(const std::string& path, const std::string& text) {
    HANDLE file = CreateFileW(Utf8ToWide(path).c_str(), FILE_APPEND_DATA,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const std::string line = text + "\n";
    const bool ok = WriteFile(file, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) != FALSE &&
                    written == line.size();
    CloseHandle(file);
    return ok;
}

bool MoveFileWithFallback(const std::string& source, const std::string& destination, std::string* error) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (MoveFileExW(Utf8ToWide(source).c_str(), Utf8ToWide(destination).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            return true;
        }
        Sleep(25);
    }
    if (CopyFileW(Utf8ToWide(source).c_str(), Utf8ToWide(destination).c_str(), FALSE)) {
        if (DeleteFileW(Utf8ToWide(source).c_str())) return true;
        DeleteFileW(Utf8ToWide(destination).c_str());
    }
    if (error != nullptr) *error = FormatWin32Error(GetLastError());
    return false;
}

bool DeleteFileWithRetry(const std::string& path, std::string* error) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (DeleteFileW(Utf8ToWide(path).c_str())) return true;
        Sleep(30);
    }
    if (error != nullptr) *error = FormatWin32Error(GetLastError());
    return false;
}

std::wstring PermissionLabel(const std::string& mode) {
    if (mode == "approve_for_me") return i18n::Text(L"Approve for me");
    if (mode == "full_access") return i18n::Text(L"Full access");
    return i18n::Text(L"Ask for approval");
}

std::wstring ReasoningLabel(const std::string& effort) {
    if (effort.empty()) return i18n::Text(L"Default");
    if (effort == "low") return i18n::Text(L"Low");
    if (effort == "medium") return i18n::Text(L"Medium");
    if (effort == "high") return i18n::Text(L"High");
    if (effort == "xhigh") return i18n::Text(L"Extra high");
    if (effort == "ultra") return i18n::Text(L"Ultra");
    if (effort == "max") return L"max";
    if (effort == "none") return L"none";
    if (effort == "minimal") return L"minimal";
    return Utf8ToWide(effort);
}

std::wstring ProviderDisplayName(const ProviderConfig& provider) {
    return Utf8ToWide(provider.label.empty() ? provider.id : provider.label);
}

std::wstring ModelDisplayName(const ModelConfig& model) {
    return Utf8ToWide(model.id);
}

const ModelConfig* ActiveModel(const AppConfig& config) {
    const ProviderConfig* provider = FindProvider(config, config.active.provider);
    return provider == nullptr ? nullptr : FindModel(*provider, config.active.model);
}

COLORREF PermissionColor(const std::string& mode) {
    if (mode == "approve_for_me") return kSuccess;
    if (mode == "full_access") return kError;
    return kWarning;
}

std::wstring SelectFolder(HWND owner) {
    wchar_t path[MAX_PATH]{};
    BROWSEINFOW browse{};
    browse.hwndOwner = owner;
    browse.lpszTitle = i18n::Text(L"Choose a project folder");
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST item = SHBrowseForFolderW(&browse);
    if (item == nullptr) return L"";
    const BOOL ok = SHGetPathFromIDListW(item, path);
    CoTaskMemFree(item);
    return ok ? path : L"";
}

std::string DefaultSparkWorkspace() {
    wchar_t documents[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, documents))) {
        const std::wstring path = std::wstring(documents) + L"\\Spark";
        std::error_code error;
        spark::fs::create_directories(path, error);
        return WideToUtf8(path);
    }
    return JoinPath(GetExecutableDirectory(), "Spark");
}

bool IsLegacyDefaultWorkspacePath(const std::string& configured, const std::string& config_path) {
    if (configured.empty()) return true;
    const std::string resolved = NormalizePathForComparison(
        AbsolutePath(ResolveConfigPath(configured, config_path), ParentPath(config_path)));
    return resolved == NormalizePathForComparison(GetExecutableDirectory()) ||
           resolved == NormalizePathForComparison(CurrentDirectoryUtf8());
}

int ListItemAtPoint(HWND list, int x, int y, int item_count) {
    if (list == nullptr || item_count <= 0) return -1;
    const LRESULT hit = SendMessageW(list, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
    if (HIWORD(hit) == 0) {
        const int index = static_cast<int>(LOWORD(hit));
        if (index >= 0 && index < item_count) return index;
    }

    RECT client{};
    GetClientRect(list, &client);
    if (x < client.left || x >= client.right || y < client.top || y >= client.bottom) return -1;
    const int top = static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0));
    const int item_height = static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0));
    if (item_height <= 0) return -1;
    const int index = top + (y - client.top) / item_height;
    return index >= 0 && index < item_count ? index : -1;
}

}  // namespace

namespace {

struct TextPromptState {
    std::wstring result;
    HWND edit = nullptr;
    bool finished = false;
    bool accepted = false;
};

LRESULT CALLBACK TextPromptProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<TextPromptState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<TextPromptState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_COMMAND && LOWORD(wparam) == 1) {
        const int length = GetWindowTextLengthW(state->edit);
        std::wstring value(static_cast<size_t>(length) + 1, L'\0');
        GetWindowTextW(state->edit, value.data(), length + 1);
        value.resize(static_cast<size_t>(length));
        state->result = std::move(value);
        state->accepted = true;
        state->finished = true;
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_CLOSE || (message == WM_COMMAND && LOWORD(wparam) == 2)) {
        state->finished = true;
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        state->finished = true;
        return 0;
    }
    if (message == WM_CTLCOLOREDIT) {
        HDC device = reinterpret_cast<HDC>(wparam);
        SetTextColor(device, kText);
        SetBkColor(device, kInput);
        return reinterpret_cast<LRESULT>(CachedBrush(kInput));
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring PromptText(HWND owner, const std::wstring& title, const std::wstring& initial, bool* accepted = nullptr) {
    if (accepted) *accepted = false;
    static bool registered = false;
    constexpr wchar_t kPromptClass[] = L"SparkTextPrompt";
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = TextPromptProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = CachedBrush(kBackground);
        window_class.lpszClassName = kPromptClass;
        RegisterClassExW(&window_class);
        registered = true;
    }
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    const int width = 520;
    const int height = 190;
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    TextPromptState state;
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kPromptClass, title.c_str(),
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, width, height, owner,
                                  nullptr, instance, &state);
    if (window == nullptr) return L"";
    HFONT font = MakeFont(10, L"Microsoft YaHei UI");
    state.edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initial.c_str(),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL, 18, 22, width - 36, 30,
                                 window, ControlId(10), instance, nullptr);
    HWND ok = CreateWindowW(L"BUTTON", i18n::Text(L"OK"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                            width - 190, 76, 80, 32, window, ControlId(1), instance, nullptr);
    HWND cancel = CreateWindowW(L"BUTTON", i18n::Text(L"Cancel"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                                width - 100, 76, 80, 32, window, ControlId(2), instance, nullptr);
    SendMessageW(state.edit, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(ok, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(state.edit);
    MSG message{};
    while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            state.finished = true;
            DestroyWindow(window);
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (font != nullptr) DeleteObject(font);
    if (accepted) *accepted = state.accepted;
    return state.result;
}

struct ProjectDialogResult {
    bool accepted = false;
    bool remove_requested = false;
    std::wstring name;
    std::vector<std::string> folders;
};

struct ProjectDialogState {
    std::wstring name;
    std::vector<std::wstring> folders;
    HWND name_edit = nullptr;
    HWND folder_list = nullptr;
    HWND add_button = nullptr;
    HWND remove_button = nullptr;
    HWND cancel_button = nullptr;
    HWND save_button = nullptr;
    HFONT font = nullptr;
    bool allow_remove = false;
    bool accepted = false;
    bool remove_requested = false;
    bool finished = false;
    int hovered_folder = -1;
};

void DrawFolderGlyph(HDC device, int x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    MoveToEx(device, x - 7, center_y + 6, nullptr);
    LineTo(device, x - 7, center_y - 6);
    LineTo(device, x - 2, center_y - 6);
    LineTo(device, x + 1, center_y - 3);
    LineTo(device, x + 8, center_y - 3);
    LineTo(device, x + 8, center_y + 6);
    LineTo(device, x - 7, center_y + 6);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawPencilPaperGlyph(HDC device, int left, int top, int right, int bottom, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    const int cx = (left + right) / 2;
    const int cy = (top + bottom) / 2;
    Rectangle(device, cx - 8, cy - 7, cx + 5, cy + 7);
    MoveToEx(device, cx - 1, cy + 5, nullptr);
    LineTo(device, cx + 9, cy - 5);
    LineTo(device, cx + 6, cy - 8);
    LineTo(device, cx - 4, cy + 2);
    LineTo(device, cx - 1, cy + 5);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawTrashGlyph(HDC device, int center_x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    Rectangle(device, center_x - 5, center_y - 4, center_x + 5, center_y + 6);
    MoveToEx(device, center_x - 7, center_y - 5, nullptr);
    LineTo(device, center_x + 7, center_y - 5);
    MoveToEx(device, center_x - 2, center_y - 7, nullptr);
    LineTo(device, center_x + 2, center_y - 7);
    MoveToEx(device, center_x - 2, center_y - 1, nullptr);
    LineTo(device, center_x - 2, center_y + 3);
    MoveToEx(device, center_x + 2, center_y - 1, nullptr);
    LineTo(device, center_x + 2, center_y + 3);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawPinGlyph(HDC device, int center_x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    Ellipse(device, center_x - 5, center_y - 7, center_x + 5, center_y - 1);
    MoveToEx(device, center_x - 7, center_y - 1, nullptr);
    LineTo(device, center_x + 7, center_y - 1);
    MoveToEx(device, center_x, center_y - 1, nullptr);
    LineTo(device, center_x, center_y + 7);
    MoveToEx(device, center_x - 3, center_y + 7, nullptr);
    LineTo(device, center_x + 3, center_y + 7);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

void DrawArchiveGlyph(HDC device, int center_x, int center_y, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ old_pen = SelectObject(device, pen);
    HGDIOBJ old_brush = SelectObject(device, GetStockObject(NULL_BRUSH));
    Rectangle(device, center_x - 7, center_y - 6, center_x + 7, center_y - 2);
    Rectangle(device, center_x - 5, center_y - 2, center_x + 5, center_y + 7);
    MoveToEx(device, center_x - 3, center_y + 1, nullptr);
    LineTo(device, center_x + 3, center_y + 1);
    SelectObject(device, old_brush);
    SelectObject(device, old_pen);
    DeleteObject(pen);
}

int SidebarChatActionForX(int x, const RECT& item_rect) {
    const int right = item_rect.right;
    if (x >= right - ScaleForSystemDpi(32)) return kHistoryArchive;
    if (x >= right - ScaleForSystemDpi(64)) return kHistoryPin;
    if (x >= right - ScaleForSystemDpi(96)) return kHistoryDelete;
    return 0;
}

LRESULT CALLBACK ProjectDialogProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<ProjectDialogState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = static_cast<ProjectDialogState*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam);
        const int notification = HIWORD(wparam);
        if (id == 1 && notification == BN_CLICKED) {
            const int length = GetWindowTextLengthW(state->name_edit);
            std::wstring name(static_cast<size_t>(length) + 1, L'\0');
            GetWindowTextW(state->name_edit, name.data(), length + 1);
            name.resize(static_cast<size_t>(length));
            if (state->folders.empty()) {
                MessageBoxW(window, i18n::Text(L"Add at least one source folder first."), i18n::Text(L"Create project"), MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            state->name = std::move(name);
            state->accepted = true;
            state->finished = true;
            DestroyWindow(window);
            return 0;
        }
        if (id == 2 && notification == BN_CLICKED) {
            state->finished = true;
            DestroyWindow(window);
            return 0;
        }
        if (id == 3 && notification == BN_CLICKED) {
            const std::wstring folder = SelectFolder(window);
            if (!folder.empty()) {
                const bool exists = std::any_of(state->folders.begin(), state->folders.end(), [&](const std::wstring& item) {
                    return _wcsicmp(item.c_str(), folder.c_str()) == 0;
                });
                if (!exists) {
                    state->folders.push_back(folder);
                    SendMessageW(state->folder_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(folder.c_str()));
                }
            }
            return 0;
        }
        if (id == 4 && notification == BN_CLICKED) {
            state->remove_requested = true;
            state->finished = true;
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_MOUSEMOVE && state->folder_list != nullptr) {
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        MapWindowPoints(window, state->folder_list, &point, 1);
        const LRESULT hit = SendMessageW(state->folder_list, LB_ITEMFROMPOINT, 0, MAKELPARAM(point.x, point.y));
        const int index = HIWORD(hit) == 0 ? LOWORD(hit) : -1;
        if (index != state->hovered_folder) {
            state->hovered_folder = index;
            InvalidateRect(state->folder_list, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    if (message == WM_MOUSELEAVE) {
        state->hovered_folder = -1;
        if (state->folder_list != nullptr) InvalidateRect(state->folder_list, nullptr, FALSE);
        return 0;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX) {
        HDC device = reinterpret_cast<HDC>(wparam);
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, kText);
        SetBkColor(device, message == WM_CTLCOLOREDIT ? kInput : kPanel);
        return reinterpret_cast<LRESULT>(CachedBrush(message == WM_CTLCOLOREDIT ? kInput : kPanel));
    }
    if (message == WM_DRAWITEM) {
        const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
        const bool pressed = (item->itemState & ODS_SELECTED) != 0;
        if (item->CtlType == ODT_BUTTON) {
            COLORREF fill = pressed ? kSelection : kPanel;
            COLORREF text_color = kText;
            if (item->CtlID == 4) {
                fill = pressed ? RGB(85, 48, 48) : RGB(63, 43, 45);
                text_color = kError;
            } else if (item->CtlID == 1) {
                fill = pressed ? RGB(205, 207, 212) : RGB(236, 237, 240);
                text_color = RGB(30, 31, 33);
            }
            FillRect(item->hDC, &item->rcItem, CachedBrush(fill));
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, text_color);
            HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, state->font));
            wchar_t text[256]{};
            GetWindowTextW(item->hwndItem, text, 255);
            RECT text_rect = item->rcItem;
            DrawTextW(item->hDC, text, -1, &text_rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(item->hDC, old_font);
            return TRUE;
        }
        if (item->CtlType == ODT_LISTBOX && item->CtlID == 10) {
            const bool hovered = static_cast<int>(item->itemID) == state->hovered_folder;
            FillRect(item->hDC, &item->rcItem, CachedBrush(hovered ? RGB(52, 53, 57) : kPanel));
            wchar_t text[1024]{};
            SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
            const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
            FillRect(item->hDC, &item->rcItem, CachedBrush(hovered ? RGB(52, 53, 57) : kPanel));
            DrawFolderGlyph(item->hDC, item->rcItem.left + 18, center_y, kMuted);
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, kText);
            HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, state->font));
            RECT text_rect = item->rcItem;
            text_rect.left += 38;
            text_rect.right -= hovered ? 42 : 14;
            DrawTextW(item->hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            SelectObject(item->hDC, old_font);
            if (hovered) {
                HPEN close_pen = CreatePen(PS_SOLID, 1, kError);
                HGDIOBJ old_pen = SelectObject(item->hDC, close_pen);
                const int x = item->rcItem.right - 24;
                MoveToEx(item->hDC, x - 4, center_y - 4, nullptr);
                LineTo(item->hDC, x + 4, center_y + 4);
                MoveToEx(item->hDC, x + 4, center_y - 4, nullptr);
                LineTo(item->hDC, x - 4, center_y + 4);
                SelectObject(item->hDC, old_pen);
                DeleteObject(close_pen);
            }
            return TRUE;
        }
    }
    if (message == WM_LBUTTONUP && state->folder_list != nullptr && state->hovered_folder >= 0) {
        RECT list_rect{};
        GetWindowRect(state->folder_list, &list_rect);
        POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ClientToScreen(window, &point);
        if (point.x >= list_rect.right - 42 && point.x <= list_rect.right) {
            state->folders.erase(state->folders.begin() + state->hovered_folder);
            SendMessageW(state->folder_list, LB_DELETESTRING, state->hovered_folder, 0);
            state->hovered_folder = -1;
            return 0;
        }
    }
    if (message == WM_CLOSE) {
        state->finished = true;
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_DESTROY) {
        state->finished = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK ProjectFolderListSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                               UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* state = reinterpret_cast<ProjectDialogState*>(reference_data);
    if (state == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const LRESULT hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        const int index = HIWORD(hit) == 0 ? LOWORD(hit) : -1;
        if (index != state->hovered_folder) {
            state->hovered_folder = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
        TrackMouseEvent(&tracking);
        return 0;
    }
    if (message == WM_MOUSELEAVE) {
        state->hovered_folder = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const LRESULT hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, MAKELPARAM(x, y));
        const int index = HIWORD(hit) == 0 ? LOWORD(hit) : -1;
        if (index >= 0 && index < static_cast<int>(state->folders.size())) {
            RECT rect{};
            SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect));
            if (x >= rect.right - ScaleForSystemDpi(42)) {
                state->folders.erase(state->folders.begin() + index);
                SendMessageW(window, LB_DELETESTRING, index, 0);
                state->hovered_folder = -1;
                return 0;
            }
        }
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(window, ProjectFolderListSubclassProc, subclass_id);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

ProjectDialogResult ShowProjectDialog(HWND owner, const std::wstring& title, const std::wstring& initial_name,
                                       const std::vector<std::string>& initial_folders, bool allow_remove) {
    static bool registered = false;
    constexpr wchar_t kDialogClass[] = L"SparkProjectDialog";
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (!registered) {
        WNDCLASSEXW window_class{};
        window_class.cbSize = sizeof(window_class);
        window_class.lpfnWndProc = ProjectDialogProc;
        window_class.hInstance = instance;
        window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        window_class.hbrBackground = CachedBrush(kBackground);
        window_class.lpszClassName = kDialogClass;
        RegisterClassExW(&window_class);
        registered = true;
    }
    RECT owner_rect{};
    GetWindowRect(owner, &owner_rect);
    const int width = ScaleForSystemDpi(560);
    const int height = ScaleForSystemDpi(410);
    const int x = owner_rect.left + ((owner_rect.right - owner_rect.left) - width) / 2;
    const int y = owner_rect.top + ((owner_rect.bottom - owner_rect.top) - height) / 2;
    ProjectDialogState state;
    state.name = initial_name;
    state.allow_remove = allow_remove;
    for (const std::string& folder : initial_folders) state.folders.push_back(Utf8ToWide(folder));
    state.font = MakeFont(10, L"Microsoft YaHei UI");
    HWND window = CreateWindowExW(WS_EX_DLGMODALFRAME, kDialogClass, title.c_str(),
                                  WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, x, y, width, height, owner,
                                  nullptr, instance, &state);
    if (window == nullptr) {
        if (state.font != nullptr) DeleteObject(state.font);
        return {};
    }
    HWND name_label = CreateWindowW(L"STATIC", i18n::Text(L"Project name"), WS_CHILD | WS_VISIBLE | SS_LEFT, ScaleForSystemDpi(24),
                                    ScaleForSystemDpi(18), width - ScaleForSystemDpi(48), ScaleForSystemDpi(20), window,
                                    nullptr, instance, nullptr);
    state.name_edit = CreateWindowExW(0, L"EDIT", initial_name.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                      ScaleForSystemDpi(24), ScaleForSystemDpi(42), width - ScaleForSystemDpi(48),
                                      ScaleForSystemDpi(34), window, ControlId(20), instance, nullptr);
    HWND source_label = CreateWindowW(L"STATIC", i18n::Text(L"Source folders"), WS_CHILD | WS_VISIBLE | SS_LEFT, ScaleForSystemDpi(24),
                                      ScaleForSystemDpi(94), width - ScaleForSystemDpi(48), ScaleForSystemDpi(20), window,
                                      nullptr, instance, nullptr);
    state.folder_list = CreateWindowExW(0, L"LISTBOX", L"",
                                        WS_CHILD | WS_VISIBLE | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY | LBS_HASSTRINGS |
                                            LBS_OWNERDRAWFIXED,
                                        ScaleForSystemDpi(24), ScaleForSystemDpi(120), width - ScaleForSystemDpi(48),
                                        ScaleForSystemDpi(150), window, ControlId(10), instance, nullptr);
    state.add_button = CreateWindowW(L"BUTTON", i18n::Text(L"Add folder"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                     ScaleForSystemDpi(24), ScaleForSystemDpi(278), width - ScaleForSystemDpi(48),
                                     ScaleForSystemDpi(34), window, ControlId(3), instance, nullptr);
    const int button_y = height - ScaleForSystemDpi(84);
    state.remove_button = CreateWindowW(L"BUTTON", i18n::Text(L"Remove local project"), WS_CHILD | (allow_remove ? WS_VISIBLE : 0) |
                                                                                   WS_TABSTOP | BS_OWNERDRAW,
                                        ScaleForSystemDpi(24), button_y,
                                        ScaleForSystemDpi(142), ScaleForSystemDpi(34), window, ControlId(4), instance,
                                        nullptr);
    state.cancel_button = CreateWindowW(L"BUTTON", i18n::Text(L"Cancel"), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                                        width - ScaleForSystemDpi(220), button_y,
                                        ScaleForSystemDpi(84), ScaleForSystemDpi(34), window, ControlId(2), instance,
                                        nullptr);
    state.save_button = CreateWindowW(L"BUTTON", allow_remove ? i18n::Text(L"Save") : i18n::Text(L"Create project"),
                                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, width - ScaleForSystemDpi(126),
                                      button_y, ScaleForSystemDpi(102), ScaleForSystemDpi(34), window, ControlId(1),
                                      instance, nullptr);
    for (HWND control : {name_label, state.name_edit, source_label, state.folder_list, state.add_button,
                         state.remove_button, state.cancel_button, state.save_button}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(state.font), TRUE);
    }
    SendMessageW(state.folder_list, LB_SETITEMHEIGHT, 0, ScaleForSystemDpi(34));
    SendMessageW(state.folder_list, LB_SETHORIZONTALEXTENT, width, 0);
    SetWindowSubclass(state.folder_list, ProjectFolderListSubclassProc, 11, reinterpret_cast<DWORD_PTR>(&state));
    for (const std::wstring& folder : state.folders) {
        SendMessageW(state.folder_list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(folder.c_str()));
    }
    ApplyDarkTitleBar(window);
    EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetFocus(state.name_edit);
    MSG message{};
    while (!state.finished && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) {
            state.finished = true;
            DestroyWindow(window);
            break;
        }
        if (!IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.font != nullptr) DeleteObject(state.font);
    ProjectDialogResult result;
    result.accepted = state.accepted;
    result.remove_requested = state.remove_requested;
    result.name = state.name;
    for (const std::wstring& folder : state.folders) result.folders.push_back(WideToUtf8(folder));
    return result;
}

}  // namespace

AppWindow::~AppWindow() {
    session_loader_.Stop();
    RemoveTrayIcon();
    CancelAndJoinAllTurns();
    if (subagents_ != nullptr) subagents_.reset();
    for (const auto& entry : attachment_bitmaps_) {
        if (entry.second != nullptr) DeleteObject(entry.second);
    }
    attachment_bitmaps_.clear();
    DeleteFileW(Utf8ToWide(StartupMarkerPath()).c_str());
}

std::shared_ptr<AppWindow::TurnRuntime> AppWindow::FindTurn(const std::string& path) const {
    if (path.empty()) return {};
    const auto found = active_turns_.find(NormalizePathForComparison(path));
    return found == active_turns_.end() ? std::shared_ptr<TurnRuntime>{} : found->second;
}

bool AppWindow::AnyTurnRunning() const {
    return !active_turns_.empty() || !external_running_sessions_.empty() ||
           (subagents_ != nullptr && subagents_->HasRunning());
}

bool AppWindow::IsSessionRunning(const std::string& path) const {
    if (FindTurn(path) != nullptr) return true;
    return external_running_sessions_.find(NormalizePathForComparison(path)) != external_running_sessions_.end();
}

bool AppWindow::IsActiveSessionRunning() const {
    // A pending new conversation has no journal path yet. It must not inherit the
    // running state of whichever conversation was previously on screen.
    return !SessionLoading() && !pending_new_conversation_ && IsSessionRunning(journal_.Path());
}

void AppWindow::DetachAttachedTurn() {
    if (attached_turn_key_.empty()) return;
    const auto found = active_turns_.find(attached_turn_key_);
    if (found != active_turns_.end()) found->second->ui_attached = false;
    attached_turn_key_.clear();
}

void AppWindow::AttachTurnUi(const std::shared_ptr<TurnRuntime>& runtime) {
    if (runtime == nullptr) return;
    current_phase_ = runtime->phase;
    phase_text_ = runtime->phase_text;
    reasoning_preview_ = runtime->reasoning_preview;
    turn_started_tick_ = runtime->started_tick;
    active_reasoning_entry_ = static_cast<size_t>(-1);
    latest_reasoning_entry_ = static_cast<size_t>(-1);
    active_assistant_entry_ = static_cast<size_t>(-1);
    assistant_stream_started_ = false;
    if (chat_view_ != nullptr) {
        chat_view_->SetActivityState(IsActiveSessionRunning() && ActivityRowVisible(), phase_text_, runtime->started_tick);
    }
}

void AppWindow::CancelAndJoinAllTurns() {
    if (subagents_ != nullptr) subagents_->CancelAll();
    if (active_turns_.empty()) return;
    std::map<std::string, std::shared_ptr<TurnRuntime>> turns;
    turns.swap(active_turns_);
    attached_turn_key_.clear();
    for (auto& entry : turns) entry.second->cancellation.Cancel();
    for (auto& entry : turns) {
        if (entry.second->worker.joinable()) entry.second->worker.join();
    }
}

void AppWindow::UpdateTurnUi() {
    UpdateSendButton();
    if (AnyTurnRunning()) BeginActivityAnimation();
    else StopActivityAnimation();
    if (history_list_ != nullptr) InvalidateRect(history_list_, nullptr, FALSE);
    if (projects_list_ != nullptr) InvalidateRect(projects_list_, nullptr, FALSE);
}

void AppWindow::TickTurnWatchdog() {
    if (active_turns_.empty()) {
        KillTimer(window_, kTurnWatchdogTimer);
        return;
    }
    std::vector<std::shared_ptr<TurnRuntime>> turns;
    turns.reserve(active_turns_.size());
    for (const auto& entry : active_turns_) turns.push_back(entry.second);
    for (const auto& runtime : turns) {
        if (runtime->complete.load()) {
            const std::string reason = runtime->interrupt_requested.load()
                                           ? "paused"
                                           : runtime->cancelled.load() ? "cancelled" : "";
            FinishTurn(runtime, runtime->success.load(), reason);
            continue;
        }
        const uint64_t silence_ms = spark::MonotonicMillis() - runtime->last_progress_tick.load();
        const int patience_seconds = std::max(30, std::max(runtime->config.agent.request_timeout_seconds,
                                                            runtime->config.agent.response_header_timeout_seconds));
        if (silence_ms >= static_cast<uint64_t>(patience_seconds) * 1000 + 30000) {
            runtime->cancellation.Cancel();
            if (IsSessionRunning(journal_.Path()) &&
                NormalizePathForComparison(journal_.Path()) == runtime->session_key) {
                SetStatus(i18n::Text(L"The model or tool timed out. Cancelling this turn..."));
            }
        }
    }
}

void AppWindow::AddTrayIcon() {
    if (window_ == nullptr || tray_icon_added_) return;
    if (tray_icon_ == nullptr) {
        HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window_, GWLP_HINSTANCE));
        tray_icon_ = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResource), IMAGE_ICON,
                                                       GetSystemMetrics(SM_CXSMICON),
                                                       GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
        if (tray_icon_ == nullptr) tray_icon_ = LoadIconW(nullptr, IDI_APPLICATION);
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = window_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayMessage;
    data.hIcon = tray_icon_;
    wcscpy_s(data.szTip, L"Spark");
    tray_icon_added_ = Shell_NotifyIconW(NIM_ADD, &data) != FALSE;
}

void AppWindow::RemoveTrayIcon() {
    if (window_ != nullptr && tray_icon_added_) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = window_;
        data.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
    }
    tray_icon_added_ = false;
    if (tray_icon_ != nullptr) {
        DestroyIcon(tray_icon_);
        tray_icon_ = nullptr;
    }
}

void AppWindow::RestoreFromTray() {
    if (exiting_) return;
    AddTrayIcon();
    ShowWindow(window_, SW_SHOW);
    ShowWindow(window_, SW_RESTORE);
    SetForegroundWindow(window_);
}

void AppWindow::OpenSettingsPage() {
    SetSettingsMode(true);
    settings_.Open(window_, config_, [this](const AppConfig& saved) { ApplyConfiguration(saved); },
                   [this]() { HandleSettingsDataChanged(); });
}

void AppWindow::ShowTrayMenu() {
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) return;
    AppendMenuW(menu, MF_STRING, kTraySettings, i18n::Text(L"Settings"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTrayExit, i18n::Text(L"Quit Spark"));
    POINT cursor{};
    GetCursorPos(&cursor);
    SetForegroundWindow(window_);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, cursor.x, cursor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    PostMessageW(window_, WM_NULL, 0, 0);
    if (command == kTraySettings) {
        RestoreFromTray();
        OpenSettingsPage();
    }
    else if (command == kTrayExit) RequestExit();
}

void AppWindow::RequestExit() {
    exiting_ = true;
    snapshot_capture_.Cancel();
    SaveComposerState();
    if (subagents_ != nullptr) subagents_->CancelAll();
    // External MCP sessions are owned by another process and do not deliver a
    // local FinishTurn callback. Wait only for turns this window can cancel/join.
    if (!active_turns_.empty()) {
        close_pending_ = true;
        for (auto& entry : active_turns_) entry.second->cancellation.Cancel();
        // Keep pumping messages so pending approvals can return and workers can
        // finish even if their final notification was lost.
        SetTimer(window_, kTurnWatchdogTimer, 100, nullptr);
        EnableWindow(window_, FALSE);
        return;
    }
    close_pending_ = false;
    if (window_ != nullptr) DestroyWindow(window_);
}

int AppWindow::Run(HINSTANCE instance, int show_command) {
    instance_ = instance;
    Gdiplus::GdiplusStartupInput gdiplus_input;
    if (Gdiplus::GdiplusStartup(&g_gdiplus_token, &gdiplus_input, nullptr) != Gdiplus::Ok) {
        g_gdiplus_token = 0;
    }
    const std::string startup_marker = StartupMarkerPath();
    std::error_code marker_error;
    spark::fs::create_directories(Utf8ToWide(ParentPath(startup_marker)), marker_error);
    g_startup_recovery_mode = PathExists(startup_marker);
    std::string marker_write_error;
    (void)WriteFileUtf8Atomic(startup_marker, "running\n", &marker_write_error);
    INITCOMMONCONTROLSEX common_controls{sizeof(common_controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common_controls);
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class);
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = WindowProc;
    window_class.hInstance = instance;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(kAppIconResource));
    window_class.hIconSm = reinterpret_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kAppIconResource),
                                                              IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                              GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    if (window_class.hIconSm == nullptr) window_class.hIconSm = window_class.hIcon;
    window_class.lpszClassName = kAppWindowClass;
    if (!RegisterClassExW(&window_class)) {
        if (g_gdiplus_token != 0) Gdiplus::GdiplusShutdown(g_gdiplus_token);
        g_gdiplus_token = 0;
        return 1;
    }
    RECT work_area{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    const int work_width = static_cast<int>(work_area.right - work_area.left);
    const int work_height = static_cast<int>(work_area.bottom - work_area.top);
    const int margin = ScaleForSystemDpi(12);
    const auto size = InitialWindowSize({work_width, work_height},
        {ScaleForSystemDpi(980), ScaleForSystemDpi(640)}, margin);
    const int width = size.width, height = size.height;
    const int x = static_cast<int>(work_area.left) + std::max(0, (work_width - width) / 2);
    const int y = static_cast<int>(work_area.top) + std::max(0, (work_height - height) / 2);
    window_ = CreateWindowExW(0, kAppWindowClass, L"Spark", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, x, y,
                              width, height, nullptr, nullptr, instance, this);
    if (window_ == nullptr) return 1;
    show_command = RestoreWindowsWindowGeometry(window_, DataDirectory(), show_command,
        {Dpi(760), Dpi(520)}, margin);
    snapshot_shortcut_.Start(window_, kSnapshotMessage);
    ApplyDarkTitleBar(window_);
    ShowWindow(window_, show_command);
    ApplyDarkTitleBar(window_);
    SetWindowPos(window_, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    UpdateWindow(window_);
    // Keep the tray icon present for the whole application lifetime. Hiding the window
    // therefore no longer needs to create the icon, and restoring it does not remove it.
    AddTrayIcon();
    // Child STATIC controls created inside WM_CREATE can miss their first paint
    // because the top-level window is not visible yet. Refresh once after the
    // window has actually been shown, without repainting the full application.
    UpdateEmptyState();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    if (g_gdiplus_token != 0) Gdiplus::GdiplusShutdown(g_gdiplus_token);
    g_gdiplus_token = 0;
    return static_cast<int>(message.wParam);
}

void AppWindow::CreateControls() {
    background_brush_ = CachedBrush(kBackground);
    panel_brush_ = CachedBrush(kSidebar);
    input_brush_ = CachedBrush(kInput);
    ui_font_ = MakeFont(10, L"Microsoft YaHei UI");
    chat_font_ = MakeFont(11, L"Microsoft YaHei UI");
    mono_font_ = MakeFont(10, L"Consolas");
    brand_font_ = MakeFont(13, L"Microsoft YaHei UI", FW_SEMIBOLD);
    empty_mark_font_ = MakeFont(22, L"Microsoft YaHei UI", FW_SEMIBOLD);
    empty_title_font_ = MakeFont(15, L"Microsoft YaHei UI", FW_SEMIBOLD);

    auto button = [&](const wchar_t* text, int id) {
        return CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                             0, 0, 0, 0, window_, ControlId(id), instance_, nullptr);
    };
    auto label = [&](const wchar_t* text, int id, DWORD style) {
        return CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 0, 0, window_, ControlId(id), instance_, nullptr);
    };

    sidebar_title_ = label(L"Spark", kSidebarTitle, SS_LEFT);
    new_button_ = button(i18n::Text(L"New chat"), kNewConversation);
    project_label_ = label(i18n::Text(L"Projects"), kProjectLabel, SS_LEFT | SS_NOTIFY | SS_OWNERDRAW);
    projects_list_ = CreateWindowExW(0, L"LISTBOX", L"",
                                     WS_CHILD | WS_VISIBLE | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY |
                                         LBS_HASSTRINGS | LBS_OWNERDRAWFIXED,
                                     0, 0, 0, 0, window_, ControlId(kProjectsList), instance_, nullptr);
    project_menu_button_ = button(L"...", kProjectMenuButton);
    project_add_button_ = button(L"+", kProjectAddButton);
    recent_label_ = label(i18n::Text(L"Recent"), kRecentLabel, SS_LEFT | SS_NOTIFY | SS_OWNERDRAW);
    history_list_ = CreateWindowExW(0, L"LISTBOX", L"",
                                    WS_CHILD | WS_VISIBLE | LBS_NOINTEGRALHEIGHT | LBS_NOTIFY |
                                        LBS_HASSTRINGS | LBS_OWNERDRAWFIXED,
                                    0, 0, 0, 0, window_, ControlId(kHistoryList), instance_, nullptr);
    recent_menu_button_ = button(L"...", kRecentMenuButton);
    recent_new_button_ = button(L"+", kRecentNewButton);
    sidebar_scrollbar_ = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOTIFY, 0, 0, 0, 0, window_,
                                       ControlId(kSidebarScrollbar), instance_, nullptr);
    settings_button_ = button(i18n::Text(L"Settings"), kSettingsButton);

    main_title_label_ = label(i18n::Text(L"New chat"), kMainTitle, SS_LEFT | SS_ENDELLIPSIS);
    context_label_ = label(L"", kContextLabel, SS_RIGHT | SS_ENDELLIPSIS);
    summary_button_ = button(L"", kSummaryButton);
    chat_view_ = std::make_unique<ChatView>();
    if (!chat_view_->Create(window_, instance_, kChat, chat_font_, mono_font_)) {
        MessageBoxW(window_, i18n::Text(L"Could not create the chat view."), i18n::Text(L"Startup failed"), MB_OK | MB_ICONERROR);
    }
    chat_view_->SetActions([this](size_t index) { ToggleToolEntry(index); },
                           [this](size_t index) { CopyChatItem(index); },
                           [this](size_t index) { EditChatItem(index); });
    chat_view_->SetGroupAction([this](size_t index) { ToggleToolGroupEntry(index); });
    side_chat_view_ = std::make_unique<ChatView>();
    if (!side_chat_view_->Create(window_, instance_, kSideChat, chat_font_, mono_font_)) {
        side_chat_view_.reset();
    }
    if (side_chat_view_ != nullptr) {
        side_chat_view_->SetActions([this](size_t index) { side_chat_view_->ToggleTool(index); },
                                    [this](size_t index) {
                                        const ChatItem* item = side_chat_view_->Item(index);
                                        if (item == nullptr || !OpenClipboard(window_)) return;
                                        const std::wstring text = Utf8ToWide(item->text);
                                        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (text.size() + 1) * sizeof(wchar_t));
                                        if (memory != nullptr) {
                                            void* target = GlobalLock(memory);
                                            if (target != nullptr) {
                                                memcpy(target, text.c_str(), (text.size() + 1) * sizeof(wchar_t));
                                                GlobalUnlock(memory);
                                                EmptyClipboard();
                                                if (!SetClipboardData(CF_UNICODETEXT, memory)) GlobalFree(memory);
                                            } else GlobalFree(memory);
                                        }
                                        CloseClipboard();
                                    }, [](size_t) {});
        side_chat_view_->SetGroupAction([this](size_t index) { side_chat_view_->ToggleToolGroup(index); });
    }
    CreateSummaryControls();
    composer_ = CreateWindowExW(0, L"EDIT", L"",
                                WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                                0, 0, 0, 0, window_, ControlId(kComposer), instance_, nullptr);
    plus_button_ = button(L"+", kPlusButton);
    permission_button_ = button(i18n::Text(L"Ask for approval"), kPermissionButton);
    model_label_ = label(i18n::Text(L"Model"), kModelLabel, SS_LEFT);
    model_combo_ = CreateWindowW(L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                     CBS_HASSTRINGS,
                                 0, 0, 0, 0, window_, ControlId(kModelCombo), instance_, nullptr);
    model_button_ = button(L"", kModelButton);
    send_button_ = button(L"↑", kSend);
    workspace_label_ = label(L"", kWorkspaceLabel, SS_LEFT | SS_ENDELLIPSIS);
    status_label_ = label(L"", kStatusLabel, SS_LEFT | SS_ENDELLIPSIS);
    empty_mark_ = label(L"", kEmptyMark, SS_OWNERDRAW | SS_NOPREFIX);
    empty_title_ = label(i18n::Text(L"What do you want to do?"), kEmptyTitle, SS_CENTER | SS_NOPREFIX);
    ShowWindow(model_label_, SW_HIDE);
    ShowWindow(model_combo_, SW_HIDE);
    ShowWindow(workspace_label_, SW_HIDE);
    ShowWindow(summary_button_, SW_SHOW);
    ShowWindow(subagent_button_, SW_HIDE);
    ShowWindow(side_title_label_, SW_HIDE);
    ShowWindow(side_close_button_, SW_HIDE);
    if (side_chat_view_ != nullptr) ShowWindow(side_chat_view_->hwnd(), SW_HIDE);

    for (HWND control : {sidebar_title_, new_button_, project_label_, projects_list_, project_menu_button_,
                         project_add_button_, recent_label_, history_list_, recent_menu_button_, recent_new_button_,
                         settings_button_, main_title_label_, context_label_, summary_button_, subagent_button_,
                         composer_, plus_button_,
                         permission_button_, model_label_, model_combo_, model_button_, send_button_, workspace_label_,
                         status_label_, empty_mark_, empty_title_, side_title_label_, side_close_button_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui_font_), TRUE);
    }
    SendMessageW(sidebar_title_, WM_SETFONT, reinterpret_cast<WPARAM>(brand_font_), TRUE);
    SendMessageW(empty_mark_, WM_SETFONT, reinterpret_cast<WPARAM>(empty_mark_font_), TRUE);
    SendMessageW(empty_title_, WM_SETFONT, reinterpret_cast<WPARAM>(empty_title_font_), TRUE);
    SendMessageW(composer_, WM_SETFONT, reinterpret_cast<WPARAM>(chat_font_), TRUE);
    SendMessageW(composer_, EM_SETLIMITTEXT, 256 * 1024, 0);
    SendMessageW(composer_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(12, 12));
    SendMessageW(composer_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(i18n::Text(L"Message Spark")));
    SendMessageW(side_title_label_, WM_SETFONT, reinterpret_cast<WPARAM>(brand_font_), TRUE);
    SendMessageW(history_list_, LB_SETITEMHEIGHT, 0, Dpi(36));
    SendMessageW(projects_list_, LB_SETITEMHEIGHT, 0, Dpi(34));
    ShowScrollBar(history_list_, SB_VERT, FALSE);
    ShowScrollBar(projects_list_, SB_VERT, FALSE);
    SendMessageW(model_combo_, CB_SETITEMHEIGHT, -1, 28);
    SendMessageW(model_combo_, CB_SETITEMHEIGHT, 0, 28);
    SendMessageW(model_combo_, CB_SETMINVISIBLE, 6, 0);
    SetWindowSubclass(composer_, ComposerSubclassProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(history_list_, HistorySubclassProc, 2, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(projects_list_, SidebarListSubclassProc, 4, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(sidebar_scrollbar_, SidebarScrollbarProc, 5, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(model_combo_, ModelComboSubclassProc, 3, reinterpret_cast<DWORD_PTR>(this));
    SetWindowSubclass(status_label_, StatusLabelSubclassProc, 6, reinterpret_cast<DWORD_PTR>(this));
    ShowWindow(status_label_, SW_HIDE);
    ShowWindow(sidebar_scrollbar_, SW_HIDE);
    SetFocus(composer_);
    UpdateSendButton();
}

AppWindow::LayoutMetrics AppWindow::MeasureLayout() const {
    RECT client{};
    GetClientRect(window_, &client);
    LayoutMetrics metrics;
    metrics.width = client.right - client.left;
    metrics.height = client.bottom - client.top;
    const int sidebar_target = Dpi(236);
    const int sidebar_min = Dpi(190);
    const int sidebar_max = Dpi(300);
    metrics.sidebar = std::min(sidebar_max, std::max(sidebar_min, std::min(sidebar_target, metrics.width * 24 / 100)));
    metrics.main_x = metrics.sidebar + 1;
    metrics.side_width = side_chat_open_
                             ? std::min(std::min(Dpi(600), std::max(Dpi(340), metrics.width * 40 / 100)),
                                        std::max(0, metrics.width - metrics.main_x - Dpi(320)))
                             : 0;
    metrics.side_left = metrics.width - metrics.side_width;
    metrics.main_right = metrics.width - metrics.side_width;
    metrics.main_width = metrics.main_right - metrics.main_x;
    metrics.topbar_height = Dpi(46);
    metrics.composer_width = std::min(Dpi(900), std::max(1, metrics.main_width - Dpi(40)));
    if (!attachments_.empty()) {
        metrics.attachment_size =
            std::clamp(metrics.height / 4, Dpi(80), Dpi(160));
        metrics.attachment_height = metrics.attachment_size + Dpi(16);
    }
    metrics.panel_top =
        std::max(metrics.topbar_height + Dpi(84), metrics.height - Dpi(150) - metrics.attachment_height);
    metrics.panel_bottom = metrics.height - Dpi(12);
    metrics.panel_left = metrics.main_x + std::max(Dpi(20), (metrics.main_width - metrics.composer_width) / 2);
    metrics.panel_right = metrics.panel_left + metrics.composer_width;
    return metrics;
}

void AppWindow::LayoutControls() {
    if (attachments_.empty()) attachment_hit_areas_.clear();
    const LayoutMetrics metrics = MeasureLayout();
    const int width = metrics.width;
    const int height = metrics.height;
    const int sidebar = metrics.sidebar;
    const int main_x = metrics.main_x;
    const int topbar_height = metrics.topbar_height;
    const int composer_width = metrics.composer_width;
    const int panel_top = metrics.panel_top;
    const int panel_bottom = metrics.panel_bottom;
    const int panel_left = metrics.panel_left;
    const int panel_right = metrics.panel_right;

    // Skip identical bounds and commit sibling moves together, without intermediate paints.
    struct Placement {
        HWND control;
        int x;
        int y;
        int width;
        int height;
    };
    std::vector<Placement> placements;
    placements.reserve(32);
    const auto place = [&](HWND control, int x, int y, int control_width, int control_height) {
        if (control == nullptr) return;
        RECT current{};
        GetWindowRect(control, &current);
        MapWindowPoints(nullptr, GetParent(control), reinterpret_cast<POINT*>(&current), 2);
        if (current.left == x && current.top == y && current.right - current.left == control_width &&
            current.bottom - current.top == control_height) return;
        constexpr UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
        if (GetParent(control) == window_) placements.push_back({control, x, y, control_width, control_height});
        else SetWindowPos(control, nullptr, x, y, control_width, control_height, flags);
    };

    place(sidebar_title_, Dpi(16), Dpi(12), sidebar - Dpi(32), Dpi(28));
    place(new_button_, Dpi(10), Dpi(46), sidebar - Dpi(20), Dpi(34));
    const int project_header_y = Dpi(90);
    const int header_button_size = Dpi(24);
    const int action_right = sidebar - Dpi(18);
    const int action_gap = Dpi(4);
    place(project_label_, Dpi(16), project_header_y, sidebar - Dpi(94), Dpi(22));
    place(project_add_button_, action_right - header_button_size, project_header_y - Dpi(3), header_button_size,
          header_button_size);
    place(project_menu_button_, action_right - header_button_size * 2 - action_gap, project_header_y - Dpi(3),
          header_button_size, header_button_size);

    const int settings_top = height - Dpi(42);
    const int projects_top = project_header_y + Dpi(26);
    const int section_header_height = Dpi(24);
    const int section_gap = Dpi(12);
    int project_list_height = 0;
    int recent_label_y = projects_top + section_gap;
    int history_y = recent_label_y + Dpi(26);
    int history_list_height = 0;
    const int available_bottom = std::max(projects_top + Dpi(88), settings_top - Dpi(8));
    if (project_section_expanded_ && !project_rows_.empty()) {
        const int available_for_projects =
            std::max(Dpi(72), available_bottom - projects_top - Dpi(54) - (recent_section_expanded_ ? Dpi(70) : 0));
        const int wanted = std::max(Dpi(34), static_cast<int>(project_rows_.size()) * Dpi(34));
        project_list_height = std::min(wanted, std::max(Dpi(68), available_for_projects * 52 / 100));
        recent_label_y = projects_top + project_list_height + section_gap;
        history_y = recent_label_y + Dpi(26);
    }
    if (recent_section_expanded_) {
        history_list_height = std::max(Dpi(48), available_bottom - history_y);
    }
    const int list_width = sidebar - Dpi(24);
    place(projects_list_, Dpi(6), projects_top, list_width, project_list_height);
    place(recent_label_, Dpi(16), recent_label_y, sidebar - Dpi(94), section_header_height);
    place(recent_new_button_, action_right - header_button_size, recent_label_y - Dpi(3), header_button_size,
          header_button_size);
    place(recent_menu_button_, action_right - header_button_size * 2 - action_gap, recent_label_y - Dpi(3),
          header_button_size, header_button_size);
    place(history_list_, Dpi(6), history_y, list_width, history_list_height);
    int scrollbar_top = projects_top;
    int scrollbar_bottom = history_y + history_list_height;
    if (!project_section_expanded_ || project_list_height <= 0) scrollbar_top = history_y;
    if (!recent_section_expanded_ || history_list_height <= 0) scrollbar_bottom = projects_top + project_list_height;
    if (scrollbar_bottom <= scrollbar_top) scrollbar_bottom = std::min(available_bottom, scrollbar_top + Dpi(48));
    place(sidebar_scrollbar_, sidebar - Dpi(18), scrollbar_top, Dpi(10),
          std::max(Dpi(24), scrollbar_bottom - scrollbar_top));
    place(settings_button_, Dpi(10), settings_top, sidebar - Dpi(20), Dpi(32));

    const int main_right = metrics.main_right;
    const int header_left = main_x + Dpi(20);
    int header_cursor = std::max(header_left, main_right - Dpi(12));
    HDC header_dc = GetDC(window_);
    HGDIOBJ old_header_font = SelectObject(header_dc, ui_font_ != nullptr ? ui_font_ : GetStockObject(DEFAULT_GUI_FONT));
    const auto place_header_control = [&](HWND control, int minimum_width, int top, int control_height) {
        wchar_t text[256]{};
        const int length = GetWindowTextW(control, text, static_cast<int>(std::size(text)));
        SIZE extent{};
        GetTextExtentPoint32W(header_dc, text, length, &extent);
        const int wanted = std::max(minimum_width, static_cast<int>(extent.cx) + Dpi(16));
        const int control_width = std::min(wanted, std::max(0, header_cursor - header_left));
        place(control, header_cursor - control_width, top, control_width, control_height);
        header_cursor = std::max(header_left, header_cursor - control_width - Dpi(12));
    };
    place(summary_button_, header_cursor - Dpi(32), Dpi(7), Dpi(32), Dpi(32));
    header_cursor = std::max(header_left, header_cursor - Dpi(44));
    ShowWindow(subagent_button_, SW_HIDE);
    place_header_control(context_label_, Dpi(195), Dpi(13), Dpi(20));
    SelectObject(header_dc, old_header_font);
    ReleaseDC(window_, header_dc);
    place(main_title_label_, header_left, Dpi(11), std::max(0, header_cursor - header_left), Dpi(24));
    // The status label is intentionally retired. Keep the HWND for compatibility with
    // existing call sites, but do not reserve a strip that can cover transcript text.
    place(status_label_, 0, 0, 0, 0);
    if (chat_view_ != nullptr && chat_view_->hwnd() != nullptr) {
        place(chat_view_->hwnd(), main_x + Dpi(16), topbar_height + Dpi(8), main_right - main_x - Dpi(32),
              std::max(Dpi(100), panel_top - topbar_height - Dpi(16)));
    }
    if (!settings_mode_) {
        const int empty_center = (topbar_height + panel_top) / 2;
        place(empty_mark_, main_x + Dpi(16), empty_center - Dpi(52), main_right - main_x - Dpi(32), Dpi(40));
        place(empty_title_, main_x + Dpi(16), empty_center - Dpi(8), main_right - main_x - Dpi(32), Dpi(32));
    }

    if (summary_panel_ != nullptr) {
        place(summary_panel_->hwnd(), metrics.side_left, 0, metrics.side_width, height);
        ShowWindow(summary_panel_->hwnd(), side_chat_open_ && !settings_mode_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    if (side_chat_view_ != nullptr) {
        const int header = summary_panel_ != nullptr ? summary_panel_->HeaderHeight() : Dpi(88);
        place(side_chat_view_->hwnd(), 0, header, metrics.side_width, std::max(1, height - header));
        ShowWindow(side_chat_view_->hwnd(),
                   side_chat_open_ && !active_subagent_id_.empty() && !settings_mode_ ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    const int panel_height = panel_bottom - panel_top;
    (void)panel_height;
    const int row_top = panel_bottom - Dpi(40);
    place(workspace_label_, panel_left + Dpi(16), panel_top + Dpi(9), std::min(Dpi(220), composer_width / 2), Dpi(18));
    const int composer_top = panel_top + Dpi(10) + metrics.attachment_height;
    place(composer_, panel_left + Dpi(12), composer_top, panel_right - panel_left - Dpi(24),
          std::max(Dpi(60), row_top - composer_top - Dpi(16)));
    place(plus_button_, panel_left + Dpi(8), row_top + Dpi(2), Dpi(30), Dpi(30));
    HDC permission_device = GetDC(permission_button_);
    const auto old_permission_font = SelectObject(permission_device, ui_font_);
    const std::wstring permission_text = PermissionLabel(permission_mode_);
    SIZE permission_size{};
    GetTextExtentPoint32W(permission_device, permission_text.c_str(), static_cast<int>(permission_text.size()),
                         &permission_size);
    SelectObject(permission_device, old_permission_font);
    ReleaseDC(permission_button_, permission_device);
    const int permission_width = std::min<int>(std::max<int>(Dpi(120), permission_size.cx + Dpi(46)),
                                              std::max(Dpi(64), composer_width * 40 / 100));
    place(permission_button_, panel_left + Dpi(44), row_top + Dpi(4), permission_width, Dpi(26));
    place(send_button_, panel_right - Dpi(42), row_top, Dpi(32), Dpi(32));
    place(model_label_, panel_right - Dpi(252), row_top + Dpi(7), Dpi(38), Dpi(20));
    const int model_left = std::max(panel_left + Dpi(44) + permission_width + Dpi(8), panel_right - Dpi(258));
    place(model_button_, model_left, row_top + Dpi(3), std::max(1, panel_right - Dpi(46) - model_left), Dpi(26));
    if (!placements.empty()) {
        constexpr UINT flags = SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER;
        HDWP positions = BeginDeferWindowPos(static_cast<int>(placements.size()));
        for (const auto& position : placements) {
            if (positions == nullptr) break;
            positions = DeferWindowPos(positions, position.control, nullptr, position.x, position.y,
                                       position.width, position.height, flags);
        }
        if (positions == nullptr || !EndDeferWindowPos(positions)) {
            for (const auto& position : placements) {
                SetWindowPos(position.control, nullptr, position.x, position.y, position.width, position.height, flags);
            }
        }
    }
    UpdateEmptyState();
    UpdateSidebarScrollbar();
}

AppWindow::CombinedSidebarMetrics AppWindow::MeasureSidebarScrollbar() {
    CombinedSidebarMetrics metrics;
    auto measure_list = [](HWND list, int& count, int& visible, int& top, int& max_top) {
        if (list == nullptr) return;
        RECT client{};
        GetClientRect(list, &client);
        const int height = std::max(0, static_cast<int>(client.bottom - client.top));
        const int item_height = std::max(1, static_cast<int>(SendMessageW(list, LB_GETITEMHEIGHT, 0, 0)));
        count = std::max(0, static_cast<int>(SendMessageW(list, LB_GETCOUNT, 0, 0)));
        visible = height > 0 ? std::max(1, height / item_height) : 0;
        top = std::max(0, static_cast<int>(SendMessageW(list, LB_GETTOPINDEX, 0, 0)));
        max_top = std::max(0, count - visible);
        top = std::min(top, max_top);
    };
    measure_list(projects_list_, metrics.project_count, metrics.project_visible, metrics.project_top,
                 metrics.project_max_top);
    measure_list(history_list_, metrics.history_count, metrics.history_visible, metrics.history_top,
                 metrics.history_max_top);

    metrics.count = metrics.project_count + metrics.history_count;
    metrics.visible = std::max(1, metrics.project_visible + metrics.history_visible);
    metrics.max_top = metrics.project_max_top + metrics.history_max_top;
    metrics.needed = metrics.max_top > 0;
    metrics.top = metrics.history_top > 0 ? metrics.project_max_top + metrics.history_top : metrics.project_top;
    metrics.top = std::clamp(metrics.top, 0, metrics.max_top);

    RECT client{};
    GetClientRect(sidebar_scrollbar_, &client);
    const int height = std::max(1, static_cast<int>(client.bottom - client.top));
    metrics.track_top = ScaleForSystemDpi(4);
    metrics.track_bottom = std::max(metrics.track_top + 1, height - ScaleForSystemDpi(4));
    if (!metrics.needed) return metrics;

    const int track_height = metrics.track_bottom - metrics.track_top;
    const int thumb_height =
        std::max(ScaleForSystemDpi(26), track_height * metrics.visible / std::max(1, metrics.count));
    const int travel = std::max(0, track_height - thumb_height);
    metrics.thumb_top = metrics.track_top + (metrics.max_top == 0 ? 0 : travel * metrics.top / metrics.max_top);
    metrics.thumb_bottom = std::min(metrics.track_bottom, metrics.thumb_top + thumb_height);
    return metrics;
}

void AppWindow::ScrollSidebarBy(int lines) {
    const CombinedSidebarMetrics metrics = MeasureSidebarScrollbar();
    if (!metrics.needed) return;
    ScrollSidebarTo(std::clamp(metrics.top + lines, 0, metrics.max_top));
}

void AppWindow::ScrollSidebarTo(int combined_top) {
    const CombinedSidebarMetrics metrics = MeasureSidebarScrollbar();
    if (!metrics.needed) return;
    const int target = std::clamp(combined_top, 0, metrics.max_top);
    const int project_top = std::min(target, metrics.project_max_top);
    const int history_top = std::min(metrics.history_max_top, std::max(0, target - metrics.project_max_top));
    hovered_history_item_ = -1;
    hovered_project_item_ = -1;
    SendMessageW(projects_list_, LB_SETTOPINDEX, project_top, 0);
    SendMessageW(history_list_, LB_SETTOPINDEX, history_top, 0);
    InvalidateRect(projects_list_, nullptr, FALSE);
    InvalidateRect(history_list_, nullptr, FALSE);
    UpdateSidebarScrollbar();
}

void AppWindow::ScrollSidebarToPoint(int y) {
    const CombinedSidebarMetrics metrics = MeasureSidebarScrollbar();
    if (!metrics.needed) return;
    const int track_height = std::max(1, metrics.track_bottom - metrics.track_top);
    const int thumb_height = std::max(1, metrics.thumb_bottom - metrics.thumb_top);
    const int travel = std::max(1, track_height - thumb_height);
    const int usable = std::max(0, y - metrics.track_top - thumb_height / 2);
    ScrollSidebarTo(std::min(metrics.max_top, usable * metrics.max_top / travel));
}

void AppWindow::UpdateSidebarScrollbar() {
    if (sidebar_scrollbar_ == nullptr) return;
    const CombinedSidebarMetrics metrics = MeasureSidebarScrollbar();
    ShowWindow(sidebar_scrollbar_, metrics.needed ? SW_SHOWNOACTIVATE : SW_HIDE);
    InvalidateRect(sidebar_scrollbar_, nullptr, TRUE);
}

void AppWindow::LoadOrCreateConfiguration() {
    config_path_ = JoinPath(GetExecutableDirectory(), "config.json");
    std::string config_error;
    const auto contents = ReadFileUtf8(config_path_, &config_error);
    if (!contents.has_value()) {
        const std::string example = DefaultConfigExampleJson();
        if (!WriteWindowsTextFileAtomic(config_path_, example, &config_error)) {
            MessageBoxW(window_, (i18n::Text(L"Could not create config.json:\n") + Utf8ToWide(config_error)).c_str(), L"Spark",
                        MB_OK | MB_ICONERROR);
        }
        config_ = ConfigFromJson(ParseConfigJson(example), config_path_);
    } else {
        try {
            config_ = ConfigFromJson(ParseConfigJson(*contents), config_path_);
        } catch (const nlohmann::json::exception& exception) {
            MessageBoxW(window_, (i18n::Text(L"Invalid config.json:\n") + Utf8ToWide(exception.what())).c_str(), L"Spark",
                        MB_OK | MB_ICONERROR);
            config_ = ConfigFromJson(ParseConfigJson(DefaultConfigExampleJson()), config_path_);
        }
    }
    const bool legacy_workspace = IsLegacyDefaultWorkspacePath(config_.workspace.path, config_.config_path);
    if (config_.workspace.path.empty() || legacy_workspace) {
        config_.workspace.path = DefaultSparkWorkspace();
        SaveConfigAtomically(config_);
    }
    ApplyConfiguration(config_);
    const auto errors = ValidateConfig(config_, true);
    if (!errors.empty()) {
        SetStatus(i18n::Text(L"Configuration is incomplete. Check the model and config file in Settings.\n") + Utf8ToWide(errors.front()));
    }
}

void AppWindow::ReloadMcpWhenIdle() {
    if (AnyTurnRunning()) {
        mcp_reload_pending_ = true;
        return;
    }
    std::lock_guard<std::mutex> mcp_lock(mcp_mutex_);
    mcp_discovered_ = false;
    mcp_network_discovered_ = false;
    mcp_network_override_.reset();
    mcp_ = std::make_unique<McpManager>(config_);
    mcp_reload_pending_ = false;
}

McpManager* AppWindow::McpForTurn(const AppConfig& config) {
    // Called under mcp_mutex_; both pools outlive all parent and child turns.
    if (mcp_ == nullptr || !config.permissions.allow_network || mcp_->AllowsNetwork() ||
        !std::any_of(config.mcp_servers.begin(), config.mcp_servers.end(), [](const McpServerConfig& server) {
            return server.enabled && server.transport == "streamable_http";
        })) return mcp_.get();
    if (mcp_network_override_ == nullptr) {
        mcp_network_override_ = std::make_unique<McpManager>(config);
        mcp_network_discovered_ = false;
    }
    return mcp_network_override_.get();
}

void AppWindow::ApplyConfiguration(AppConfig config) {
    const bool initialized = configuration_initialized_;
    const bool theme_changed = !initialized || config_.ui.theme != config.ui.theme;
    const bool language_changed = !initialized || config_.ui.language != config.ui.language;
    const bool mcp_changed = !initialized || config_.permissions.allow_network != config.permissions.allow_network ||
        ConfigToJson(config_)["mcp_servers"] != ConfigToJson(config)["mcp_servers"];
    config_ = std::move(config);
    i18n::SetLocale(config_.ui.language);
    config_.config_path = config_path_;
    config_.workspace.path = AbsolutePath(ResolveConfigPath(config_.workspace.path, config_.config_path),
                                          ParentPath(config_.config_path));
    config_.storage.data_dir = AbsolutePath(ResolveConfigPath(config_.storage.data_dir, config_.config_path),
                                            ParentPath(config_.config_path));
    if (mcp_changed) ReloadMcpWhenIdle();
    if (settings_mode_ && initialized) {
        const bool keep_project_workspace =
            active_session_scope_ == "project" && !active_workspace_path_.empty() &&
            IsDirectory(active_workspace_path_) && FindProjectName(active_workspace_path_, nullptr);
        if (!keep_project_workspace && pending_new_conversation_ && active_session_scope_ == "recent") {
            active_workspace_path_ = config_.workspace.path;
            SetWindowTextW(workspace_label_,
                           (std::wstring(Text(L"Workspace  ")) +
                            Utf8ToWide(active_workspace_path_)).c_str());
        }
        permission_mode_ = config_.permissions.default_mode;
        settings_theme_dirty_ = settings_theme_dirty_ || theme_changed;
        settings_language_dirty_ = settings_language_dirty_ || language_changed;
        SetWindowTextW(permission_button_, PermissionLabel(permission_mode_).c_str());
        InvalidateRect(permission_button_, nullptr, FALSE);
        RefreshModelSelector();
        RefreshContextLabel();
        configuration_initialized_ = true;
        return;
    }
    const bool keep_project_workspace =
        active_session_scope_ == "project" && !active_workspace_path_.empty() && IsDirectory(active_workspace_path_) &&
        FindProjectName(active_workspace_path_, nullptr);
    if (!keep_project_workspace) active_workspace_path_ = config_.workspace.path;
    if (theme_changed) ApplyTheme();
    if (language_changed) ApplyLocalization();
    configuration_initialized_ = true;
    permission_mode_ = config_.permissions.default_mode;
    if (subagents_ == nullptr) {
        auto post_subagent_event = [this](EventKind kind, const SubAgentInfo& info, std::string first = {},
                                          std::string second = {}, bool success = false,
                                          AgentPhase phase = AgentPhase::WaitingModel) {
            auto* event = new UiEvent;
            event->kind = kind;
            event->session_key = NormalizePathForComparison(info.parent_session_key);
            event->subagent_id = info.id;
            event->first = std::move(first);
            event->second = std::move(second);
            event->third = info.name;
            event->success = success;
            event->phase = phase;
            if (kind == EventKind::SubAgentCreated || kind == EventKind::SubAgentFinished) event->child_info = info;
            if (!PostMessageW(window_, kUiEventMessage, 0, reinterpret_cast<LPARAM>(event))) delete event;
        };
        SubAgentCallbacks callbacks;
        callbacks.on_created = [post_subagent_event](const SubAgentInfo& info) {
            post_subagent_event(EventKind::SubAgentCreated, info, info.name, info.task);
        };
        callbacks.on_phase = [post_subagent_event](const SubAgentInfo& info, AgentPhase phase,
                                                   const std::string& detail) {
            post_subagent_event(EventKind::SubAgentPhase, info, detail, {}, false, phase);
        };
        callbacks.on_text_delta = [post_subagent_event](const SubAgentInfo& info, const std::string& delta) {
            post_subagent_event(EventKind::SubAgentText, info, delta);
        };
        callbacks.on_reasoning_delta = [post_subagent_event](const SubAgentInfo& info, const std::string& delta) {
            post_subagent_event(EventKind::SubAgentReasoning, info, delta);
        };
        callbacks.on_tool_started = [this](const SubAgentInfo& info, const ToolCall& call) {
            auto* event = new UiEvent;
            event->kind = EventKind::SubAgentToolStart;
            event->session_key = NormalizePathForComparison(info.parent_session_key);
            event->subagent_id = info.id;
            event->first = call.id;
            event->second = call.name;
            event->third = call.arguments_json;
            if (!PostMessageW(window_, kUiEventMessage, 0, reinterpret_cast<LPARAM>(event))) delete event;
        };
        callbacks.on_tool_finished = [this](const SubAgentInfo& info, const ToolCall& call, const ToolResult& result) {
            auto* event = new UiEvent;
            event->kind = EventKind::SubAgentToolFinish;
            event->session_key = NormalizePathForComparison(info.parent_session_key);
            event->subagent_id = info.id;
            event->first = call.id;
            event->second = call.name;
            event->third = result.output;
            event->success = result.ok;
            if (!PostMessageW(window_, kUiEventMessage, 0, reinterpret_cast<LPARAM>(event))) delete event;
        };
        callbacks.on_finished = [post_subagent_event](const SubAgentInfo& info, bool success,
                                                      const std::string& reason) {
            post_subagent_event(EventKind::SubAgentFinished, info, info.output, reason, success);
        };
        callbacks.on_error = [post_subagent_event](const SubAgentInfo& info, const std::string& error) {
            post_subagent_event(EventKind::SubAgentError, info, error);
        };
        subagents_ = std::make_unique<SubAgentManager>(std::move(callbacks));
    }
    RefreshModelSelector();
    RefreshProjects();
    SetWindowTextW(workspace_label_, (std::wstring(Text(L"Workspace  ")) + Utf8ToWide(active_workspace_path_)).c_str());
    SetWindowTextW(permission_button_, PermissionLabel(permission_mode_).c_str());
    const bool needs_session_open = journal_.Path().empty() || journal_.State().workspace.empty() ||
                                    NormalizePathForComparison(journal_.State().workspace) !=
                                        NormalizePathForComparison(active_workspace_path_);
    if (needs_session_open) {
        const std::string last_session = g_startup_recovery_mode ? std::string{} : ReadActiveSessionPath();
        if (!last_session.empty() && PathExists(last_session)) {
            ResumeSessionPath(last_session);
        } else {
            // Do not create an empty session file just to show the welcome screen. The first
            // successful turn creates the session and its no-project output directory.
            journal_ = SessionJournal{};
            pending_new_conversation_ = true;
            active_session_scope_ = "recent";
            active_project_name_.clear();
            active_workspace_path_ = config_.workspace.path;
            show_active_placeholder_ = false;
            ClearChatView();
            SetWindowTextW(main_title_label_, Text(L"New chat"));
        }
        if (!SessionLoading()) {
            RestoreComposerState();
            WriteActiveSessionPath();
        }
        UpdateEmptyState();
    }
    RefreshContextLabel();
    RefreshHistory();
    UpdateTurnUi();
}

const wchar_t* AppWindow::Text(std::wstring_view message) const {
    return i18n::Text(message);
}

void AppWindow::ApplyTheme() {
    const theme::Palette palette = theme::For(config_.ui.theme);
    kBackground = palette.background;
    kSidebar = palette.sidebar;
    kPanel = palette.panel;
    kInput = palette.input;
    kSelection = palette.selection;
    kBorder = palette.border;
    kText = palette.text;
    kMuted = palette.muted;
    kSubtle = palette.subtle;
    kAccent = palette.accent;
    kAccentPressed = palette.accent_pressed;
    kError = palette.error;
    kWarning = palette.warning;
    kSuccess = palette.success;
    kRailIdle = palette.rail_idle;
    kRailHover = palette.rail_hover;
    kSidebarHover = theme::IsDark(config_.ui.theme) ? palette.selection : RGB(42, 43, 46);
    kSidebarHoverText = RGB(250, 250, 250);
    background_brush_ = CachedBrush(kBackground);
    panel_brush_ = CachedBrush(kSidebar);
    input_brush_ = CachedBrush(kInput);
    if (!settings_mode_) {
        if (chat_view_ != nullptr) chat_view_->SetTheme(theme::IsDark(config_.ui.theme));
        if (side_chat_view_ != nullptr) side_chat_view_->SetTheme(theme::IsDark(config_.ui.theme));
        if (summary_panel_ != nullptr) summary_panel_->SetAppearance(config_.ui.theme);
        if (summary_popup_ != nullptr) summary_popup_->SetAppearance(config_.ui.theme);
        if (window_ != nullptr) RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
    }
}

void AppWindow::ApplyLocalization() {
    i18n::SetLocale(config_.ui.language);
    if (window_ == nullptr || settings_mode_) return;
    SetWindowTextW(new_button_, Text(L"New chat"));
    SetWindowTextW(project_label_, Text(L"Projects"));
    SetWindowTextW(recent_label_, Text(L"Recent"));
    SetWindowTextW(settings_button_, Text(L"Settings"));
    UpdateEmptyState();
    SetWindowTextW(permission_button_, PermissionLabel(permission_mode_).c_str());
    if (chat_view_ != nullptr) chat_view_->RefreshLocalization();
    if (side_chat_view_ != nullptr) side_chat_view_->RefreshLocalization();
    for (const auto& turn : active_turns_) {
        const auto& runtime = turn.second;
        SetPhase(runtime, runtime->phase, runtime->phase_detail);
    }
    RefreshSummaryPanel();
    SendMessageW(composer_, EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(Text(L"Message Spark")));
    const std::string title = journal_.State().title;
    if (title.empty() || title == "新对话" || title == "New chat") {
        SetWindowTextW(main_title_label_, Text(L"New chat"));
    }
    RefreshModelButton();
    RefreshHistory();
    RefreshProjectRows();
    RefreshContextLabel();
    LayoutControls();
    if (!settings_mode_) InvalidateRect(window_, nullptr, FALSE);
}

void AppWindow::RefreshModelSelector() {
    populating_ = true;
    SendMessageW(model_combo_, CB_RESETCONTENT, 0, 0);
    int active_index = -1;
    int index = 0;
    for (const ProviderConfig& provider : config_.providers) {
        for (const ModelConfig& model : provider.models) {
            const std::string label = (provider.label.empty() ? provider.id : provider.label) + " · " +
                                      model.id;
            SendMessageW(model_combo_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Utf8ToWide(label).c_str()));
            if (provider.id == config_.active.provider && model.id == config_.active.model) active_index = index;
            ++index;
        }
    }
    SendMessageW(model_combo_, CB_SETCURSEL, active_index, 0);
    RefreshModelButton();
    populating_ = false;
}

void AppWindow::RefreshModelButton() {
    const ModelConfig* model = ActiveModel(config_);
    if (model_button_ == nullptr) return;
    std::wstring label = model == nullptr ? Text(L"No model configured") : ModelDisplayName(*model);
    const std::string effort = model != nullptr && config_.active.reasoning_effort.empty()
        ? EffectiveDefaultReasoningEffort(*model) : config_.active.reasoning_effort;
    if (!effort.empty()) label += L" · " + ReasoningLabel(effort);
    SetWindowTextW(model_button_, label.c_str());
    InvalidateRect(model_button_, nullptr, TRUE);
}

void AppWindow::ResizeModelDropdown() {
    COMBOBOXINFO info{};
    info.cbSize = sizeof(info);
    if (!GetComboBoxInfo(model_combo_, &info) || info.hwndList == nullptr) return;
    const int count = static_cast<int>(SendMessageW(model_combo_, CB_GETCOUNT, 0, 0));
    const int item_height = static_cast<int>(SendMessageW(model_combo_, CB_GETITEMHEIGHT, 0, 0));
    const int dropdown_height = std::max(item_height + 2, count * item_height + 2);
    RECT combo_rect{};
    RECT work_area{};
    GetWindowRect(model_combo_, &combo_rect);
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0);
    int top = combo_rect.bottom;
    if (top + dropdown_height > work_area.bottom) top = std::max(work_area.top, combo_rect.top - dropdown_height);
    MoveWindow(info.hwndList, combo_rect.left, top, combo_rect.right - combo_rect.left, dropdown_height, TRUE);
}

void AppWindow::RefreshProjects() {
    std::map<std::string, bool> previous_expanded;
    for (size_t index = 0; index < projects_.size() && index < project_expanded_.size(); ++index) {
        previous_expanded[NormalizePathForComparison(projects_[index].workspace)] = project_expanded_[index];
    }
    const std::map<std::string, bool> previous_show_all = project_show_all_;
    projects_.clear();
    const std::string projects_path = JoinPath(config_.storage.data_dir, "projects.json");
    std::string error;
    const auto contents = ReadFileUtf8(projects_path, &error);
    if (contents.has_value()) {
        try {
            const nlohmann::json document = nlohmann::json::parse(*contents);
            organize_mode_ = document.value("organize_mode", organize_mode_);
            chat_sort_ = document.value("chat_sort", chat_sort_);
            if (document.contains("projects") && document["projects"].is_array()) {
                for (const auto& item : document["projects"]) {
                    if (!item.is_object()) continue;
                    ProjectEntry entry;
                    entry.name = item.value("name", std::string{});
                    entry.workspace = item.value("workspace_path", std::string{});
                    entry.last_opened_at = item.value("last_opened_at", uint64_t{0});
                    entry.pinned = item.value("pinned", false);
                    if (item.contains("folders") && item["folders"].is_array()) {
                        for (const auto& folder : item["folders"]) {
                            if (folder.is_string()) entry.folders.push_back(folder.get<std::string>());
                        }
                    }
                    if (entry.folders.empty() && !entry.workspace.empty()) entry.folders.push_back(entry.workspace);
                    if (!entry.workspace.empty()) projects_.push_back(std::move(entry));
                }
            }
        } catch (const nlohmann::json::exception&) {
        }
    }
    const std::string current = NormalizePathForComparison(active_workspace_path_);
    SortProjects();
    project_expanded_.assign(projects_.size(), false);
    std::map<std::string, bool> retained_show_all;
    for (size_t index = 0; index < projects_.size(); ++index) {
        const std::string key = NormalizePathForComparison(projects_[index].workspace);
        const auto previous = previous_expanded.find(key);
        project_expanded_[index] = previous != previous_expanded.end() ? previous->second : key == current;
        const auto show_all = previous_show_all.find(key);
        if (show_all != previous_show_all.end()) retained_show_all[key] = show_all->second;
    }
    project_show_all_ = std::move(retained_show_all);
    if (projects_.empty()) project_section_expanded_ = false;
    RefreshProjectRows();
    SaveProjects();
}

void AppWindow::SaveProjects() {
    nlohmann::json document;
    document["schema_version"] = 1;
    document["projects"] = nlohmann::json::array();
    for (const ProjectEntry& project : projects_) {
        document["projects"].push_back({
            {"name", project.name},
            {"workspace_path", project.workspace},
            {"last_opened_at", project.last_opened_at},
            {"pinned", project.pinned},
            {"folders", project.folders},
        });
    }
    document["organize_mode"] = organize_mode_;
    document["chat_sort"] = chat_sort_;
    const std::string projects_path = JoinPath(config_.storage.data_dir, "projects.json");
    std::error_code error;
    spark::fs::create_directories(Utf8ToWide(config_.storage.data_dir), error);
    std::string write_error;
    if (!WriteFileUtf8Atomic(
            projects_path, document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), &write_error)) {
        SetStatus(i18n::Text(L"Could not save the project list: ") + Utf8ToWide(write_error));
    }
}

void AppWindow::SortProjects() {
    if (chat_sort_ == "manual") {
        std::stable_sort(projects_.begin(), projects_.end(), [](const ProjectEntry& left, const ProjectEntry& right) {
            if (left.pinned != right.pinned) return left.pinned > right.pinned;
            return false;
        });
        return;
    }
    std::sort(projects_.begin(), projects_.end(), [](const ProjectEntry& left, const ProjectEntry& right) {
        if (left.pinned != right.pinned) return left.pinned > right.pinned;
        return left.last_opened_at > right.last_opened_at;
    });
}

void AppWindow::EnsureJournalStorageRoot() {
    // SessionJournal only learns its storage root from Open()/Resume(). On a cold
    // start neither has run yet, so ListRecent() would scan an empty directory and
    // the sidebar would stay blank until a first message created a session.
    if (!journal_.Path().empty()) return;
    if (!config_.storage.data_dir.empty()) journal_.SetStorageRoot(config_.storage.data_dir);
}

void AppWindow::RefreshProjectRows() {
    EnsureJournalStorageRoot();
    project_rows_.clear();
    if (projects_list_ == nullptr) return;
    populating_ = true;
    SendMessageW(projects_list_, LB_RESETCONTENT, 0, 0);
    hovered_project_item_ = -1;

    const std::string active_path = pending_new_conversation_ ? std::string{} : NormalizePathForComparison(journal_.Path());
    std::vector<SessionSummary> sessions;
    const auto recent = journal_.ListRecent(240);
    if (recent.ok) {
        for (const std::string& filename : recent.value) {
            const std::string path = JoinPath(JoinPath(config_.storage.data_dir, "sessions"), filename);
            SessionSummary summary = ReadSessionSummary(path);
            summary.title = Trim(summary.title);
            if (summary.title.empty() || IsPlaceholderSession(summary)) continue;
            sessions.push_back(std::move(summary));
        }
    }
    int selected = -1;
    for (size_t project_index = 0; project_index < projects_.size(); ++project_index) {
        const ProjectEntry& project = projects_[project_index];
        SidebarRow project_row;
        project_row.kind = SidebarRowKind::Project;
        project_row.project = project_index;
        project_row.label = Utf8ToWide(project.name.empty() ? project.workspace : project.name);
        project_rows_.push_back(project_row);
        const int row_index = static_cast<int>(project_rows_.size() - 1);
        SendMessageW(projects_list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(project_rows_.back().label.c_str()));
        if (!pending_new_conversation_ &&
            NormalizePathForComparison(project.workspace) == NormalizePathForComparison(active_workspace_path_) &&
            selected < 0) {
            selected = row_index;
        }

        if (!project_section_expanded_ || organize_mode_ != "project") continue;
        const bool expanded = project_index < project_expanded_.size() && project_expanded_[project_index];
        if (!expanded) continue;

        std::vector<SessionSummary> project_sessions;
        for (const SessionSummary& summary : sessions) {
            if (summary.scope == "recent") continue;
            const bool workspace_match =
                NormalizePathForComparison(summary.workspace) == NormalizePathForComparison(project.workspace);
            bool folder_match = false;
            for (const std::string& folder : project.folders) {
                if (NormalizePathForComparison(summary.workspace) == NormalizePathForComparison(folder)) {
                    folder_match = true;
                    break;
                }
            }
            if (workspace_match || folder_match) project_sessions.push_back(summary);
        }
        if (chat_sort_ == "recent") {
            std::sort(project_sessions.begin(), project_sessions.end(), [](const SessionSummary& left,
                                                                         const SessionSummary& right) {
                if (left.pinned != right.pinned) return left.pinned > right.pinned;
                return left.updated_at > right.updated_at;
            });
        } else if (chat_sort_ == "priority") {
            std::stable_sort(project_sessions.begin(), project_sessions.end(), [](const SessionSummary& left,
                                                                                 const SessionSummary& right) {
                if (left.pinned != right.pinned) return left.pinned > right.pinned;
                return false;
            });
        }
        const std::string project_key = NormalizePathForComparison(project.workspace);
        const bool show_all = project_show_all_.find(project_key) != project_show_all_.end() &&
                              project_show_all_[project_key];
        const size_t visible_sessions =
            show_all ? project_sessions.size() : std::min<size_t>(5, project_sessions.size());
        for (size_t session_index = 0; session_index < visible_sessions; ++session_index) {
            const SessionSummary& summary = project_sessions[session_index];
            SidebarRow chat_row;
            chat_row.kind = SidebarRowKind::Chat;
            chat_row.project = project_index;
            chat_row.session_path = summary.path;
            chat_row.label = summary.title.empty() || summary.title == "新对话" || summary.title == "New chat"
                                 ? Text(L"New chat")
                                 : Utf8ToWide(summary.title);
            project_rows_.push_back(chat_row);
            SendMessageW(projects_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(chat_row.label.c_str()));
            if (!active_path.empty() && NormalizePathForComparison(summary.path) == active_path) {
                selected = static_cast<int>(project_rows_.size() - 1);
            }
        }
        if (!show_all && project_sessions.size() > visible_sessions) {
            SidebarRow more_row;
            more_row.kind = SidebarRowKind::More;
            more_row.project = project_index;
            more_row.label = Text(L"Show more");
            project_rows_.push_back(more_row);
            SendMessageW(projects_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(more_row.label.c_str()));
        }
    }
    if (selected >= 0) SendMessageW(projects_list_, LB_SETCURSEL, selected, 0);
    populating_ = false;
    LayoutControls();
}

void AppWindow::ToggleProjectExpanded(size_t project_index) {
    if (project_index >= project_expanded_.size()) project_expanded_.resize(projects_.size(), false);
    if (project_index >= project_expanded_.size()) return;
    if (project_index < projects_.size()) {
        project_show_all_[NormalizePathForComparison(projects_[project_index].workspace)] = false;
    }
    project_expanded_[project_index] = !project_expanded_[project_index];
    RefreshProjectRows();
}

void AppWindow::ShowSectionMenu(POINT anchor, bool for_projects) {
    HMENU menu = CreatePopupMenu();
    HMENU organize_menu = CreatePopupMenu();
    HMENU sort_menu = CreatePopupMenu();
    menu_item_texts_.clear();
    menu_submenu_items_.clear();

    menu_item_texts_[kOrganizeByProject] = i18n::Text(L"By project");
    menu_item_texts_[kOrganizeByList] = i18n::Text(L"As a list");
    AppendMenuW(organize_menu, MF_STRING | (organize_mode_ == "project" ? MF_CHECKED : 0) | MF_OWNERDRAW,
                kOrganizeByProject, nullptr);
    AppendMenuW(organize_menu, MF_STRING | (organize_mode_ == "list" ? MF_CHECKED : 0) | MF_OWNERDRAW,
                kOrganizeByList, nullptr);

    menu_item_texts_[kSortPriority] = i18n::Text(L"Priority");
    menu_item_texts_[kSortRecent] = i18n::Text(L"Recently updated");
    menu_item_texts_[kSortManual] = i18n::Text(L"Manual order");
    AppendMenuW(sort_menu, MF_STRING | (chat_sort_ == "priority" ? MF_CHECKED : 0) | MF_OWNERDRAW, kSortPriority,
                nullptr);
    AppendMenuW(sort_menu, MF_STRING | (chat_sort_ == "recent" ? MF_CHECKED : 0) | MF_OWNERDRAW, kSortRecent, nullptr);
    AppendMenuW(sort_menu, MF_STRING | (chat_sort_ == "manual" ? MF_CHECKED : 0) | MF_OWNERDRAW, kSortManual, nullptr);

    menu_item_texts_[kOrganizeSidebar] = i18n::Text(L"Organize sidebar");
    menu_item_texts_[kChatSort] = i18n::Text(L"Chat sort order");
    menu_submenu_items_[kOrganizeSidebar] = true;
    menu_submenu_items_[kChatSort] = true;
    const auto append_submenu = [&](UINT id, HMENU submenu) {
        MENUITEMINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask = MIIM_FTYPE | MIIM_ID | MIIM_SUBMENU;
        info.fType = MFT_OWNERDRAW;
        info.wID = id;
        info.hSubMenu = submenu;
        InsertMenuItemW(menu, GetMenuItemCount(menu), TRUE, &info);
    };
    append_submenu(kOrganizeSidebar, organize_menu);
    append_submenu(kChatSort, sort_menu);
    if (!for_projects) {
        menu_item_texts_[kImportCodexRecent] = i18n::Text(L"Import Codex chat branch");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kImportCodexRecent, nullptr);
    }
    ApplyDarkMenuStyle(menu);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x, anchor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    DestroyMenu(organize_menu);
    DestroyMenu(sort_menu);
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    if (command == kImportCodexRecent) {
        ImportCodexSessionInteractive(config_.workspace.path, "recent");
        return;
    }
    if (command == kOrganizeByProject) organize_mode_ = "project";
    else if (command == kOrganizeByList) organize_mode_ = "list";
    else if (command == kSortPriority) chat_sort_ = "priority";
    else if (command == kSortRecent) chat_sort_ = "recent";
    else if (command == kSortManual) chat_sort_ = "manual";
    else return;
    SaveProjects();
    SortProjects();
    RefreshProjectRows();
    RefreshHistory();
    SetStatus(for_projects ? i18n::Text(L"Project sidebar settings updated.") : i18n::Text(L"Chat sort order updated."));
}

void AppWindow::ShowProjectMenu(POINT anchor, size_t project_index) {
    if (project_index >= projects_.size()) return;
    HMENU menu = CreatePopupMenu();
    project_menu_targets_.clear();
    menu_item_texts_.clear();
    const UINT base = 2000 + static_cast<UINT>(project_index) * 10;
    const std::pair<int, const wchar_t*> entries[] = {
        {kProjectPin, projects_[project_index].pinned ? i18n::Text(L"Unpin") : i18n::Text(L"Pin")},
        {kProjectEdit, i18n::Text(L"Edit")},
        {0, nullptr},
        {kProjectOpenFolder, i18n::Text(L"Open in File Explorer")},
        {kProjectImportCodex, i18n::Text(L"Import Codex chat branch")},
        {kProjectArchive, i18n::Text(L"Archive chats")},
        {kProjectRemove, i18n::Text(L"Remove project")},
    };
    for (const auto& entry : entries) {
        if (entry.second == nullptr) {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        const UINT id = base + static_cast<UINT>(entry.first - kProjectPin);
        project_menu_targets_[id] = project_index;
        menu_item_texts_[id] = entry.second;
        AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, id, nullptr);
    }
    ApplyDarkMenuStyle(menu);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x, anchor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    const auto target = project_menu_targets_.find(static_cast<UINT>(command));
    const size_t target_index = target == project_menu_targets_.end() ? static_cast<size_t>(-1) : target->second;
    project_menu_targets_.clear();
    menu_item_texts_.clear();
    if (target_index == static_cast<size_t>(-1)) return;
    size_t target_row = static_cast<size_t>(-1);
    for (size_t index = 0; index < project_rows_.size(); ++index) {
        if (project_rows_[index].kind == SidebarRowKind::Project && project_rows_[index].project == target_index) {
            target_row = index;
            break;
        }
    }
    if (target_row == static_cast<size_t>(-1)) return;
    const int action = kProjectPin + static_cast<int>(static_cast<UINT>(command) - base);
    HandleProjectRowAction(target_row, action);
}

void AppWindow::ShowHistoryMenu(POINT anchor, int history_index) {
    if (history_index < 0 || history_index >= static_cast<int>(history_paths_.size())) return;
    HMENU menu = CreatePopupMenu();
    history_menu_targets_.clear();
    menu_item_texts_.clear();
    menu_item_texts_[kHistoryPinAction] = i18n::Text(L"Pin / unpin");
    menu_item_texts_[kHistoryArchiveAction] = i18n::Text(L"Archive chats");
    menu_item_texts_[kHistoryDeleteAction] = i18n::Text(L"Delete chat");
    AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kHistoryPinAction, nullptr);
    AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kHistoryArchiveAction, nullptr);
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kHistoryDeleteAction, nullptr);
    ApplyDarkMenuStyle(menu);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x, anchor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    menu_item_texts_.clear();
    if (command == kHistoryPinAction) HandleHistoryAction(history_index, kHistoryPin);
    else if (command == kHistoryArchiveAction) HandleHistoryAction(history_index, kHistoryArchive);
    else if (command == kHistoryDeleteAction) HandleHistoryAction(history_index, kHistoryDelete);
}

void AppWindow::HandleProjectRowAction(size_t row_index, int action) {
    if (row_index >= project_rows_.size()) return;
    const SidebarRow& row = project_rows_[row_index];
    if (row.project >= projects_.size()) return;
    ProjectEntry& project = projects_[row.project];
    if (action == kProjectPin) {
        project.pinned = !project.pinned;
        SaveProjects();
        SortProjects();
        RefreshProjectRows();
        SetStatus(project.pinned ? i18n::Text(L"Project pinned.") : i18n::Text(L"Project unpinned."));
        return;
    }
    if (action == kProjectEdit) {
        EditProjectInteractive(row.project);
        return;
    }
    if (action == kProjectOpenFolder) {
        ShellExecuteW(window_, L"open", Utf8ToWide(project.workspace).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (action == kProjectImportCodex) {
        ImportCodexSessionInteractive(project.workspace, "project");
        return;
    }
    if (action == kProjectRemove) {
        if (MessageBoxW(window_, i18n::Text(L"Remove this project from the sidebar? Local folders will be kept."), i18n::Text(L"Remove project"),
                        MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
        projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(row.project));
        SaveProjects();
        RefreshProjectRows();
        SetStatus(i18n::Text(L"Project removed from the sidebar."));
        return;
    }
    if (action == kProjectArchive) {
        if (MessageBoxW(window_, i18n::Text(L"Archive all chats in this project? You can restore them in Settings > Archived chats."), i18n::Text(L"Archive chats"),
                        MB_YESNO | MB_ICONQUESTION) != IDYES) {
            return;
        }
        const std::string archive_directory = JoinPath(JoinPath(config_.storage.data_dir, "sessions"), "archived");
        std::error_code error;
        spark::fs::create_directories(Utf8ToWide(archive_directory), error);
        if (error) {
            SetStatus(i18n::Text(L"Could not create the archive folder: ") + Utf8ToWide(error.message()));
            return;
        }
        const auto recent = journal_.ListRecent(500);
        int archived = 0;
        int failed = 0;
        bool archived_current = false;
        std::string archived_current_path;
        const std::string previous_workspace = journal_.State().workspace;
        if (recent.ok) {
            for (const std::string& filename : recent.value) {
                const std::string source = JoinPath(JoinPath(config_.storage.data_dir, "sessions"), filename);
                const SessionSummary summary = ReadSessionSummary(source);
                bool match = NormalizePathForComparison(summary.workspace) == NormalizePathForComparison(project.workspace);
                for (const std::string& folder : project.folders) {
                    if (NormalizePathForComparison(summary.workspace) == NormalizePathForComparison(folder)) match = true;
                }
                if (!match) continue;
                const std::string destination = JoinPath(archive_directory, filename);
                const bool current = NormalizePathForComparison(source) == NormalizePathForComparison(journal_.Path());
                std::string move_error;
                if (MoveFileWithFallback(source, destination, &move_error)) {
                    ++archived;
                    if (current) {
                        archived_current = true;
                        archived_current_path = source;
                    }
                } else {
                    ++failed;
                }
            }
        }
        if (archived_current) {
            if (!ActivateSessionAfterRemoval(archived_current_path, previous_workspace)) return;
        } else {
            RefreshProjectRows();
            RefreshHistory();
        }
        SetStatus(i18n::Format(failed == 0 ? L"Archived {count} chats."
                                         : L"Archived {count} chats; {failed} failed.",
                              {{L"count", std::to_wstring(archived)}, {L"failed", std::to_wstring(failed)}}));
    }
}

void AppWindow::CreateProjectInteractive() {
    ProjectDialogResult result = ShowProjectDialog(window_, i18n::Text(L"Create project"), L"", {}, false);
    if (!result.accepted || result.folders.empty()) return;
    ProjectEntry entry;
    entry.folders = result.folders;
    entry.workspace = result.folders.front();
    entry.name = WideToUtf8(Trim(result.name));
    if (entry.name.empty()) entry.name = WideToUtf8(WorkspaceName(entry.workspace));
    entry.last_opened_at = UnixTimeSeconds();
    projects_.push_back(std::move(entry));
    project_expanded_.push_back(false);
    project_section_expanded_ = true;
    SaveProjects();
    RefreshProjects();
    SetStatus(i18n::Text(L"Project created."));
}

void AppWindow::EditProjectInteractive(size_t project_index) {
    if (project_index >= projects_.size()) return;
    ProjectEntry& project = projects_[project_index];
    ProjectDialogResult result =
        ShowProjectDialog(window_, i18n::Text(L"Edit project"), Utf8ToWide(project.name), project.folders, true);
    if (result.remove_requested) {
        if (MessageBoxW(window_, i18n::Text(L"Remove this project from the sidebar? Local folders will be kept."), i18n::Text(L"Remove project"),
                        MB_YESNO | MB_ICONWARNING) == IDYES) {
            projects_.erase(projects_.begin() + static_cast<std::ptrdiff_t>(project_index));
            SaveProjects();
            RefreshProjectRows();
        }
        return;
    }
    if (!result.accepted || result.folders.empty()) return;
    project.folders = result.folders;
    project.workspace = result.folders.front();
    project.name = WideToUtf8(Trim(result.name));
    if (project.name.empty()) project.name = WideToUtf8(WorkspaceName(project.workspace));
    SaveProjects();
    RefreshProjectRows();
    SetStatus(i18n::Text(L"Project saved."));
}

void AppWindow::SwitchProject(int index) {
    if (index < 0 || index >= static_cast<int>(projects_.size())) return;
    const std::string workspace = AbsolutePath(projects_[index].workspace, ParentPath(config_.config_path));
    const std::string project_name = projects_[index].name;
    if (!PathExists(workspace) || !IsDirectory(workspace)) {
        SetStatus(i18n::Text(L"Project folder does not exist: ") + Utf8ToWide(workspace));
        return;
    }
    active_workspace_path_ = workspace;
    projects_[index].last_opened_at = UnixTimeSeconds();
    SaveProjects();
    SaveComposerState();
    ClearChatView();
    const auto opened = journal_.Open(config_.storage.data_dir, workspace);
    if (!opened.ok) {
        SetStatus(i18n::Text(L"Could not switch projects: ") + Utf8ToWide(opened.error));
        return;
    }
    SetWindowTextW(workspace_label_, (std::wstring(Text(L"Workspace  ")) + Utf8ToWide(workspace)).c_str());
    show_active_placeholder_ = false;
    const SessionSummary project_summary = ReadSessionSummary(journal_.Path());
    UpdateActiveSessionScope(project_summary.scope, project_summary.workspace);
    ReloadSessionIntoView();
    RestoreComposerState();
    RefreshContextLabel();
    WriteActiveSessionPath();
    RefreshProjects();
    RefreshHistory();
    SetStatus(i18n::Text(L"Switched to project: ") + Utf8ToWide(project_name));
}

void AppWindow::NewConversationInProject(size_t project_index) {
    if (project_index >= projects_.size()) return;
    const std::string workspace = AbsolutePath(projects_[project_index].workspace, ParentPath(config_.config_path));
    if (!PathExists(workspace) || !IsDirectory(workspace)) {
        SetStatus(i18n::Text(L"Project folder does not exist: ") + Utf8ToWide(workspace));
        return;
    }
    SaveComposerState();
    active_workspace_path_ = workspace;
    projects_[project_index].last_opened_at = UnixTimeSeconds();
    SaveProjects();
    SetWindowTextW(workspace_label_, (std::wstring(Text(L"Workspace  ")) + Utf8ToWide(workspace)).c_str());
    const std::string project_name = projects_[project_index].name.empty()
                                         ? WideToUtf8(WorkspaceName(workspace))
                                         : projects_[project_index].name;
    PreparePendingConversation("project", project_name);
    if (project_index >= project_expanded_.size()) project_expanded_.resize(projects_.size(), false);
    project_expanded_[project_index] = true;
    RefreshProjectRows();
    RefreshHistory();
    SetStatus(i18n::Text(L"New chat ready. The first message will save it in this project."));
    SetFocus(composer_);
    UpdateEmptyState();
}

void AppWindow::ApplyModelSelection() {
    const int selected = ComboIndex(model_combo_);
    if (selected < 0) return;
    int index = 0;
    for (const ProviderConfig& provider : config_.providers) {
        for (const ModelConfig& model : provider.models) {
            if (index == selected) {
                ApplyModelChoice(provider.id, model.id);
                return;
            }
            ++index;
        }
    }
}

void AppWindow::RefreshHistory() {
    EnsureJournalStorageRoot();
    populating_ = true;
    SendMessageW(history_list_, LB_RESETCONTENT, 0, 0);
    SendMessageW(history_list_, LB_SETCURSEL, static_cast<WPARAM>(-1), 0);
    history_paths_.clear();
    history_more_index_ = static_cast<size_t>(-1);
    pending_history_action_path_.clear();
    pending_history_action_ = 0;
    pending_history_more_ = false;
    hovered_history_item_ = -1;
    const auto recent = journal_.ListRecent(80);
    if (!recent.ok) {
        populating_ = false;
        UpdateSidebarScrollbar();
        return;
    }
    std::vector<SessionSummary> summaries;
    summaries.reserve(recent.value.size());
    const auto belongs_to_project = [this](const std::string& workspace) {
        if (workspace.empty()) return false;
        const std::string normalized_workspace = NormalizePathForComparison(workspace);
        for (const ProjectEntry& project : projects_) {
            if (NormalizePathForComparison(project.workspace) == normalized_workspace) return true;
            for (const std::string& folder : project.folders) {
                if (NormalizePathForComparison(folder) == normalized_workspace) return true;
            }
        }
        return false;
    };
    for (const std::string& filename : recent.value) {
        const std::string path = JoinPath(JoinPath(config_.storage.data_dir, "sessions"), filename);
        SessionSummary summary = ReadSessionSummary(path);
        summary.title = Trim(summary.title);
        if (summary.scope.empty() && belongs_to_project(summary.workspace)) continue;
        if (summary.scope == "project") continue;
        if (summary.title.empty() || IsPlaceholderSession(summary)) continue;
        summaries.push_back(std::move(summary));
    }
    if (chat_sort_ == "recent") {
        std::sort(summaries.begin(), summaries.end(), [](const SessionSummary& left, const SessionSummary& right) {
            if (left.pinned != right.pinned) return left.pinned > right.pinned;
            return left.updated_at > right.updated_at;
        });
    } else if (chat_sort_ == "priority") {
        std::stable_sort(summaries.begin(), summaries.end(),
                         [](const SessionSummary& left, const SessionSummary& right) {
                             if (left.pinned != right.pinned) return left.pinned > right.pinned;
                             return false;
                         });
    }
    const size_t visible_count = recent_show_all_ ? summaries.size() : std::min<size_t>(5, summaries.size());
    for (size_t index = 0; index < visible_count; ++index) {
        const SessionSummary& summary = summaries[index];
        std::wstring display = summary.title.empty() || summary.title == "新对话" || summary.title == "New chat"
                                   ? Text(L"New chat")
                                   : Utf8ToWide(summary.title);
        SendMessageW(history_list_, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(display.c_str()));
        history_paths_.push_back(summary.path);
    }
    if (!recent_show_all_ && summaries.size() > visible_count) {
        history_more_index_ = history_paths_.size();
        SendMessageW(history_list_, LB_ADDSTRING, 0,
                     reinterpret_cast<LPARAM>(Text(L"Show more")));
    }
    populating_ = false;
    UpdateSidebarScrollbar();
}

void AppWindow::HandleSettingsDataChanged() {
    if (!journal_.Path().empty() && !PathExists(journal_.Path())) {
        ActivateSessionAfterRemoval(journal_.Path(), journal_.State().workspace);
    }
    RefreshProjects();
    RefreshHistory();
}

void AppWindow::SetSettingsMode(bool enabled) {
    if (window_ == nullptr) return;
    settings_mode_ = enabled;
    if (summary_popup_ != nullptr) summary_popup_->Hide();
    std::vector<HWND> controls = {
        sidebar_title_, new_button_, project_label_, projects_list_, project_menu_button_, project_add_button_,
        recent_label_, history_list_, recent_menu_button_, recent_new_button_, sidebar_scrollbar_, settings_button_,
        main_title_label_, context_label_, summary_button_, subagent_button_, model_label_, model_combo_, model_button_,
        permission_button_, plus_button_, side_title_label_, side_close_button_,
        composer_, send_button_, workspace_label_, status_label_, empty_mark_, empty_title_,
    };
    if (chat_view_ != nullptr && chat_view_->hwnd() != nullptr) controls.push_back(chat_view_->hwnd());
    if (side_chat_view_ != nullptr && side_chat_view_->hwnd() != nullptr) controls.push_back(side_chat_view_->hwnd());
    if (summary_panel_ != nullptr) controls.push_back(summary_panel_->hwnd());
    for (HWND control : controls) {
        if (control != nullptr) ShowWindow(control, enabled ? SW_HIDE : SW_SHOW);
    }
    ShowWindow(project_add_button_, enabled ? SW_HIDE : SW_SHOW);
    if (!enabled) {
        ShowWindow(model_label_, SW_HIDE);
        ShowWindow(model_combo_, SW_HIDE);
        ShowWindow(workspace_label_, SW_HIDE);
        ShowWindow(status_label_, SW_HIDE);
        LayoutControls();
        UpdateEmptyState();
        if (chat_view_ != nullptr) SetFocus(composer_);
        RefreshSubAgentButton();
        if (settings_theme_dirty_) {
            settings_theme_dirty_ = false;
            ApplyTheme();
        }
        if (settings_language_dirty_) {
            settings_language_dirty_ = false;
            ApplyLocalization();
            LayoutControls();
        }
        InvalidateRect(window_, nullptr, FALSE);
    } else {
        InvalidateRect(window_, nullptr, FALSE);
    }
}

void AppWindow::BeginNewSession() {
    SaveComposerState();
    active_workspace_path_ = config_.workspace.path;
    PreparePendingConversation("recent", std::string{});
    RefreshHistory();
    RefreshProjectRows();
    SetStatus(i18n::Text(L"New chat ready. The first message will save it."));
    SetFocus(composer_);
    UpdateEmptyState();
}

void AppWindow::PreparePendingConversation(const std::string& scope, const std::string& project_name) {
    CloseSubAgentChat();
    const auto* provider = FindProvider(config_, config_.active.provider);
    const auto* model = provider ? FindModel(*provider, config_.active.model) : nullptr;
    if (model) config_.active.reasoning_effort = EffectiveDefaultReasoningEffort(*model);
    pending_new_conversation_ = true;
    active_session_scope_ = scope == "project" ? "project" : "recent";
    active_project_name_ = active_session_scope_ == "project"
                               ? (project_name.empty() ? WideToUtf8(WorkspaceName(active_workspace_path_)) : project_name)
                               : std::string{};
    show_active_placeholder_ = false;
    goal_.clear();
    plan_mode_ = false;
    attachments_.clear();
    pending_edit_index_ = static_cast<size_t>(-1);
    active_tool_entries_.clear();
    active_assistant_entry_ = static_cast<size_t>(-1);
    assistant_stream_started_ = false;
    ClearChatView();
    ClearComposer();
    RestoreComposerState();
    RefreshModelButton();
    SetWindowTextW(main_title_label_, Text(L"New chat"));
    LayoutControls();
    RefreshContextLabel();
    UpdateEmptyState();
    UpdateTurnUi();
}

bool AppWindow::MaterializePendingConversation() {
    if (!pending_new_conversation_) return true;
    const std::string pending_draft = ComposerStatePath();
    const std::string scope = active_session_scope_ == "project" ? "project" : "recent";
    if (scope == "recent") {
        const std::string selected_workspace = active_workspace_path_.empty() ? config_.workspace.path : active_workspace_path_;
        if (NormalizePathForComparison(selected_workspace) == NormalizePathForComparison(config_.workspace.path)) {
            std::string workspace_error;
            const std::string conversation_workspace =
                PrepareNoProjectConversationWorkspace(&workspace_error);
            if (conversation_workspace.empty()) {
                SetStatus(i18n::Text(L"Could not create the project-free task folder: ") + Utf8ToWide(workspace_error));
                return false;
            }
            active_workspace_path_ = conversation_workspace;
        }
    }
    const auto opened = journal_.Open(config_.storage.data_dir, active_workspace_path_);
    if (!opened.ok) {
        SetStatus(i18n::Text(L"Could not create a session: ") + Utf8ToWide(opened.error));
        return false;
    }
    const auto scoped = journal_.Append("session.scope", {{"scope", scope}});
    if (!scoped.ok) {
        SetStatus(i18n::Text(L"Could not create a chat: ") + Utf8ToWide(scoped.error));
        return false;
    }
    journal_.Append("session.permission", {{"mode", permission_mode_}});
    pending_new_conversation_ = false;
    SaveComposerState();
    if (!pending_draft.empty()) DeleteFileW(Utf8ToWide(pending_draft).c_str());
    show_active_placeholder_ = false;
    WriteActiveSessionPath();
    UpdateEmptyState();
    return true;
}

void AppWindow::ResumeSelectedSession() {
    const int index = static_cast<int>(SendMessageW(history_list_, LB_GETCURSEL, 0, 0));
    if (index < 0 || index >= static_cast<int>(history_paths_.size())) return;
    // Queue the switch so the list box finishes handling its click before the transcript, and the
    // list itself, are rebuilt.
    PostSessionActivation(history_paths_[index]);
}

void AppWindow::RefreshSessionTitle(const std::string& path) {
    const auto set_title = [this](const std::string& title) {
        SetWindowTextW(main_title_label_, title.empty() || title == "新对话" || title == "New chat"
            ? Text(L"New chat") : Utf8ToWide(title).c_str());
    };
    const std::string key = NormalizePathForComparison(path);
    if (!path.empty() && key == NormalizePathForComparison(journal_.Path())) {
        set_title(journal_.State().title);
        return;
    }
    // Sidebar population already read these titles; switching must not reread a long journal.
    const auto cached = SessionSummaryCache().find(path);
    if (cached != SessionSummaryCache().end()) {
        set_title(cached->second.summary.title);
        return;
    }
    for (const auto& row : project_rows_) {
        if (row.kind == SidebarRowKind::Chat && NormalizePathForComparison(row.session_path) == key) {
            SetWindowTextW(main_title_label_, row.label.c_str());
            return;
        }
    }
    for (size_t index = 0; index < history_paths_.size(); ++index) {
        if (NormalizePathForComparison(history_paths_[index]) != key) continue;
        const LRESULT length = SendMessageW(history_list_, LB_GETTEXTLEN, index, 0);
        if (length == LB_ERR) break;
        std::wstring title(static_cast<size_t>(length) + 1, L'\0');
        if (SendMessageW(history_list_, LB_GETTEXT, index, reinterpret_cast<LPARAM>(title.data())) != LB_ERR) {
            SetWindowTextW(main_title_label_, title.c_str());
            return;
        }
        break;
    }
    SetWindowTextW(main_title_label_, L"");
}

void AppWindow::PostSessionActivation(const std::string& path) {
    if (path.empty() || window_ == nullptr) return;
    auto* queued = new std::string(path);
    if (!PostMessageW(window_, kActivateSessionMessage, 0, reinterpret_cast<LPARAM>(queued))) delete queued;
}

void AppWindow::ChooseWorkspace() {
    const std::wstring selected = SelectFolder(window_);
    if (selected.empty()) return;
    SaveComposerState();
    config_.workspace.path = WideToUtf8(selected);
    active_workspace_path_ = config_.workspace.path;
    SaveConfigAtomically(config_);
    const auto opened = journal_.Open(config_.storage.data_dir, active_workspace_path_);
    if (!opened.ok) {
        SetStatus(i18n::Text(L"Could not switch projects: ") + Utf8ToWide(opened.error));
        return;
    }
    SetWindowTextW(workspace_label_, (std::wstring(Text(L"Workspace  ")) + selected).c_str());
    show_active_placeholder_ = false;
    const SessionSummary workspace_summary = ReadSessionSummary(journal_.Path());
    UpdateActiveSessionScope(workspace_summary.scope, workspace_summary.workspace);
    ReloadSessionIntoView();
    RestoreComposerState();
    RefreshContextLabel();
    WriteActiveSessionPath();
    RefreshProjects();
    RefreshHistory();
    SetStatus(i18n::Text(L"Project folder changed."));
}

void AppWindow::EditGoal() {
    bool accepted = false;
    const std::wstring value = PromptText(window_, Text(L"Goal (clear to remove)"),
                                         Utf8ToWide(goal_), &accepted);
    if (accepted) goal_ = TruncateUtf8(WideToUtf8(Trim(value)), 4096);
    SaveComposerState();
    SetFocus(composer_);
}

bool AppWindow::IsImageAttachment(const std::string& path) const {
    const std::string extension =
        LowerAscii(WideToUtf8(spark::fs::path(Utf8ToWide(path)).extension().wstring()));
    return extension == ".bmp" || extension == ".gif" || extension == ".jpeg" || extension == ".jpg" ||
           extension == ".png" || extension == ".webp";
}

HBITMAP AppWindow::LoadAttachmentBitmap(const std::string& path) {
    const auto cached = attachment_bitmaps_.find(path);
    if (cached != attachment_bitmaps_.end()) return cached->second;
    if (g_gdiplus_token == 0 || !IsImageAttachment(path)) return nullptr;
    Gdiplus::Bitmap source(Utf8ToWide(path).c_str(), FALSE);
    if (source.GetLastStatus() != Gdiplus::Ok) return nullptr;
    const int edge = std::max(1, Dpi(160));
    const double scale = std::min(1.0, static_cast<double>(edge) /
                                         std::max<UINT>(1, std::max(source.GetWidth(), source.GetHeight())));
    const int width = std::max(1, static_cast<int>(source.GetWidth() * scale));
    const int height = std::max(1, static_cast<int>(source.GetHeight() * scale));
    Gdiplus::Bitmap thumbnail(width, height, PixelFormat32bppPARGB);
    {
        Gdiplus::Graphics graphics(&thumbnail);
        graphics.Clear(Gdiplus::Color(GetRValue(kInput), GetGValue(kInput), GetBValue(kInput)));
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        if (graphics.DrawImage(&source, 0, 0, width, height) != Gdiplus::Ok) return nullptr;
    }
    HBITMAP bitmap = nullptr;
    if (thumbnail.GetHBITMAP(Gdiplus::Color(48, 49, 52), &bitmap) != Gdiplus::Ok || bitmap == nullptr) return nullptr;
    if (attachment_bitmaps_.size() >= 12) {
        const auto oldest = attachment_bitmaps_.begin();
        if (oldest->second != nullptr) DeleteObject(oldest->second);
        attachment_bitmaps_.erase(oldest);
    }
    attachment_bitmaps_[path] = bitmap;
    return bitmap;
}

void AppWindow::OpenAttachment(const std::string& path) {
    if (path.empty()) return;
    ShellExecuteW(window_, L"open", Utf8ToWide(path).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

bool AppWindow::AttachClipboardImage() {
    if (!IsClipboardFormatAvailable(CF_DIBV5) && !IsClipboardFormatAvailable(CF_DIB)) return false;
    if (!OpenClipboard(window_)) return false;
    const UINT format = IsClipboardFormatAvailable(CF_DIBV5) ? CF_DIBV5 : CF_DIB;
    HANDLE handle = GetClipboardData(format);
    if (handle == nullptr) {
        CloseClipboard();
        return false;
    }
    const size_t size = static_cast<size_t>(GlobalSize(handle));
    void* locked = GlobalLock(handle);
    if (locked == nullptr || size < sizeof(BITMAPINFOHEADER)) {
        if (locked != nullptr) GlobalUnlock(handle);
        CloseClipboard();
        return false;
    }

    const auto* header = static_cast<const BITMAPINFOHEADER*>(locked);
    size_t header_bytes = header->biSize;
    size_t color_bytes = 0;
    if (header_bytes < sizeof(BITMAPINFOHEADER) || header_bytes > size) {
        GlobalUnlock(handle);
        CloseClipboard();
        return false;
    }
    if (header->biBitCount <= 8) {
        const DWORD colors = header->biClrUsed == 0 ? (1u << header->biBitCount) : header->biClrUsed;
        color_bytes = static_cast<size_t>(colors) * sizeof(RGBQUAD);
    } else if (header->biCompression == BI_BITFIELDS && header_bytes == sizeof(BITMAPINFOHEADER)) {
        color_bytes = 3 * sizeof(DWORD);
    }
    if (header_bytes > size - std::min(size, color_bytes)) {
        GlobalUnlock(handle);
        CloseClipboard();
        return false;
    }
    const size_t bits_offset = header_bytes + color_bytes;
    if (bits_offset >= size || size > static_cast<SIZE_T>(std::numeric_limits<DWORD>::max())) {
        GlobalUnlock(handle);
        CloseClipboard();
        return false;
    }

    const std::string directory = JoinPath(JoinPath(config_.storage.data_dir, "drafts"), "clipboard");
    std::error_code directory_error;
    spark::fs::create_directories(Utf8ToWide(directory), directory_error);
    if (directory_error) {
        GlobalUnlock(handle);
        CloseClipboard();
        return false;
    }
    std::ostringstream filename;
    filename << "clipboard-" << spark::MonotonicMillis() << ".bmp";
    const std::string path = JoinPath(directory, filename.str());
    const std::wstring wide_path = Utf8ToWide(path);
    HANDLE file = CreateFileW(wide_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        GlobalUnlock(handle);
        CloseClipboard();
        return false;
    }
    BITMAPFILEHEADER file_header{};
    file_header.bfType = 0x4D42;
    file_header.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + bits_offset);
    file_header.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + size);
    DWORD written = 0;
    const BOOL header_written =
        WriteFile(file, &file_header, sizeof(file_header), &written, nullptr) && written == sizeof(file_header);
    const BOOL dib_written = header_written &&
                             WriteFile(file, locked, static_cast<DWORD>(size), &written, nullptr) &&
                             written == static_cast<DWORD>(size);
    CloseHandle(file);
    GlobalUnlock(handle);
    CloseClipboard();
    if (!dib_written) {
        DeleteFileW(wide_path.c_str());
        return false;
    }

    attachments_.push_back(path);
    LayoutControls();
    InvalidateRect(window_, nullptr, FALSE);
    SetStatus(i18n::Text(L"Image pasted. It will be attached to this message only."));
    UpdateSendButton();
    return true;
}

void AppWindow::DrawAttachmentPreview(HDC device, const LayoutMetrics& metrics) {
    attachment_hit_areas_.clear();
    if (attachments_.empty()) return;
    const int top = metrics.panel_top + Dpi(8);
    const int left = metrics.panel_left + Dpi(12);
    const int max_right = metrics.panel_right - Dpi(12);
    const int image_size = metrics.attachment_size;
    const int gap = Dpi(8);
    int x = left;
    size_t visible = 0;
    for (const std::string& path : attachments_) {
        if (visible >= 5) break;
        const int width = IsImageAttachment(path) ? image_size : std::max(image_size, Dpi(196));
        if (x + width > max_right) break;
        const RECT box{x, top, x + width, top + image_size};
        const int close_size = Dpi(24);
        const RECT close{box.right - close_size - Dpi(3), box.top + Dpi(3),
                         box.right - Dpi(3), box.top + Dpi(3) + close_size};
        attachment_hit_areas_.push_back({box, close, path});

        HBRUSH fill = CreateSolidBrush(kInput);
        HPEN border = CreatePen(PS_SOLID, 1, kBorder);
        HGDIOBJ old_brush = SelectObject(device, fill);
        HGDIOBJ old_pen = SelectObject(device, border);
        RoundRect(device, box.left, box.top, box.right, box.bottom, Dpi(8), Dpi(8));
        SelectObject(device, old_pen);
        SelectObject(device, old_brush);
        DeleteObject(border);
        DeleteObject(fill);

        if (IsImageAttachment(path)) {
            const HBITMAP bitmap = LoadAttachmentBitmap(path);
            if (bitmap != nullptr) {
                BITMAP source{};
                GetObjectW(bitmap, sizeof(source), &source);
                const int source_width = std::max(1, static_cast<int>(source.bmWidth));
                const int source_height = std::max(1, static_cast<int>(source.bmHeight));
                const int target_width =
                    std::max(1, std::min(image_size - Dpi(4), image_size * source_width / source_height));
                const int target_height =
                    std::max(1, std::min(image_size - Dpi(4), image_size * source_height / source_width));
                const RECT target{box.left + (image_size - target_width) / 2, box.top + (image_size - target_height) / 2,
                                  box.left + (image_size - target_width) / 2 + target_width,
                                  box.top + (image_size - target_height) / 2 + target_height};
                HDC source_dc = CreateCompatibleDC(device);
                if (source_dc != nullptr) {
                    HGDIOBJ previous = SelectObject(source_dc, bitmap);
                    SetStretchBltMode(device, HALFTONE);
                    StretchBlt(device, target.left, target.top, target.right - target.left, target.bottom - target.top,
                               source_dc, 0, 0, source_width, source_height, SRCCOPY);
                    SelectObject(source_dc, previous);
                    DeleteDC(source_dc);
                }
            } else {
                SetBkMode(device, TRANSPARENT);
                SetTextColor(device, kMuted);
                RECT text = box;
                DrawTextW(device, i18n::Text(L"Image"), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
            }
        } else {
            std::wstring label = spark::fs::path(Utf8ToWide(path)).filename().wstring();
            if (label.empty()) label = Utf8ToWide(path);
            SetBkMode(device, TRANSPARENT);
            SetTextColor(device, kText);
            RECT text{box.left + Dpi(10), box.top + Dpi(6), close.left - Dpi(6), box.bottom - Dpi(6)};
            DrawTextW(device, label.c_str(), -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                                                         DT_NOPREFIX);
        }
        fill = CreateSolidBrush(kPanel);
        border = CreatePen(PS_SOLID, 1, kBorder);
        old_brush = SelectObject(device, fill);
        old_pen = SelectObject(device, border);
        Ellipse(device, close.left, close.top, close.right, close.bottom);
        SelectObject(device, old_pen);
        SelectObject(device, old_brush);
        DeleteObject(border);
        DeleteObject(fill);
        HPEN cross = CreatePen(PS_SOLID, std::max(1, Dpi(1)), kText);
        old_pen = SelectObject(device, cross);
        const int inset = Dpi(8);
        MoveToEx(device, close.left + inset, close.top + inset, nullptr);
        LineTo(device, close.right - inset, close.bottom - inset);
        MoveToEx(device, close.right - inset, close.top + inset, nullptr);
        LineTo(device, close.left + inset, close.bottom - inset);
        SelectObject(device, old_pen);
        DeleteObject(cross);
        x += width + gap;
        ++visible;
    }
    if (attachments_.size() > visible && visible < 5) {
        RECT more{x, top, x + Dpi(52), top + image_size};
        attachment_hit_areas_.push_back({more, {}, {}});
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, kMuted);
        DrawTextW(device, (L"+" + std::to_wstring(attachments_.size() - visible)).c_str(), -1,
                  &more, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
}

void AppWindow::ShowPermissionMenu(POINT anchor) {
    HMENU menu = CreatePopupMenu();
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    menu_item_texts_[kPermissionAsk] = i18n::Text(L"Ask for approval");
    menu_item_texts_[kPermissionApprove] = i18n::Text(L"Approve for me");
    menu_item_texts_[kPermissionFull] = i18n::Text(L"Full access");
    AppendMenuW(menu, MF_STRING | (permission_mode_ == "ask" ? MF_CHECKED : 0) | MF_OWNERDRAW, kPermissionAsk, nullptr);
    AppendMenuW(menu, MF_STRING | (permission_mode_ == "approve_for_me" ? MF_CHECKED : 0) | MF_OWNERDRAW,
                kPermissionApprove, nullptr);
    AppendMenuW(menu, MF_STRING | (permission_mode_ == "full_access" ? MF_CHECKED : 0) | MF_OWNERDRAW, kPermissionFull,
                nullptr);
    ApplyDarkMenuStyle(menu);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, anchor.x, anchor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    if (command == kPermissionAsk) permission_mode_ = "ask";
    else if (command == kPermissionApprove) permission_mode_ = "approve_for_me";
    else if (command == kPermissionFull) permission_mode_ = "full_access";
    else return;
    if (config_.permissions.default_mode != permission_mode_) {
        ApplyPermissionMode(&config_, permission_mode_);
        ReloadMcpWhenIdle();
    }
    SaveConfigAtomically(config_);
    if (!journal_.Path().empty()) {
        journal_.Append("session.permission", {{"mode", permission_mode_}});
    }
    SetWindowTextW(permission_button_, PermissionLabel(permission_mode_).c_str());
    InvalidateRect(permission_button_, nullptr, TRUE);
    SetStatus(i18n::Text(L"Permissions for the next turn: ") + PermissionLabel(permission_mode_));
}

void AppWindow::ShowModelMenu(POINT anchor) {
    HMENU menu = CreatePopupMenu();
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    model_menu_targets_.clear();
    reasoning_menu_targets_.clear();

    std::vector<ModelPickerRow> picker_rows;
    UINT checked_model = 0;
    UINT model_index = 0;
    for (size_t provider_index = 0; provider_index < config_.providers.size(); ++provider_index) {
        const ProviderConfig& provider = config_.providers[provider_index];
        if (provider.models.empty()) continue;
        const UINT header_id = static_cast<UINT>(kModelHeaderBase + provider_index);
        menu_item_texts_[header_id] = ProviderDisplayName(provider);
        picker_rows.push_back({header_id, menu_item_texts_[header_id], ModelPickerKind::Header});
        AppendMenuW(menu, MF_STRING | MF_DISABLED | MF_OWNERDRAW, header_id, nullptr);
        for (const ModelConfig& model : provider.models) {
            const UINT item_id = static_cast<UINT>(kModelItemBase + model_index);
            model_menu_targets_[item_id] = ModelMenuTarget{provider.id, model.id};
            menu_item_texts_[item_id] = ModelDisplayName(model);
            const bool selected = provider.id == config_.active.provider && model.id == config_.active.model;
            picker_rows.push_back({item_id, menu_item_texts_[item_id], ModelPickerKind::Model});
            if (selected) checked_model = item_id;
            AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0) | MF_OWNERDRAW, item_id, nullptr);
            ++model_index;
        }
    }

    const ModelConfig* active_model = ActiveModel(config_);
    if (active_model != nullptr && !EffectiveReasoningEfforts(*active_model).empty()) {
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        const std::string effort = config_.active.reasoning_effort.empty()
            ? EffectiveDefaultReasoningEffort(*active_model) : config_.active.reasoning_effort;
        menu_item_texts_[kModelReasoningEntry] = i18n::Text(L"Reasoning effort · ") + ReasoningLabel(effort);
        picker_rows.push_back({kModelReasoningEntry, menu_item_texts_[kModelReasoningEntry], ModelPickerKind::Reasoning});
        AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kModelReasoningEntry, nullptr);
    }

    ApplyDarkMenuStyle(menu);
    ModelPicker picker;
    const int command = picker_rows.size() > 12
        ? static_cast<int>(picker.Track(window_, ui_font_, anchor, config_.ui.theme, std::move(picker_rows), checked_model))
        : TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, anchor.x, anchor.y, 0, window_, nullptr);
    DestroyMenu(menu);

    const auto target = model_menu_targets_.find(static_cast<UINT>(command));
    ModelMenuTarget selected_target;
    const bool has_model_target = target != model_menu_targets_.end();
    if (has_model_target) selected_target = target->second;
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    model_menu_targets_.clear();
    reasoning_menu_targets_.clear();

    if (has_model_target) {
        ApplyModelChoice(selected_target.provider, selected_target.model);
        const ProviderConfig* provider = FindProvider(config_, selected_target.provider);
        const ModelConfig* model = provider == nullptr ? nullptr : FindModel(*provider, selected_target.model);
        if (model != nullptr && !EffectiveReasoningEfforts(*model).empty()) {
            ShowReasoningMenu(anchor, selected_target.provider, selected_target.model);
        }
    } else if (command == kModelReasoningEntry && active_model != nullptr) {
        ShowReasoningMenu(anchor, config_.active.provider, config_.active.model);
    }
}

void AppWindow::ShowReasoningMenu(POINT anchor, const std::string& provider_id, const std::string& model_id) {
    const ProviderConfig* provider = FindProvider(config_, provider_id);
    const ModelConfig* model = provider == nullptr ? nullptr : FindModel(*provider, model_id);
    if (model == nullptr) return;
    const std::vector<std::string> efforts = EffectiveReasoningEfforts(*model);
    if (efforts.empty()) return;

    HMENU menu = CreatePopupMenu();
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    reasoning_menu_targets_.clear();
    const std::string active_effort = config_.active.reasoning_effort.empty()
        ? EffectiveDefaultReasoningEffort(*model) : config_.active.reasoning_effort;
    for (size_t index = 0; index < efforts.size(); ++index) {
        const UINT item_id = static_cast<UINT>(kReasoningItemBase + index);
        reasoning_menu_targets_[item_id] = efforts[index];
        menu_item_texts_[item_id] = ReasoningLabel(efforts[index]);
        const bool selected = provider_id == config_.active.provider && model_id == config_.active.model &&
                              efforts[index] == active_effort;
        AppendMenuW(menu, MF_STRING | (selected ? MF_CHECKED : 0) | MF_OWNERDRAW, item_id, nullptr);
    }
    ApplyDarkMenuStyle(menu);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_BOTTOMALIGN, anchor.x, anchor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    const auto selected = reasoning_menu_targets_.find(static_cast<UINT>(command));
    const bool has_selection = selected != reasoning_menu_targets_.end();
    const std::string effort = has_selection ? selected->second : std::string{};
    menu_item_texts_.clear();
    menu_submenu_items_.clear();
    reasoning_menu_targets_.clear();
    if (!has_selection) return;

    config_.active.provider = provider_id;
    config_.active.model = model_id;
    config_.active.reasoning_effort = effort;
    SaveConfigAtomically(config_);
    RefreshModelButton();
    SetStatus(i18n::Text(L"Reasoning effort changed to: ") + ReasoningLabel(effort));
}

void AppWindow::ApplyModelChoice(const std::string& provider_id, const std::string& model_id) {
    const ProviderConfig* provider = FindProvider(config_, provider_id);
    const ModelConfig* model = provider == nullptr ? nullptr : FindModel(*provider, model_id);
    if (provider == nullptr || model == nullptr) return;
    const bool model_changed = config_.active.provider != provider_id || config_.active.model != model_id;
    config_.active.provider = provider_id;
    config_.active.model = model_id;
    const std::vector<std::string> efforts = EffectiveReasoningEfforts(*model);
    if (efforts.empty()) {
        config_.active.reasoning_effort.clear();
    } else if (model_changed ||
               std::find(efforts.begin(), efforts.end(), config_.active.reasoning_effort) == efforts.end()) {
        config_.active.reasoning_effort = EffectiveDefaultReasoningEffort(*model);
    }
    SaveConfigAtomically(config_);
    RefreshModelButton();
    RefreshContextLabel();
    SetStatus(i18n::Text(L"Model changed to: ") + ProviderDisplayName(*provider) + L" / " + ModelDisplayName(*model));
}

void AppWindow::HandleHistoryAction(int index, int action) {
    if (index < 0 || index >= static_cast<int>(history_paths_.size())) return;
    HandleSessionAction(history_paths_[index], action);
}

void AppWindow::HandleSessionAction(const std::string& path, int action) {
    if (path.empty()) return;
    if (action == kHistoryDelete) {
        const std::string source = AbsolutePath(path, config_.storage.data_dir);
        const bool current = NormalizePathForComparison(source) == NormalizePathForComparison(journal_.Path());
        const auto running = FindTurn(source);
        if (running != nullptr) {
            running->pending_delete_path = source;
            running->pending_archive_path.clear();
            running->cancellation.Cancel();
            SetStatus(i18n::Text(L"Stopping this turn. The chat will then be deleted."));
            return;
        }
        if (MessageBoxW(window_, i18n::Text(L"Permanently delete this chat? This cannot be undone."), i18n::Text(L"Delete chat"),
                        MB_YESNO | MB_ICONWARNING) != IDYES) {
            return;
        }
        const std::string previous_workspace = current ? journal_.State().workspace : std::string{};
        std::string delete_error;
        if (!DeleteFileWithRetry(source, &delete_error)) {
            SetStatus(i18n::Text(L"Delete failed: ") + Utf8ToWide(delete_error));
            RefreshHistory();
            RefreshProjectRows();
            return;
        }
        if (current) {
            if (!ActivateSessionAfterRemoval(source, previous_workspace)) return;
        } else {
            RefreshHistory();
            RefreshProjectRows();
        }
        SetStatus(i18n::Text(L"Chat deleted."));
        return;
    }
    if (action == kHistoryPin) {
        const SessionSummary summary = ReadSessionSummary(path);
        const nlohmann::json event = {
            {"timestamp", UnixTimeSeconds()},
            {"type", "session.pinned"},
            {"payload", {{"pinned", !summary.pinned}}},
        };
        if (AppendUtf8Line(path, event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
            SetStatus(summary.pinned ? i18n::Text(L"Chat unpinned.") : i18n::Text(L"Chat pinned."));
            RefreshHistory();
            RefreshProjectRows();
        }
        return;
    }
    if (action == kHistoryArchive) {
        const std::string source = AbsolutePath(path, config_.storage.data_dir);
        const auto running = FindTurn(source);
        if (running != nullptr) {
            running->pending_archive_path = source;
            running->pending_delete_path.clear();
            running->cancellation.Cancel();
            SetStatus(i18n::Text(L"Stopping this turn. The chat will then be archived."));
            return;
        }
        ArchiveSessionPath(source);
    }
}

bool AppWindow::ActivateSessionAfterRemoval(const std::string& removed_session_path,
                                            const std::string& preferred_workspace) {
    const auto recent = journal_.ListRecent(500);
    std::string replacement;
    std::string fallback;
    if (recent.ok) {
        for (const std::string& filename : recent.value) {
            const std::string candidate = JoinPath(JoinPath(config_.storage.data_dir, "sessions"), filename);
            if (NormalizePathForComparison(candidate) == NormalizePathForComparison(removed_session_path)) continue;
            const SessionSummary summary = ReadSessionSummary(candidate);
            if (IsPlaceholderSession(summary)) continue;
            if (fallback.empty()) fallback = candidate;
            if (!preferred_workspace.empty()) {
                if (NormalizePathForComparison(summary.workspace) !=
                    NormalizePathForComparison(preferred_workspace)) {
                    continue;
                }
            }
            replacement = candidate;
            break;
        }
    }
    if (replacement.empty()) replacement = fallback;

    SaveComposerState();
    bool restored = false;
    if (!replacement.empty()) restored = journal_.Resume(replacement).ok;
    if (!restored) {
        active_workspace_path_ = preferred_workspace.empty() ? config_.workspace.path : preferred_workspace;
        const auto opened = journal_.Open(config_.storage.data_dir, active_workspace_path_);
        if (!opened.ok) {
            SetStatus(i18n::Text(L"Could not switch chats: ") + Utf8ToWide(opened.error));
            return false;
        }
    }
    show_active_placeholder_ = false;
    const SessionSummary replacement_summary = ReadSessionSummary(journal_.Path());
    active_workspace_path_ = replacement_summary.workspace.empty() ? config_.workspace.path
                                                                   : replacement_summary.workspace;
    UpdateActiveSessionScope(replacement_summary.scope, replacement_summary.workspace);
    ReloadSessionIntoView();
    RestoreComposerState();
    RefreshContextLabel();
    WriteActiveSessionPath();
    RefreshHistory();
    RefreshProjectRows();
    UpdateTurnUi();
    return true;
}

void AppWindow::ArchiveSessionPath(const std::string& path) {
    if (path.empty()) return;
    const std::string source = AbsolutePath(path, config_.storage.data_dir);
    if (!PathExists(source)) {
        SetStatus(i18n::Text(L"Archive failed: the chat file was already deleted or archived."));
        RefreshHistory();
        RefreshProjectRows();
        return;
    }
    const std::string directory = JoinPath(JoinPath(config_.storage.data_dir, "sessions"), "archived");
    std::error_code error;
    spark::fs::create_directories(Utf8ToWide(directory), error);
    if (error) {
        SetStatus(i18n::Text(L"Could not create the archive folder: ") + Utf8ToWide(error.message()));
        return;
    }
    const spark::fs::path source_path(Utf8ToWide(source));
    const std::wstring filename = source_path.filename().wstring();
    const std::string destination =
        JoinPath(directory, filename.empty() ? std::string("session.jsonl") : WideToUtf8(filename));
    const bool current = NormalizePathForComparison(source) == NormalizePathForComparison(journal_.Path());
    const std::string previous_workspace = current ? journal_.State().workspace : std::string{};
    std::string move_error;
    if (!MoveFileWithFallback(source, destination, &move_error)) {
        SetStatus(i18n::Text(L"Archive failed: ") + Utf8ToWide(move_error));
        RefreshHistory();
        RefreshProjectRows();
        return;
    }
    if (current) {
        if (!ActivateSessionAfterRemoval(source, previous_workspace)) return;
    } else {
        RefreshHistory();
        RefreshProjectRows();
    }
    SetStatus(i18n::Text(L"Chat archived."));
}

void AppWindow::ShowChatContextMenu(POINT anchor, const std::string& session_path) {
    if (session_path.empty()) return;
    HMENU menu = CreatePopupMenu();
    menu_item_texts_.clear();
    menu_item_texts_[kHistoryRenameAction] = i18n::Text(L"Rename");
    menu_item_texts_[kHistoryExportCodexAction] = i18n::Text(L"Export as Codex chat branch");
    AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kHistoryRenameAction, nullptr);
    AppendMenuW(menu, MF_STRING | MF_OWNERDRAW, kHistoryExportCodexAction, nullptr);
    ApplyDarkMenuStyle(menu);
    const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, anchor.x, anchor.y, 0,
                                       window_, nullptr);
    DestroyMenu(menu);
    menu_item_texts_.clear();
    if (command == kHistoryRenameAction) RenameSession(session_path);
    else if (command == kHistoryExportCodexAction) ExportSessionToCodex(session_path);
}

void AppWindow::ExportSessionToCodex(const std::string& session_path) {
    if (session_path.empty()) return;
    const std::string source = AbsolutePath(session_path, config_.storage.data_dir);
    if (!PathExists(source)) {
        SetStatus(i18n::Text(L"Export failed: the session file does not exist."));
        return;
    }

    const std::string root = DefaultCodexSessionsRoot();
    const std::string suggested = SuggestedCodexRolloutPath(root);
    std::error_code directory_error;
    spark::fs::create_directories(Utf8ToWide(ParentPath(suggested)), directory_error);

    std::vector<wchar_t> buffer(32768, L'\0');
    const std::wstring suggested_wide = Utf8ToWide(suggested);
    wcsncpy_s(buffer.data(), buffer.size(), suggested_wide.c_str(), _TRUNCATE);
    const std::wstring initial_directory = Utf8ToWide(root);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    std::wstring filter = Text(L"Codex chat branch (*.jsonl)");
    filter.append(L"\0*.jsonl\0", 9);
    filter += Text(L"All files");
    filter.append(L"\0*.*\0", 5);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = initial_directory.c_str();
    dialog.lpstrDefExt = L"jsonl";
    dialog.Flags = OFN_EXPLORER | OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (!GetSaveFileNameW(&dialog)) return;

    const auto exported = ExportSparkSessionToCodex(source, WideToUtf8(buffer.data()), config_.active.provider);
    if (!exported.ok) {
        SetStatus(i18n::Text(L"Export failed: ") + Utf8ToWide(exported.error));
        return;
    }
    SetStatus(i18n::Text(L"Codex chat branch exported: ") + std::wstring(buffer.data()));
    MessageBoxW(window_, (i18n::Text(L"Exported as a Codex chat branch:\n\n") + std::wstring(buffer.data())).c_str(),
                L"Spark", MB_OK | MB_ICONINFORMATION);
}

void AppWindow::ImportCodexSessionInteractive(const std::string& workspace, const std::string& scope) {
    std::vector<wchar_t> buffer(32768, L'\0');
    const std::wstring initial_directory = Utf8ToWide(DefaultCodexSessionsRoot());
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = window_;
    std::wstring filter = Text(L"Codex chat branch (*.jsonl)");
    filter.append(L"\0*.jsonl\0", 9);
    filter += Text(L"All files");
    filter.append(L"\0*.*\0", 5);
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.lpstrInitialDir = initial_directory.c_str();
    dialog.lpstrDefExt = L"jsonl";
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameW(&dialog)) return;

    const auto imported = ImportCodexSessionToSpark(WideToUtf8(buffer.data()), config_.storage.data_dir, workspace,
                                                    scope);
    if (!imported.ok) {
        SetStatus(i18n::Text(L"Import failed: ") + Utf8ToWide(imported.error));
        MessageBoxW(window_, (i18n::Text(L"Could not import the Codex chat branch:\n\n") + Utf8ToWide(imported.error)).c_str(),
                    L"Spark", MB_OK | MB_ICONERROR);
        return;
    }
    RefreshHistory();
    RefreshProjectRows();
    ResumeSessionPath(imported.value.path);
    SetStatus(i18n::Text(L"Codex chat branch imported: ") + Utf8ToWide(imported.value.title));
}

void AppWindow::RenameSession(const std::string& session_path) {
    if (session_path.empty()) return;
    const SessionSummary summary = ReadSessionSummary(session_path);
    const std::wstring initial = summary.title.empty() || summary.title == "新对话" || summary.title == "New chat"
                                     ? Text(L"New chat") : Utf8ToWide(summary.title);
    const std::wstring renamed = Trim(PromptText(window_, i18n::Text(L"Rename chat"), initial));
    if (renamed.empty()) return;
    std::string title = WideToUtf8(renamed);
    if (title.size() > 120) title.resize(120);
    const nlohmann::json event = {
        {"timestamp", UnixTimeSeconds()},
        {"type", "session.title"},
        {"payload", {{"title", title}}},
    };
    if (!AppendUtf8Line(session_path, event.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace))) {
        SetStatus(i18n::Text(L"Rename failed: could not write the session file."));
        return;
    }
    if (NormalizePathForComparison(session_path) == NormalizePathForComparison(journal_.Path())) {
        journal_.MutableState().title = title;
        SetWindowTextW(main_title_label_, Utf8ToWide(title).c_str());
    }
    RefreshHistory();
    RefreshProjectRows();
    SetStatus(i18n::Text(L"Chat renamed."));
}

void AppWindow::StartTurn() {
    if (exiting_ || SessionLoading()) return;
    const bool resuming = resume_turn_requested_;
    resume_turn_requested_ = false;
    if (active_turns_.size() >= kMaxConcurrentTurns) {
        SetStatus(i18n::Text(L"The parallel chat limit has been reached. Wait for a chat to finish."));
        return;
    }
    if (resuming && !HasPausedTurn()) {
        UpdateTurnUi();
        return;
    }
    const uint64_t resume_elapsed_ms = resuming ? PausedTurnElapsedMs() : 0;
    const std::string text = resuming ? std::string{} : Trim(ComposerText());
    if (!resuming && text.empty()) {
        SetStatus(i18n::Text(L"Enter a message before sending."));
        SetFocus(composer_);
        UpdateSendButton();
        return;
    }
    const auto errors = ValidateConfig(config_, true);
    if (!errors.empty()) {
        std::wstring message = i18n::Text(L"Cannot send: the configuration is incomplete.\n\n");
        const size_t shown = std::min<size_t>(errors.size(), 5);
        for (size_t index = 0; index < shown; ++index) message += L"- " + Utf8ToWide(errors[index]) + L"\n";
        if (errors.size() > shown) message += i18n::Text(L"- Check Settings for the remaining errors.\n");
        SetStatus(i18n::Text(L"Configuration is incomplete. Settings opened."));
        MessageBoxW(window_, message.c_str(), i18n::Text(L"Cannot send message"), MB_OK | MB_ICONWARNING);
        OpenSettingsPage();
        return;
    }

    if (!MaterializePendingConversation()) return;
    if (IsSessionRunning(journal_.Path())) {
        SetStatus(i18n::Text(L"This chat is still running. Stop the current turn first."));
        return;
    }
    if (!resuming && HasPausedTurn()) {
        // Sending new text abandons the paused turn. Remove both the pause
        // metadata and its trailing context marker so the model does not keep
        // waiting for the old turn to be resumed.
        DiscardPausedTurnForNewMessage(journal_);
    }

    const std::string local_command = resuming ? std::string{} : LowerAscii(text);
    const bool is_local_command = local_command == "/compact" || local_command == "/context" ||
                                  local_command == "/上下文压缩" || local_command == "/上下文";

    if (!resuming && pending_edit_index_ != static_cast<size_t>(-1) && chat_view_ != nullptr) {
        SessionState& session = journal_.MutableState();
        std::vector<ChatMessage>& messages = session.messages;
        for (size_t index = messages.size(); index > 0; --index) {
            if (messages[index - 1].role == MessageRole::User) {
                messages.resize(index - 1);
                break;
            }
        }
        std::vector<ChatMessage>& context = session.context_messages;
        for (size_t index = context.size(); index > 0; --index) {
            if (context[index - 1].role == MessageRole::User) {
                context.resize(index - 1);
                break;
            }
        }
        // The edited turn is removed from both transcripts, so its persisted timer
        // and any interrupt marker must be removed with it. Leaving either behind makes
        // the replaced turn replay stale state after switching conversations.
        if (session.metadata.contains("turn_durations_ms") &&
            session.metadata["turn_durations_ms"].is_array() &&
            !session.metadata["turn_durations_ms"].empty()) {
            session.metadata["turn_durations_ms"].erase(session.metadata["turn_durations_ms"].end() - 1);
        }
        if (session.metadata.contains("turn_usages") && session.metadata["turn_usages"].is_array() &&
            !session.metadata["turn_usages"].empty()) {
            session.metadata["turn_usages"].erase(session.metadata["turn_usages"].end() - 1);
        }
        if (session.metadata.contains("turn_errors") && session.metadata["turn_errors"].is_array()) {
            const size_t remaining_turns = static_cast<size_t>(
                std::count_if(messages.begin(), messages.end(), [](const ChatMessage& message) {
                    return message.role == MessageRole::User;
                }));
            auto& errors = session.metadata["turn_errors"];
            errors.erase(std::remove_if(errors.begin(), errors.end(), [remaining_turns](const nlohmann::json& error) {
                             return error.is_object() &&
                                    error.value("turn_index", int64_t{-1}) >= static_cast<int64_t>(remaining_turns);
                         }),
                         errors.end());
        }
        ClearPausedTurn(journal_);
        journal_.SaveSnapshot();
        chat_view_->EraseFrom(pending_edit_index_);
        pending_edit_index_ = static_cast<size_t>(-1);
        active_tool_entries_.clear();
        active_assistant_entry_ = static_cast<size_t>(-1);
        assistant_stream_started_ = false;
    }

    std::string turn_text;
    if (resuming) {
        turn_text.clear();
    } else if (is_local_command) {
        turn_text = text;
    } else {
        turn_text += SnapshotContext();
        if (!attachments_.empty()) {
            turn_text += "[附件引用]\n";
            for (const std::string& path : attachments_) turn_text += "- " + path + "\n";
            turn_text += "\n";
        }
        turn_text += text;
    }
    const std::vector<std::string> turn_image_paths =
        resuming ? std::vector<std::string>{}
                 : [&]() {
                       std::vector<std::string> images;
                       for (const std::string& path : attachments_) {
                           if (IsImageAttachment(path)) images.push_back(path);
                       }
                       return images;
                   }();

    if (!resuming && !is_local_command &&
        (journal_.State().messages.empty() || journal_.State().title == "新对话")) {
        const std::string title = SessionTitleFromMessage(text);
        journal_.MutableState().title = title;
        journal_.Append("session.title", {{"title", title}});
        SetWindowTextW(main_title_label_, Utf8ToWide(title).c_str());
    }

    auto runtime = std::make_shared<TurnRuntime>();
    runtime->path = journal_.Path();
    runtime->session_key = NormalizePathForComparison(runtime->path);
    runtime->workspace = active_workspace_path_.empty() ? config_.workspace.path : active_workspace_path_;
    runtime->config = config_;
    runtime->config.workspace.path = runtime->workspace;
    runtime->plan_mode = plan_mode_;
    runtime->goal = goal_;
    runtime->permission_mode = permission_mode_;
    if (runtime->permission_mode != runtime->config.permissions.default_mode) {
        ApplyPermissionMode(&runtime->config, runtime->permission_mode);
    }
    const auto resumed = runtime->journal.Resume(runtime->path);
    if (!resumed.ok) {
        SetStatus(i18n::Text(L"Could not create a session copy for the parallel turn: ") + Utf8ToWide(resumed.error));
        return;
    }
    runtime->journal.Append("session.permission", {{"mode", permission_mode_}});
    const uint64_t now_tick = spark::MonotonicMillis();
    runtime->started_tick = resuming && resume_elapsed_ms <= now_tick ? now_tick - resume_elapsed_ms : now_tick;
    runtime->resume_turn = resuming;
    runtime->timed_turn = resuming || !is_local_command;
    runtime->last_progress_tick.store(runtime->started_tick);

    if (!resuming) {
        ClearComposer();
        ClearComposerState();
        // Attachments are draft-scoped. They are sent once with this turn and
        // must not silently reappear in the next turn.
        attachments_.clear();
        LayoutControls();
        InvalidateRect(window_, nullptr, FALSE);
    }
    if (!resuming && !is_local_command) {
        ShowWindow(empty_mark_, SW_HIDE);
        ShowWindow(empty_title_, SW_HIDE);
        if (chat_view_ != nullptr && chat_view_->hwnd() != nullptr) {
            ShowWindow(chat_view_->hwnd(), SW_SHOW);
        }
        chat_view_->AddUser(text);
        active_turn_entry_ = chat_view_->AddTurnStatus();
    } else {
        active_turn_entry_ = static_cast<size_t>(-1);
    }
    assistant_stream_started_ = false;
    reasoning_preview_.clear();
    active_reasoning_entry_ = static_cast<size_t>(-1);
    latest_reasoning_entry_ = static_cast<size_t>(-1);
    turn_started_tick_ = runtime->started_tick;
    runtime->ui_attached = true;
    attached_turn_key_ = runtime->session_key;
    active_turns_[runtime->session_key] = runtime;
    if (resuming) {
        const auto reloaded = runtime->journal.Resume(runtime->path);
        if (!reloaded.ok) {
            active_turns_.erase(runtime->session_key);
            runtime->ui_attached = false;
            attached_turn_key_.clear();
            SetStatus(i18n::Text(L"Could not resume the interrupted turn: ") + Utf8ToWide(reloaded.error));
            UpdateTurnUi();
            return;
        }
        ReloadSessionIntoView();
        // ReloadSessionIntoView starts by detaching the old chat view, so reattach the
        // resumed runtime after the transcript has been rebuilt.
        runtime->ui_attached = true;
        attached_turn_key_ = runtime->session_key;
        turn_started_tick_ = runtime->started_tick;
    }
    SetStatus({});
    UpdateEmptyState();
    UpdateTurnUi();
    SetTimer(window_, kTurnWatchdogTimer, 1000, nullptr);
    runtime->worker = std::thread([this, runtime, turn_text, turn_image_paths] {
        const auto post_event = [this, runtime](EventKind kind, std::string first = {}, std::string second = {},
                                       std::string third = {}, bool success = false,
                                       AgentPhase phase = AgentPhase::WaitingModel) {
            runtime->last_progress_tick.store(spark::MonotonicMillis());
            auto* event = new UiEvent;
            event->kind = kind;
            event->session_key = runtime->session_key;
            event->first = std::move(first);
            event->second = std::move(second);
            event->third = std::move(third);
            event->success = success;
            event->phase = phase;
            if (!PostMessageW(window_, kUiEventMessage, 0, reinterpret_cast<LPARAM>(event))) delete event;
        };
        std::string pending_text;
        std::string pending_reasoning;
        ULONGLONG last_text_post = spark::MonotonicMillis();
        ULONGLONG last_reasoning_post = spark::MonotonicMillis();
        const auto flush_text = [&]() {
            if (pending_text.empty()) return;
            std::string delta;
            delta.swap(pending_text);
            post_event(EventKind::TextDelta, std::move(delta));
            last_text_post = spark::MonotonicMillis();
        };
        const auto flush_reasoning = [&]() {
            if (pending_reasoning.empty()) return;
            std::string delta;
            delta.swap(pending_reasoning);
            post_event(EventKind::ReasoningDelta, std::move(delta));
            last_reasoning_post = spark::MonotonicMillis();
        };
        try {
            McpManager* turn_mcp = nullptr;
            {
                std::lock_guard<std::mutex> mcp_lock(mcp_mutex_);
                turn_mcp = McpForTurn(runtime->config);
                const bool use_override = turn_mcp != nullptr && turn_mcp == mcp_network_override_.get();
                const bool discovered_already = use_override ? mcp_network_discovered_ : mcp_discovered_.load();
                if (!discovered_already && turn_mcp != nullptr) {
                    const auto discovered = turn_mcp->DiscoverEnabled(&runtime->cancellation);
                    const std::string status = discovered.ok ? WideToUtf8(i18n::Text(L"MCP tools refreshed"))
                        : WideToUtf8(i18n::Text(L"MCP initialization failed: ")) + discovered.error;
                    post_event(EventKind::Status, status);
                    if (use_override) mcp_network_discovered_ = true;
                    else mcp_discovered_.store(true);
                }
            }
            Agent agent(runtime->config, &runtime->journal, turn_mcp, subagents_.get(),
                        {runtime->plan_mode ? CollaborationMode::Plan : CollaborationMode::Execute, runtime->goal});
            AgentCallbacks callbacks;
            callbacks.on_status = [&](const std::string& status) { post_event(EventKind::Status, status); };
            callbacks.on_phase = [&](AgentPhase phase, const std::string& detail) {
                flush_reasoning();
                flush_text();
                post_event(EventKind::Phase, detail, {}, {}, false, phase);
            };
            callbacks.on_user_message_appended = [&]() { post_event(EventKind::UserMessageAppended); };
            callbacks.on_text_delta = [&](const std::string& delta) {
                flush_reasoning();
                pending_text += delta;
                // Codex streams each SSE chunk straight into its render queue and lets the
                // frame tick decide what is visible. Keep the same shape here: coalesce only
                // big or rate-limited batches so the UI always holds near-current text.
                if (pending_text.size() >= 2048 || spark::MonotonicMillis() - last_text_post >= 25) flush_text();
            };
            callbacks.on_reasoning_delta = [&](const std::string& delta) {
                pending_reasoning += delta;
                if (pending_reasoning.size() >= 2048 || spark::MonotonicMillis() - last_reasoning_post >= 25) {
                    flush_reasoning();
                }
            };
            callbacks.on_tool_start = [&](const std::string& call_id, const std::string& name,
                                          const std::string& arguments) {
                flush_reasoning();
                flush_text();
                post_event(EventKind::ToolStart, call_id, name, arguments);
            };
            callbacks.on_tool_finish = [&](const std::string& call_id, const std::string& name, bool success,
                                           const std::string& output) {
                flush_reasoning();
                flush_text();
                post_event(EventKind::ToolFinish, call_id, name, output, success);
            };
            callbacks.on_model_usage = [runtime](int input_tokens, int output_tokens, int cached_input_tokens) {
                const uint64_t input = static_cast<uint64_t>(std::max(0, input_tokens));
                const uint64_t output = static_cast<uint64_t>(std::max(0, output_tokens));
                const uint64_t cached = static_cast<uint64_t>(std::max(0, cached_input_tokens));
                runtime->usage_input_tokens.fetch_add(input);
                runtime->usage_output_tokens.fetch_add(output);
                runtime->usage_cached_input_tokens.fetch_add(cached);
                runtime->usage_last_input_tokens.store(input);
                runtime->usage_last_output_tokens.store(output);
                runtime->usage_model_calls.fetch_add(1);
            };
            // The agent reports the live context size after every provider call and after every
            // compaction, so the header tracks the conversation like Codex instead of waiting for
            // the whole turn to finish.
            callbacks.on_context_updated = [&](uint64_t used, uint64_t limit) {
                runtime->usage_context_tokens.store(used);
                runtime->usage_context_limit_tokens.store(limit);
                runtime->usage_context_published.store(true);
                post_event(EventKind::Context, std::to_string(used), std::to_string(limit));
            };
            callbacks.on_context_compacted = [&](uint64_t before, uint64_t after, const std::string& trigger) {
                post_event(EventKind::Compaction, std::to_string(before), std::to_string(after), {},
                           trigger != "manual");
            };
            callbacks.on_error = [&](const std::string& error) {
                flush_reasoning();
                flush_text();
                post_event(EventKind::Error, error);
            };
            const auto result = agent.RunTurn(turn_text,
                                              [this, runtime](const ApprovalRequest& request) {
                                                  return AskApproval(runtime, request);
                                              },
                                               callbacks, &runtime->cancellation, runtime->resume_turn,
                                               turn_image_paths);
            flush_reasoning();
            flush_text();
            const bool cancelled = runtime->cancellation.IsCancelled();
            const bool paused = cancelled && runtime->interrupt_requested.load();
            if (!runtime->usage_context_published.load()) {
                // Local commands never call the provider; fall back to a local estimate so the
                // header and the usage card still agree.
                const uint64_t used = EstimateMessagesTokens(ModelContextMessages(runtime->journal.State()));
                const ProviderConfig* provider = FindProvider(runtime->config, runtime->config.active.provider);
                const ModelConfig* model =
                    provider == nullptr ? nullptr : FindModel(*provider, runtime->config.active.model);
                const uint64_t limit =
                    model == nullptr ? static_cast<uint64_t>(runtime->config.context.fallback_context_window)
                                     : EffectiveContextLimit(runtime->config.context, *model);
                runtime->usage_context_tokens.store(used);
                runtime->usage_context_limit_tokens.store(limit);
                post_event(EventKind::Context, std::to_string(used), std::to_string(limit));
            }
            const bool success = result.ok || cancelled;
            runtime->cancelled.store(cancelled);
            runtime->success.store(success);
            runtime->complete.store(true);
            post_event(EventKind::TurnFinished,
                       paused ? std::string("paused") : cancelled ? std::string("cancelled") : std::string{}, {}, {},
                       success);
        } catch (const std::exception& exception) {
            flush_reasoning();
            flush_text();
            runtime->cancelled.store(false);
            runtime->success.store(false);
            runtime->complete.store(true);
            post_event(EventKind::Error, WideToUtf8(i18n::Text(L"Turn failed: ")) + exception.what());
            post_event(EventKind::TurnFinished);
        } catch (...) {
            flush_reasoning();
            flush_text();
            runtime->cancelled.store(false);
            runtime->success.store(false);
            runtime->complete.store(true);
            post_event(EventKind::Error, WideToUtf8(i18n::Text(L"Turn failed: unknown exception")));
            post_event(EventKind::TurnFinished);
        }
    });
}

void AppWindow::CancelTurn() {
    const auto runtime = FindTurn(journal_.Path());
    if (runtime == nullptr) return;
    runtime->cancellation.Cancel();
    if (runtime->ui_attached) SetPhase(runtime, AgentPhase::Cancelling, {});
}

void AppWindow::InterruptTurn() {
    const auto runtime = FindTurn(journal_.Path());
    if (runtime != nullptr) {
        runtime->interrupt_requested.store(true);
        runtime->cancellation.Cancel();
        if (runtime->ui_attached) SetPhase(runtime, AgentPhase::Cancelling, {});
        return;
    }
    const std::string key = NormalizePathForComparison(journal_.Path());
    const auto external = external_run_details_.find(key);
    if (external == external_run_details_.end() || !external->second.is_object()) return;
    const std::string id = external->second.value("id", std::string{});
    if (id.empty()) return;
    const std::string directory = JoinPath(config_.storage.data_dir, "external_agents");
    std::error_code directory_error;
    spark::fs::create_directories(Utf8ToWide(directory), directory_error);
    if (directory_error) return;
    const nlohmann::json request = {
        {"action", "cancel"},
        {"id", id},
        {"requested_at", UnixTimeSeconds()},
    };
    const std::string path = JoinPath(directory, "control.json");
    std::string write_error;
    WriteFileUtf8Atomic(path, request.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), &write_error);
    SetStatus(i18n::Text(L"Subagent cancellation requested..."));
}

void AppWindow::ResumePausedTurn() {
    if (!HasPausedTurn()) return;
    resume_turn_requested_ = true;
    StartTurn();
}

void AppWindow::ToggleSendAction() {
    if (IsActiveSessionRunning()) {
        InterruptTurn();
    } else if (HasPausedTurn() && Trim(ComposerText()).empty()) {
        ResumePausedTurn();
    } else {
        StartTurn();
    }
}

void AppWindow::BeginActivityAnimation() {
    activity_frame_ = 0;
    if (window_ == nullptr || activity_timer_active_) return;
    activity_timer_active_ = true;
    SetTimer(window_, kActivityTimer, kActivityIntervalMs, nullptr);
}

bool AppWindow::RefreshExternalRuns() {
    std::set<std::string> current;
    std::map<std::string, nlohmann::json> details;
    CollectExternalRunningSessions(config_.storage.data_dir, &current, &details);
    const bool changed = current != external_running_sessions_;
    external_running_sessions_ = std::move(current);
    external_run_details_ = std::move(details);
    return changed;
}

void AppWindow::PollExternalRuns() {
    const std::set<std::string> previous_running = external_running_sessions_;
    const bool changed = RefreshExternalRuns();
    if (mcp_reload_pending_ && !AnyTurnRunning()) ReloadMcpWhenIdle();
    bool live_changed = external_run_states_.size() != external_run_details_.size();
    for (const auto& entry : external_run_details_) {
        const auto previous = external_run_states_.find(entry.first);
        if (previous == external_run_states_.end() || previous->second != entry.second) {
            live_changed = true;
            break;
        }
    }
    external_run_states_ = external_run_details_;

    if (changed) {
        RefreshHistory();
        RefreshProjectRows();
        if (!external_running_sessions_.empty() && !activity_timer_active_) BeginActivityAnimation();
        for (const std::string& session : external_running_sessions_) InvalidateSidebarRow(session);
        UpdateTurnUi();
    }

    bool reload_active = false;
    if (!SessionLoading() && !pending_new_conversation_ && !journal_.Path().empty() && FindTurn(journal_.Path()) == nullptr) {
        const std::string session_key = NormalizePathForComparison(journal_.Path());
        if (live_changed && external_run_details_.find(session_key) != external_run_details_.end()) {
            reload_active = true;
        }
        if (previous_running.find(session_key) != previous_running.end() &&
            external_run_details_.find(session_key) == external_run_details_.end()) {
            reload_active = true;
        }
        ULONGLONG size = 0;
        FILETIME write_time{};
        if (ReadFileStamp(journal_.Path(), &size, &write_time)) {
            const auto stamp = external_session_stamps_.find(session_key);
            const bool stamp_changed =
                stamp == external_session_stamps_.end() || stamp->second.first != size ||
                CompareFileTime(&stamp->second.second, &write_time) != 0;
            if (stamp_changed) {
                reload_active = true;
                external_session_stamps_[session_key] = {size, write_time};
            }
        }
        if (reload_active && journal_.Resume(journal_.Path()).ok) {
            ReloadSessionIntoView();
            RefreshContextLabel();
        }
    }

    if (!external_running_sessions_.empty()) {
        if (!activity_timer_active_) BeginActivityAnimation();
        for (const std::string& session : external_running_sessions_) InvalidateSidebarRow(session);
    } else if (active_turns_.empty()) {
        StopActivityAnimation();
    }
}

void AppWindow::StopActivityAnimation() {
    if (window_ != nullptr && activity_timer_active_) KillTimer(window_, kActivityTimer);
    activity_timer_active_ = false;
    activity_frame_ = 0;
    phase_text_.clear();
    if (chat_view_ != nullptr) {
        chat_view_->SetActivityFrame(activity_frame_);
        chat_view_->SetActivityState(false, {});
    }
    if (status_label_ != nullptr) InvalidateRect(status_label_, nullptr, FALSE);
    if (window_ != nullptr) InvalidateRect(window_, nullptr, FALSE);
    // The sidebar spinner belongs to the running turn and has to disappear with it.
    if (history_list_ != nullptr) InvalidateRect(history_list_, nullptr, FALSE);
    if (projects_list_ != nullptr) InvalidateRect(projects_list_, nullptr, FALSE);
}

void AppWindow::TickActivity() {
    ++activity_frame_;
    const bool active_running = IsActiveSessionRunning();
    if (chat_view_ != nullptr && active_running) chat_view_->SetActivityFrame(activity_frame_);
    if (side_chat_open_ && side_chat_view_ != nullptr && !active_subagent_id_.empty()) {
        const auto child = subagents_ui_.find(active_subagent_id_);
        if (child != subagents_ui_.end() && child->second.info.state == SubAgentState::Running) {
            side_chat_view_->SetActivityFrame(activity_frame_);
        }
    }
    if (!AnyTurnRunning()) {
        StopActivityAnimation();
        return;
    }
    // The live status lives inside the transcript now, so the timer only has to keep
    // the shimmer sweeping; the line above the composer is left for real messages.
    if (chat_view_ != nullptr && active_running && ActivityRowVisible()) {
        chat_view_->SetActivityState(true, phase_text_);
    }
    // Repaint only the rows that own a running turn. Invalidating both list boxes in full on
    // every tick made every conversation visibly flash on Win7's non-composited desktop.
    for (const auto& entry : active_turns_) InvalidateSidebarRow(entry.first);
    for (const std::string& session : external_running_sessions_) InvalidateSidebarRow(session);
}

void AppWindow::InvalidateSidebarRow(const std::string& session_key) {
    if (session_key.empty()) return;
    const auto invalidate_row = [&](HWND list, size_t index) {
        if (list == nullptr) return;
        RECT rect{};
        if (SendMessageW(list, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&rect)) == LB_ERR) {
            // The row is scrolled out of view, so there is nothing to repaint.
            return;
        }
        InvalidateRect(list, &rect, FALSE);
    };
    for (size_t index = 0; index < history_paths_.size(); ++index) {
        if (NormalizePathForComparison(history_paths_[index]) != session_key) continue;
        invalidate_row(history_list_, index);
        break;
    }
    for (size_t index = 0; index < project_rows_.size(); ++index) {
        if (project_rows_[index].session_path.empty()) continue;
        if (NormalizePathForComparison(project_rows_[index].session_path) != session_key) continue;
        invalidate_row(projects_list_, index);
        break;
    }
}

bool AppWindow::ActivityRowVisible() const {
    // Streamed reasoning is a real transcript item now, so the transient status row must
    // not duplicate it. Tool rows and answer text carry their own live progress too.
    if (active_turn_entry_ != static_cast<size_t>(-1)) return false;
    if (active_reasoning_entry_ != static_cast<size_t>(-1)) return false;
    if (current_phase_ == AgentPhase::RunningTool || current_phase_ == AgentPhase::Streaming) return false;
    return true;
}

void AppWindow::UpdateSendButton() {
    const bool running = IsActiveSessionRunning();
    const bool has_text = !Trim(ComposerText()).empty();
    const bool paused = !running && HasPausedTurn();
    const bool show_play = paused && !has_text;
    SetWindowTextW(send_button_, running ? L"■" : show_play ? L"▶" : L"↑");
    const bool enabled = !SessionLoading() && (running || paused || has_text);
    EnableWindow(send_button_, enabled ? TRUE : FALSE);
    InvalidateRect(send_button_, nullptr, TRUE);
}

void AppWindow::HandleUiEvent(UiEvent* event) {
    if (event == nullptr) return;
    if (HandleSubAgentEvent(event)) { delete event; return; }
    const auto found = active_turns_.find(event->session_key);
    if (found == active_turns_.end()) {
        delete event;
        return;
    }
    const std::shared_ptr<TurnRuntime> runtime = found->second;
    const std::string loading_path = session_load_path_.empty() ? journal_.Path() : session_load_path_;
    if (SessionLoading() && NormalizePathForComparison(loading_path) == runtime->session_key &&
        event->kind != EventKind::TurnFinished) {
        const size_t bytes = event->first.size() + event->second.size() + event->third.size();
        if (session_deferred_events_.size() < 1024 && session_deferred_bytes_ + bytes <= 2 * 1024 * 1024) {
            session_deferred_bytes_ += bytes;
            session_deferred_events_.emplace_back(event);
        } else {
            session_deferred_overflow_ = true;
            delete event;
        }
        return;
    }
    const bool active = !SessionLoading() && !pending_new_conversation_ &&
                        NormalizePathForComparison(journal_.Path()) == runtime->session_key;
    const bool attached = active && runtime->ui_attached && attached_turn_key_ == runtime->session_key;
    if (!attached) {
        switch (event->kind) {
            case EventKind::Phase:
                SetPhase(runtime, event->phase, event->first);
                break;
            case EventKind::ReasoningDelta:
                AppendReasoningPreview(runtime, event->first);
                SetPhase(runtime, AgentPhase::WaitingModel, "thinking");
                break;
            case EventKind::TextDelta:
                SetPhase(runtime, AgentPhase::Streaming, {});
                break;
            case EventKind::TurnFinished:
                FinishTurn(runtime, event->success, event->first);
                break;
            case EventKind::UserMessageAppended:
                show_active_placeholder_ = false;
                RefreshHistory();
                RefreshProjectRows();
                break;
            case EventKind::Context:
                if (!event->first.empty()) {
                    session_context_tokens_[runtime->session_key] = ParseTokenCount(event->first);
                }
                if (active) RefreshContextLabel();
                break;
            case EventKind::Status:
            case EventKind::ToolStart:
            case EventKind::ToolFinish:
        case EventKind::Compaction:
            case EventKind::Error:
                break;
        }
        delete event;
        return;
    }
    switch (event->kind) {
        case EventKind::Status:
            break;
        case EventKind::Phase:
            SetPhase(runtime, event->phase, event->first);
            break;
        case EventKind::ReasoningDelta: {
            AppendReasoningPreview(runtime, event->first);
            if (chat_view_ != nullptr) {
                if (active_reasoning_entry_ == static_cast<size_t>(-1)) {
                    FinishActiveReasoning(true);
                    active_reasoning_entry_ = chat_view_->AddReasoning();
                    latest_reasoning_entry_ = active_reasoning_entry_;
                }
                chat_view_->AppendReasoning(active_reasoning_entry_, event->first);
            }
            SetPhase(runtime, AgentPhase::WaitingModel, "thinking");
            break;
        }
        case EventKind::UserMessageAppended:
            show_active_placeholder_ = false;
            RefreshHistory();
            RefreshProjectRows();
            break;
        case EventKind::TextDelta:
            SetPhase(runtime, AgentPhase::Streaming, {});
            FinishActiveReasoning(true);
            if (!assistant_stream_started_) {
                active_assistant_entry_ = chat_view_->AddAssistant();
                assistant_stream_started_ = true;
            }
            chat_view_->AppendAssistant(active_assistant_entry_, event->first);
            break;
        case EventKind::ToolStart:
            FinishActiveReasoning(true);
            if (assistant_stream_started_ && chat_view_ != nullptr) {
                chat_view_->FinishAssistant(active_assistant_entry_);
            }
            assistant_stream_started_ = false;
            active_assistant_entry_ = static_cast<size_t>(-1);
            ShowWindow(empty_mark_, SW_HIDE);
            ShowWindow(empty_title_, SW_HIDE);
            {
                std::string call_id = event->first;
                if (call_id.empty()) call_id = "legacy-" + event->second + "-" + std::to_string(active_tool_entries_.size());
                active_tool_entries_[call_id] = chat_view_->AddTool(event->second, event->third);
            }
            break;
        case EventKind::ToolFinish: {
            std::string call_id = event->first;
            auto iterator = active_tool_entries_.find(call_id);
            size_t entry = iterator == active_tool_entries_.end() ? static_cast<size_t>(-1) : iterator->second;
            if (iterator != active_tool_entries_.end()) active_tool_entries_.erase(iterator);
            if (entry >= chat_view_->Count()) {
                entry = chat_view_->AddTool(event->second.empty() ? "tool" : event->second, "{}");
            }
            chat_view_->FinishTool(entry, event->success, event->third);
            break;
        }
        case EventKind::TurnFinished:
            FinishTurn(runtime, event->success, event->first);
            break;
        case EventKind::Context: {
            if (!event->first.empty()) {
                session_context_tokens_[runtime->session_key] = ParseTokenCount(event->first);
            }
            RefreshContextLabel();
            break;
        }
        case EventKind::Compaction:
            FinishActiveReasoning(true);
            if (assistant_stream_started_ && chat_view_ != nullptr) {
                chat_view_->FinishAssistant(active_assistant_entry_);
            }
            assistant_stream_started_ = false;
            active_assistant_entry_ = static_cast<size_t>(-1);
            if (chat_view_ != nullptr) chat_view_->AddCompactionNotice(event->success);
            break;
        case EventKind::Error:
            FinishActiveReasoning(true);
            if (IsQuotaExhaustedError(event->first)) {
                chat_view_->AddQuotaNotice();
            } else {
                chat_view_->AddError(event->first);
            }
            break;
    }
    delete event;
}

void AppWindow::FinishTurn(const std::shared_ptr<TurnRuntime>& runtime, bool success, const std::string& reason) {
    if (runtime == nullptr) return;
    const auto found = active_turns_.find(runtime->session_key);
    if (found == active_turns_.end() || found->second != runtime) return;
    if (runtime->worker.joinable()) runtime->worker.join();

    const bool active = !SessionLoading() && !pending_new_conversation_ &&
                        NormalizePathForComparison(journal_.Path()) == runtime->session_key;
    const bool attached = active && runtime->ui_attached && attached_turn_key_ == runtime->session_key;
    const uint64_t elapsed_ms = runtime->started_tick == 0 ? 0 : spark::MonotonicMillis() - runtime->started_tick;
    const bool paused = reason == "paused";
    const uint64_t usage_input_tokens = runtime->usage_input_tokens.load();
    const uint64_t usage_output_tokens = runtime->usage_output_tokens.load();
    const uint64_t usage_cached_input_tokens = runtime->usage_cached_input_tokens.load();
    const uint64_t usage_model_calls = runtime->usage_model_calls.load();
    const uint64_t last_request_tokens = runtime->usage_last_input_tokens.load() +
                                         runtime->usage_last_output_tokens.load();
    // The header label and this usage card must show one number. When the agent published a live
    // context size (provider usage, or the size after a compaction) use exactly that value;
    // otherwise fall back to the local estimate.
    const uint64_t usage_context_tokens =
        runtime->usage_context_published.load()
            ? runtime->usage_context_tokens.load()
            : std::max(runtime->usage_context_tokens.load(), last_request_tokens);
    const uint64_t usage_context_limit_tokens = runtime->usage_context_limit_tokens.load();
    if (runtime->timed_turn) {
        const size_t user_turns = static_cast<size_t>(
            std::count_if(runtime->journal.State().messages.begin(), runtime->journal.State().messages.end(),
                          [](const ChatMessage& message) { return message.role == MessageRole::User; }));
        const size_t turn_index = user_turns == 0 ? 0 : user_turns - 1;
        runtime->journal.Append("turn.duration", {
            {"turn_index", turn_index},
            {"elapsed_ms", elapsed_ms},
            {"success", success},
            {"reason", reason},
        });
        if (usage_model_calls > 0) {
            runtime->journal.Append("turn.usage", {
                {"turn_index", turn_index},
                {"input_tokens", usage_input_tokens},
                {"output_tokens", usage_output_tokens},
                {"cached_input_tokens", usage_cached_input_tokens},
                {"model_calls", usage_model_calls},
                {"context_tokens", usage_context_tokens},
                {"context_limit_tokens", usage_context_limit_tokens},
                {"elapsed_ms", elapsed_ms},
            });
        }
    }
    if (paused) {
        PersistInterruptedTurn(runtime, elapsed_ms);
    } else {
        ClearPausedTurn(runtime->journal);
    }

    if (attached && chat_view_ != nullptr) {
        FinishActiveReasoning(false);
        if (assistant_stream_started_) {
            chat_view_->FinishAssistant(active_assistant_entry_);
            assistant_stream_started_ = false;
            active_assistant_entry_ = static_cast<size_t>(-1);
        }
        for (const auto& [call_id, entry] : active_tool_entries_) {
            (void)call_id;
            if (entry < chat_view_->Count()) {
                chat_view_->FinishTool(entry, false, paused ? "Interrupted by user before completion."
                                                             : "Tool did not report completion before the turn ended.");
            }
        }
        if (active_turn_entry_ != static_cast<size_t>(-1)) {
            if (paused) chat_view_->MarkTurnPaused(active_turn_entry_, elapsed_ms);
            else chat_view_->FinishTurnStatus(active_turn_entry_, elapsed_ms);
            active_turn_entry_ = static_cast<size_t>(-1);
        }
        if (usage_model_calls > 0) {
            chat_view_->AddUsageStats(usage_input_tokens, usage_output_tokens, usage_cached_input_tokens,
                                      usage_model_calls, usage_context_tokens, usage_context_limit_tokens,
                                      elapsed_ms);
        }
    }
    if (attached) {
        active_tool_entries_.clear();
        active_assistant_entry_ = static_cast<size_t>(-1);
        active_reasoning_entry_ = static_cast<size_t>(-1);
        latest_reasoning_entry_ = static_cast<size_t>(-1);
        assistant_stream_started_ = false;
        turn_started_tick_ = 0;
        attached_turn_key_.clear();
    }

    const std::string pending_archive = runtime->pending_archive_path;
    const std::string pending_delete = runtime->pending_delete_path;
    active_turns_.erase(runtime->session_key);
    if (active_turns_.empty()) KillTimer(window_, kTurnWatchdogTimer);

    if (!AnyTurnRunning() && mcp_reload_pending_) {
        ReloadMcpWhenIdle();
    }

    const std::string loading_path = session_load_path_.empty() ? journal_.Path() : session_load_path_;
    if (SessionLoading() && NormalizePathForComparison(loading_path) == runtime->session_key) {
        // The final duration and transcript were written after the reader's snapshot.
        QueueSessionLoad(runtime->path);
    } else if (active) {
        journal_.Resume(runtime->path);
        RefreshSummaryPanel();
        if (!attached) {
            ReloadSessionIntoView();
            RestoreComposerState();
            RefreshContextLabel();
        }
        SetStatus(paused ? i18n::Text(L"Interrupted. Click the resume button to continue this turn.")
                         : success ? (reason == "cancelled" ? i18n::Text(L"Stopped.") : L"")
                          : i18n::Text(L"This turn ended with an error.\nSend another message or check the configuration in Settings."));
    }
    UpdateTurnUi();
    RefreshHistory();
    RefreshProjectRows();

    if (!pending_archive.empty()) {
        ArchiveSessionPath(pending_archive);
    } else if (!pending_delete.empty()) {
        HandleSessionAction(pending_delete, kHistoryDelete);
    }
    if (close_pending_ && active_turns_.empty()) {
        RequestExit();
    }
}

void AppWindow::PersistInterruptedTurn(const std::shared_ptr<TurnRuntime>& runtime, uint64_t elapsed_ms) {
    if (runtime == nullptr) return;
    SessionState& state = runtime->journal.MutableState();
    const bool attached = NormalizePathForComparison(journal_.Path()) == runtime->session_key &&
                          runtime->ui_attached && attached_turn_key_ == runtime->session_key;
    const auto append_message = [&](const ChatMessage& message) {
        state.messages.push_back(message);
        if (!state.context_messages.empty()) state.context_messages.push_back(message);
        runtime->journal.Append("message", message);
    };

    if (attached && chat_view_ != nullptr && active_assistant_entry_ != static_cast<size_t>(-1)) {
        const ChatItem* item = chat_view_->Item(active_assistant_entry_);
        if (item != nullptr && item->kind == ChatItemKind::Assistant && !Trim(item->text).empty()) {
            const bool already_saved = !state.messages.empty() &&
                                       state.messages.back().role == MessageRole::Assistant &&
                                       state.messages.back().content == item->text &&
                                       state.messages.back().tool_calls.empty();
            if (!already_saved) {
                ChatMessage assistant;
                assistant.role = MessageRole::Assistant;
                assistant.content = item->text;
                assistant.timestamp = UnixTimeSeconds();
                append_message(assistant);
            }
        }
    }

    for (const auto& [call_id, entry] : active_tool_entries_) {
        (void)entry;
        if (call_id.empty()) continue;
        const bool already_saved =
            std::any_of(state.messages.begin(), state.messages.end(), [&](const ChatMessage& message) {
                return message.role == MessageRole::Tool && message.tool_call_id == call_id;
            });
        if (already_saved) continue;
        ChatMessage tool;
        tool.role = MessageRole::Tool;
        tool.name = "interrupted";
        tool.tool_call_id = call_id;
        tool.content = "Interrupted by user before completion. The tool may have partially executed.";
        tool.timestamp = UnixTimeSeconds();
        append_message(tool);
    }

    ChatMessage marker;
    marker.role = MessageRole::System;
    marker.content = kInterruptedTurnMarker;
    marker.timestamp = UnixTimeSeconds();
    append_message(marker);

    if (!state.metadata.is_object()) state.metadata = nlohmann::json::object();
    state.metadata["paused_turn"] = {
        {"elapsed_ms", elapsed_ms},
        {"interrupted_at", UnixTimeSeconds()},
    };
    runtime->journal.SaveSnapshot();
}

bool AppWindow::HasPausedTurn() const {
    if (pending_new_conversation_ || pending_edit_index_ != static_cast<size_t>(-1)) return false;
    const nlohmann::json& metadata = journal_.State().metadata;
    return metadata.is_object() && metadata.contains("paused_turn") && metadata["paused_turn"].is_object();
}

uint64_t AppWindow::PausedTurnElapsedMs() const {
    if (!HasPausedTurn()) return 0;
    return journal_.State().metadata["paused_turn"].value("elapsed_ms", uint64_t{0});
}

void AppWindow::ClearPausedTurn(SessionJournal& journal) {
    nlohmann::json& metadata = journal.MutableState().metadata;
    if (!metadata.is_object() || metadata.erase("paused_turn") == 0) return;
    journal.SaveSnapshot();
}

void AppWindow::DiscardPausedTurnForNewMessage(SessionJournal& journal) {
    SessionState& state = journal.MutableState();
    bool changed = false;
    const auto remove_tail_marker = [](std::vector<ChatMessage>& messages) {
        if (messages.empty()) return false;
        const ChatMessage& last = messages.back();
        if (last.role != MessageRole::System || last.content != kInterruptedTurnMarker) return false;
        messages.pop_back();
        return true;
    };
    changed = remove_tail_marker(state.messages) || changed;
    if (!state.context_messages.empty()) {
        changed = remove_tail_marker(state.context_messages) || changed;
    }
    if (state.metadata.is_object() && state.metadata.erase("paused_turn") > 0) changed = true;
    if (changed) journal.SaveSnapshot();
}

bool AppWindow::AskApproval(const std::shared_ptr<TurnRuntime>& runtime, const ApprovalRequest& request) {
    if (runtime == nullptr || runtime->cancellation.IsCancelled()) return false;
    if (runtime->plan_mode && request.kind != ApprovalKind::OutsideWorkspaceRead) return false;
    if (runtime->approve_turn.load()) return true;
    ApprovalContext context;
    context.request = request;
    context.runtime = runtime;
    SendMessageW(window_, kApprovalMessage, 0, reinterpret_cast<LPARAM>(&context));
    return context.approved;
}

void AppWindow::ClearChatView(ChatViewReset mode) {
    session_loader_.Cancel();
    session_load_path_.clear();
    session_replay_step_ = {};
    session_deferred_events_.clear();
    session_deferred_bytes_ = 0;
    session_deferred_overflow_ = false;
    session_view_loading_ = mode == ChatViewReset::Loading;
    if (!SessionLoading()) session_load_started_tick_ = 0;
    else if (session_load_started_tick_ == 0) session_load_started_tick_ = spark::MonotonicMillis();
    KillTimer(window_, kSessionLoadTimer);
    EnableWindow(composer_, SessionLoading() ? FALSE : TRUE);
    EnableWindow(plus_button_, SessionLoading() ? FALSE : TRUE);
    DetachAttachedTurn();
    active_tool_entries_.clear();
    active_turn_entry_ = static_cast<size_t>(-1);
    active_assistant_entry_ = static_cast<size_t>(-1);
    active_reasoning_entry_ = static_cast<size_t>(-1);
    latest_reasoning_entry_ = static_cast<size_t>(-1);
    assistant_stream_started_ = false;
    current_phase_ = AgentPhase::WaitingModel;
    phase_text_.clear();
    reasoning_preview_.clear();
    turn_started_tick_ = 0;
    pending_edit_index_ = static_cast<size_t>(-1);
    if (chat_view_ != nullptr) chat_view_->Clear();
}

void AppWindow::ToggleToolEntry(size_t index) {
    if (chat_view_ != nullptr) chat_view_->ToggleTool(index);
}

void AppWindow::ToggleToolGroupEntry(size_t index) {
    if (chat_view_ != nullptr) chat_view_->ToggleToolGroup(index);
}

void AppWindow::CopyChatItem(size_t index) {
    if (chat_view_ == nullptr) return;
    const ChatItem* item = chat_view_->Item(index);
    if (item == nullptr) return;
    std::string text = item->text;
    if (item->kind == ChatItemKind::Tool) {
        text = item->tool_name + "\n" + item->arguments;
        if (!item->result.empty()) text += "\n\n" + item->result;
    }
    if (text.empty()) return;
    const std::wstring wide = Utf8ToWide(text);
    if (!OpenClipboard(window_)) return;
    EmptyClipboard();
    const SIZE_T bytes = (wide.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory != nullptr) {
        void* destination = GlobalLock(memory);
        if (destination != nullptr) memcpy(destination, wide.c_str(), bytes);
        GlobalUnlock(memory);
        SetClipboardData(CF_UNICODETEXT, memory);
    }
    CloseClipboard();
    SetStatus(i18n::Text(L"Message copied."));
}

void AppWindow::EditChatItem(size_t index) {
    if (IsActiveSessionRunning() || chat_view_ == nullptr) return;
    const ChatItem* item = chat_view_->Item(index);
    if (item == nullptr || item->kind != ChatItemKind::User || index != chat_view_->LastUserIndex()) return;
    pending_edit_index_ = index;
    SetWindowTextW(composer_, Utf8ToWide(item->text).c_str());
    SetFocus(composer_);
    UpdateSendButton();
    SetStatus(i18n::Text(L"Editing the last message. Sending will replace it and its replies."));
}

void AppWindow::FinishActiveReasoning(bool hide) {
    if (chat_view_ != nullptr) {
        size_t entry = active_reasoning_entry_;
        if (entry == static_cast<size_t>(-1)) entry = latest_reasoning_entry_;
        if (entry != static_cast<size_t>(-1) && entry < chat_view_->Count()) {
            chat_view_->FinishReasoning(entry);
            if (hide) chat_view_->HideReasoning(entry);
        }
    }
    active_reasoning_entry_ = static_cast<size_t>(-1);
    latest_reasoning_entry_ = static_cast<size_t>(-1);
}

void AppWindow::AppendReasoningPreview(const std::shared_ptr<TurnRuntime>& runtime, const std::string& delta) {
    if (runtime == nullptr) return;
    // Keep a bounded one-line preview per turn. Detached turns still update this
    // state so switching back restores the same live reasoning context.
    for (const wchar_t character : Utf8ToWide(delta)) {
        if (character == L'\r' || character == L'\n' || character == L'\t') {
            if (!runtime->reasoning_preview.empty() && runtime->reasoning_preview.back() != L' ') {
                runtime->reasoning_preview.push_back(L' ');
            }
            continue;
        }
        runtime->reasoning_preview.push_back(character);
    }
    if (runtime->reasoning_preview.size() > 200) {
        runtime->reasoning_preview.erase(0, runtime->reasoning_preview.size() - 200);
    }
}

void AppWindow::SetPhase(const std::shared_ptr<TurnRuntime>& runtime, AgentPhase phase, const std::string& detail) {
    if (runtime == nullptr) return;
    const bool attached = runtime->ui_attached && attached_turn_key_ == runtime->session_key &&
                          NormalizePathForComparison(journal_.Path()) == runtime->session_key;
    runtime->phase = phase;
    runtime->phase_detail = detail;
    std::wstring text;
    switch (phase) {
        case AgentPhase::WaitingModel:
            if (detail == "thinking") {
                // Codex shows the reasoning text itself, so no "thinking" prefix is added.
                text = runtime->reasoning_preview;
            } else {
                // A new provider request starts a new reasoning segment. The previous one
                // is transient within the active turn and must not accumulate above the
                // current command/output.
                if (attached) FinishActiveReasoning(true);
                runtime->reasoning_preview.clear();
                text = i18n::Text(L"Waiting for the model...");
                if (!detail.empty()) text += L"  " + Utf8ToWide(detail);
            }
            break;
        case AgentPhase::Streaming:
            // The answer streams into the transcript, but the model can still spend minutes
            // before it emits the next tool call, so the last reasoning text stays visible.
            text = runtime->reasoning_preview;
            break;
        case AgentPhase::Compacting:
            text = i18n::Text(L"Compacting context...");
            break;
        case AgentPhase::RunningTool:
            if (attached) FinishActiveReasoning(true);
            runtime->reasoning_preview.clear();
            text = detail.empty() ? i18n::Text(L"Running tool...") : i18n::Text(L"Running ") + Utf8ToWide(detail);
            break;
        case AgentPhase::WaitingApproval:
            text = i18n::Text(L"Waiting for your approval...");
            break;
        case AgentPhase::Cancelling:
            text = i18n::Text(L"Stopping...");
            break;
        case AgentPhase::Completed:
            runtime->phase_text.clear();
            runtime->reasoning_preview.clear();
            if (attached) {
                FinishActiveReasoning(true);
                current_phase_ = phase;
                phase_text_.clear();
                reasoning_preview_.clear();
                if (chat_view_ != nullptr) chat_view_->SetActivityState(false, {});
            }
            return;
    }
    runtime->phase_text = text;
    if (!attached) return;
    current_phase_ = phase;
    phase_text_ = text;
    reasoning_preview_ = runtime->reasoning_preview;
    if (chat_view_ != nullptr) chat_view_->SetActivityState(ActivityRowVisible(), text, runtime->started_tick);
}

void AppWindow::SetStatus(const std::wstring& text) {
    (void)text;
    if (status_label_ != nullptr) ShowWindow(status_label_, SW_HIDE);
}

std::string AppWindow::ComposerText() const { return WideToUtf8(ReadWindowText(composer_)); }

void AppWindow::ClearComposer() {
    SetWindowTextW(composer_, L"");
    UpdateSendButton();
}

void AppWindow::RefreshContextLabel() {
    const ProviderConfig* provider = FindProvider(config_, config_.active.provider);
    const ModelConfig* model = provider == nullptr ? nullptr : FindModel(*provider, config_.active.model);
    const uint64_t limit = model == nullptr ? static_cast<uint64_t>(config_.context.fallback_context_window)
                                            : EffectiveContextLimit(config_.context, *model);
    uint64_t used = 0;
    if (!pending_new_conversation_) {
        // One source of truth: whatever the agent last published (provider usage or the size of
        // the compacted context). The per-turn usage card reads the same value, so the header and
        // the card can no longer disagree.
        const auto cached = session_context_tokens_.find(NormalizePathForComparison(journal_.Path()));
        if (cached != session_context_tokens_.end()) {
            used = cached->second;
        } else {
            used = LastRecordedContextTokens(journal_.State());
            if (used == 0) used = EstimateMessagesTokens(ModelContextMessages(journal_.State()));
        }
    }
    SetWindowTextW(context_label_, (std::wstring(Text(L"Context ")) + FormatTokenCount(used) + L" / " +
                                    FormatTokenCount(limit)).c_str());
}

bool AppWindow::FindProjectName(const std::string& workspace, std::string* name) const {
    if (workspace.empty()) return false;
    const std::string normalized_workspace = NormalizePathForComparison(
        AbsolutePath(workspace, ParentPath(config_.config_path)));
    for (const ProjectEntry& project : projects_) {
        const std::string project_workspace = AbsolutePath(project.workspace, ParentPath(config_.config_path));
        bool match = NormalizePathForComparison(project_workspace) == normalized_workspace;
        for (const std::string& folder : project.folders) {
            if (NormalizePathForComparison(AbsolutePath(folder, ParentPath(config_.config_path))) ==
                normalized_workspace) {
                match = true;
                break;
            }
        }
        if (!match) continue;
        if (name != nullptr) {
            *name = project.name.empty() ? WideToUtf8(WorkspaceName(project_workspace)) : project.name;
        }
        return true;
    }
    return false;
}

std::string AppWindow::PrepareNoProjectConversationWorkspace(std::string* error) const {
    return CreateNoProjectConversationWorkspace(config_.workspace.path, error);
}

void AppWindow::UpdateActiveSessionScope(const std::string& scope, const std::string& workspace) {
    pending_new_conversation_ = false;
    active_session_scope_ = "recent";
    active_project_name_.clear();
    std::string project_name;
    if (scope == "project" && FindProjectName(workspace, &project_name)) {
        active_session_scope_ = "project";
    }
    if (active_session_scope_ == "project") {
        active_project_name_ = project_name;
    }
    UpdateEmptyState();
}

void AppWindow::UpdateEmptyState() {
    if (empty_mark_ == nullptr || empty_title_ == nullptr) return;
    if (settings_mode_) {
        if (chat_view_ != nullptr && chat_view_->hwnd() != nullptr) ShowWindow(chat_view_->hwnd(), SW_HIDE);
        ShowWindow(empty_mark_, SW_HIDE);
        ShowWindow(empty_title_, SW_HIDE);
        return;
    }
    const bool empty = !SessionLoading() && !IsActiveSessionRunning() &&
                       (pending_new_conversation_ || journal_.State().messages.empty());
    if (chat_view_ != nullptr && chat_view_->hwnd() != nullptr) {
        ShowWindow(chat_view_->hwnd(), empty ? SW_HIDE : SW_SHOW);
    }
    ShowWindow(empty_mark_, empty ? SW_SHOW : SW_HIDE);
    ShowWindow(empty_title_, empty ? SW_SHOW : SW_HIDE);
    if (active_session_scope_ == "project" && !active_project_name_.empty()) {
        const std::wstring title = i18n::Format(L"What would you like to do in {project}?",
                                                {{L"project", Utf8ToWide(active_project_name_)}});
        SetWindowTextW(empty_title_, title.c_str());
    } else {
        SetWindowTextW(empty_title_, Text(L"What do you want to do?"));
    }
    if (empty) {
        const UINT redraw_flags = RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN;
        RedrawWindow(empty_mark_, nullptr, nullptr, redraw_flags);
        RedrawWindow(empty_title_, nullptr, nullptr, redraw_flags);
        // Sidebar refreshes triggered by the current command can queue another
        // parent paint after this call. Repaint the two empty-state controls
        // once more after that command has fully returned.
        PostMessageW(window_, kEmptyStateRefreshMessage, 0, 0);
    }
}

void AppWindow::ReloadSessionIntoView() {
    if (!active_subagent_id_.empty()) {
        const auto child = subagents_ui_.find(active_subagent_id_);
        if (child == subagents_ui_.end() || NormalizePathForComparison(child->second.info.parent_session_key) !=
                                                NormalizePathForComparison(journal_.Path())) {
            active_subagent_id_.clear();
        }
    }
    summary_show_all_ = false;
    RefreshSummaryPanel(true);
    ClearChatView(ChatViewReset::Loading);
    if (chat_view_ == nullptr) return;
    chat_view_->BeginRestore(session_load_started_tick_);
    RefreshSessionTitle(journal_.Path());
    SetTimer(window_, kSessionLoadTimer, 16, nullptr);
    UpdateSendButton();
    UpdateEmptyState();
    const std::shared_ptr<TurnRuntime> running = FindTurn(journal_.Path());
    const nlohmann::json& metadata = journal_.State().metadata;
    const nlohmann::json durations = metadata.contains("turn_durations_ms") && metadata["turn_durations_ms"].is_array()
                                         ? metadata["turn_durations_ms"]
                                         : nlohmann::json::array();
    const nlohmann::json usages = metadata.contains("turn_usages") && metadata["turn_usages"].is_array()
                                      ? metadata["turn_usages"]
                                      : nlohmann::json::array();
    const nlohmann::json compactions =
        metadata.contains("context_compactions") && metadata["context_compactions"].is_array()
            ? metadata["context_compactions"]
            : nlohmann::json::array();
    const nlohmann::json tool_durations =
        metadata.contains("tool_durations_ms") && metadata["tool_durations_ms"].is_object()
            ? metadata["tool_durations_ms"]
            : nlohmann::json::object();
    const nlohmann::json turn_errors = metadata.contains("turn_errors") && metadata["turn_errors"].is_array()
                                           ? metadata["turn_errors"]
                                           : nlohmann::json::array();
    const std::string current_session_key = NormalizePathForComparison(journal_.Path());
    const auto external_run_it = external_run_details_.find(current_session_key);
    bool external_running = false;
    uint64_t external_start_tick = 0;
    std::string external_phase;
    std::string external_output;
    std::string external_reasoning;
    std::string external_tool_name;
    std::string external_tool_arguments;
    if (external_run_it != external_run_details_.end() && external_run_it->second.is_object()) {
        const nlohmann::json& run = external_run_it->second;
        const std::string state = LowerAscii(Trim(run.value("state", std::string{})));
        external_running =
            state == "running" || state == "queued" || state == "awaiting_approval" || state == "cancelling";
        if (external_running) {
            const uint64_t started_at = run.value("started_at", uint64_t{0});
            const uint64_t now = UnixTimeSeconds();
            external_start_tick =
                started_at == 0 || started_at > now ? spark::MonotonicMillis() : spark::MonotonicMillis() - (now - started_at) * 1000;
            external_phase = run.value("phase", WideToUtf8(Text(L"Processing...")));
            external_output = run.value("output", std::string{});
            external_reasoning = run.value("reasoning", std::string{});
            external_tool_name = run.value("active_tool_name", std::string{});
            external_tool_arguments = run.value("active_tool_arguments", std::string{});
        }
    }
    const std::string session_permission = metadata.value("permission_mode", std::string{});
    if (session_permission == "ask" || session_permission == "approve_for_me" || session_permission == "full_access" ||
        session_permission == "read_only") {
        permission_mode_ = session_permission;
        if (permission_button_ != nullptr) {
            SetWindowTextW(permission_button_, PermissionLabel(permission_mode_).c_str());
        }
    }
    const bool paused_tail =
        running == nullptr && metadata.contains("paused_turn") && metadata["paused_turn"].is_object();
    const uint64_t paused_elapsed_ms =
        paused_tail ? metadata["paused_turn"].value("elapsed_ms", uint64_t{0}) : uint64_t{0};
    const auto& messages = journal_.State().messages;
    const size_t total_turns =
        static_cast<size_t>(std::count_if(messages.begin(), messages.end(), [](const ChatMessage& message) {
            return message.role == MessageRole::User;
        }));
    struct ReplayProgress {
        size_t message_index = 0;
        size_t turn_index = 0;
        size_t pending_turn_status = static_cast<size_t>(-1);
        size_t pending_turn_index = static_cast<size_t>(-1);
        uint64_t pending_turn_duration = 0;
        nlohmann::json pending_turn_usage;
        size_t compaction_index = 0;
        std::map<std::string, size_t> pending_tools;
    };
    auto progress = std::make_shared<ReplayProgress>();
    session_replay_step_ = [this, progress, running, durations, usages, compactions, tool_durations, turn_errors,
                            total_turns, paused_tail, paused_elapsed_ms, external_running, external_start_tick,
                            external_phase, external_output, external_reasoning, external_tool_name,
                            external_tool_arguments]() {
        const auto& transcript = journal_.State().messages;
        auto& pending_turn_status = progress->pending_turn_status;
        auto& pending_turn_index = progress->pending_turn_index;
        auto& pending_turn_duration = progress->pending_turn_duration;
        auto& pending_turn_usage = progress->pending_turn_usage;
        auto& turn_index = progress->turn_index;
        auto& compaction_index = progress->compaction_index;
        auto& pending_tools = progress->pending_tools;
        const auto tool_elapsed_ms = [&](const std::string& call_id) -> uint64_t {
            const auto found = tool_durations.find(call_id);
            return found == tool_durations.end() || !found->is_number_unsigned() ? 0 : found->get<uint64_t>();
        };
        const auto restore_turn_errors = [&](size_t restored_turn_index) {
            for (const auto& error : turn_errors) {
                if (!error.is_object()) continue;
                const int64_t error_turn_index = error.value("turn_index", int64_t{-1});
                if (error_turn_index < 0 || static_cast<size_t>(error_turn_index) != restored_turn_index) continue;
                const std::string text = error.value("error", std::string{});
                if (text.empty()) continue;
                const uint64_t timestamp = error.value("timestamp", uint64_t{0});
                if (IsQuotaExhaustedError(text)) chat_view_->AddQuotaNotice(timestamp);
                else chat_view_->AddError(text, timestamp);
            }
        };
        const auto finish_pending_turn = [&](bool paused = false) {
            if (pending_turn_status == static_cast<size_t>(-1)) return;
            if (paused) chat_view_->MarkTurnPaused(pending_turn_status, paused_elapsed_ms);
            else chat_view_->FinishTurnStatus(pending_turn_status, pending_turn_duration);
            if (pending_turn_index != static_cast<size_t>(-1)) restore_turn_errors(pending_turn_index);
            if (pending_turn_usage.is_object()) {
                chat_view_->AddUsageStats(pending_turn_usage.value("input_tokens", uint64_t{0}),
                                          pending_turn_usage.value("output_tokens", uint64_t{0}),
                                          pending_turn_usage.value("cached_input_tokens", uint64_t{0}),
                                          pending_turn_usage.value("model_calls", uint64_t{0}),
                                          pending_turn_usage.value("context_tokens", uint64_t{0}),
                                          pending_turn_usage.value("context_limit_tokens", uint64_t{0}),
                                          pending_turn_usage.value("elapsed_ms", uint64_t{0}));
            }
            pending_turn_status = static_cast<size_t>(-1);
            pending_turn_index = static_cast<size_t>(-1);
            pending_turn_usage = nlohmann::json{};
        };
        const auto emit_compactions_through = [&](size_t message_count) {
            while (compaction_index < compactions.size()) {
                const nlohmann::json& compaction = compactions[compaction_index];
                if (!compaction.is_object()) {
                    ++compaction_index;
                    continue;
                }
                if (compaction.value("message_count", uint64_t{0}) > message_count) break;
                const std::string trigger = compaction.value("trigger", std::string("auto"));
                chat_view_->AddCompactionNotice(trigger != "manual");
                ++compaction_index;
            }
        };
        // Yield between small batches so sidebar clicks, repaint and cancellation stay responsive.
        const uint64_t deadline = spark::MonotonicMillis() + 8;
        size_t restored = 0;
        for (; progress->message_index < transcript.size(); ++progress->message_index) {
            if (restored >= 64 || (restored > 0 && spark::MonotonicMillis() >= deadline)) return false;
            ++restored;
            const size_t message_index = progress->message_index;
            emit_compactions_through(message_index);
            const ChatMessage& message = transcript[message_index];
            if (message.role == MessageRole::User) {
                finish_pending_turn();
                chat_view_->AddUser(message.content, message.timestamp);
                const bool running_turn = (running != nullptr || external_running) && turn_index + 1 == total_turns;
                const uint64_t running_start_tick = running != nullptr ? running->started_tick : external_start_tick;
                pending_turn_status = chat_view_->AddTurnStatus(0, running_turn ? running_start_tick : 0);
                pending_turn_index = turn_index;
                pending_turn_duration = turn_index < durations.size() ? durations[turn_index].get<uint64_t>() : 0;
                pending_turn_usage =
                    !running_turn && turn_index < usages.size() ? usages[turn_index] : nlohmann::json{};
                ++turn_index;
            } else if (message.role == MessageRole::Assistant) {
                if (!message.content.empty()) {
                    const size_t index = chat_view_->AddAssistant(message.content, message.timestamp);
                    chat_view_->FinishAssistant(index, message.timestamp);
                }
                for (const ToolCall& call : message.tool_calls) {
                    const size_t index = chat_view_->AddTool(call.name, call.arguments_json, message.timestamp);
                    pending_tools[call.id] = index;
                }
            } else if (message.role == MessageRole::Tool) {
                const auto pending = pending_tools.find(message.tool_call_id);
                if (pending != pending_tools.end()) {
                    chat_view_->FinishTool(pending->second, true, message.content, message.timestamp,
                                           tool_elapsed_ms(message.tool_call_id));
                    pending_tools.erase(pending);
                } else {
                    const size_t index = chat_view_->AddTool(message.name, "{}", message.timestamp);
                    chat_view_->FinishTool(index, true, message.content, message.timestamp,
                                           tool_elapsed_ms(message.tool_call_id));
                }
            }
        }
        emit_compactions_through(transcript.size());
        if (running == nullptr && !external_running) {
            for (const auto& pending : pending_tools) {
                if (pending.second < chat_view_->Count()) {
                    chat_view_->FinishTool(pending.second, false,
                                           "Tool did not report completion before the session stopped.");
                }
            }
            pending_tools.clear();
        }
        if ((running != nullptr || external_running) && pending_turn_status != static_cast<size_t>(-1)) {
            active_turn_entry_ = pending_turn_status;
            turn_started_tick_ = running != nullptr ? running->started_tick : external_start_tick;
            if (running != nullptr) active_tool_entries_ = pending_tools;
        } else {
            finish_pending_turn(paused_tail);
        }
        if (external_running) {
            size_t live_reasoning_entry = static_cast<size_t>(-1);
            size_t live_assistant_entry = static_cast<size_t>(-1);
            if (!external_tool_name.empty()) {
                chat_view_->AddTool(external_tool_name, external_tool_arguments, 0);
            }
            if (external_output.empty() && !external_reasoning.empty()) {
                live_reasoning_entry = chat_view_->AddReasoning(external_reasoning, 0);
            } else if (!external_output.empty()) {
                live_assistant_entry = chat_view_->AddAssistant(external_output, 0);
            }
            active_reasoning_entry_ = live_reasoning_entry;
            latest_reasoning_entry_ = live_reasoning_entry;
            active_assistant_entry_ = live_assistant_entry;
            assistant_stream_started_ = live_assistant_entry != static_cast<size_t>(-1);
            current_phase_ = AgentPhase::WaitingModel;
            phase_text_ = external_phase.empty() ? std::wstring{} : Utf8ToWide(external_phase);
            reasoning_preview_ = Utf8ToWide(external_reasoning);
            if (live_reasoning_entry == static_cast<size_t>(-1) && live_assistant_entry == static_cast<size_t>(-1) &&
                external_tool_name.empty() && !phase_text_.empty()) {
                chat_view_->SetActivityState(true, phase_text_, turn_started_tick_);
            }
        } else if (running == nullptr) {
            active_turn_entry_ = static_cast<size_t>(-1);
        }
        chat_view_->EndRestore();
        LayoutControls();
        return true;
    };
}

std::string AppWindow::ComposerStatePath() const {
    if (pending_new_conversation_) return PendingComposerStatePath();
    const std::string id = journal_.State().id;
    if (id.empty()) return {};
    return JoinPath(JoinPath(config_.storage.data_dir, "drafts"), id + ".json");
}

std::string AppWindow::PendingComposerStatePath() const {
    const std::string scope = active_session_scope_ == "project" ? "project" : "recent";
    const std::string workspace = NormalizePathForComparison(
        active_workspace_path_.empty() ? config_.workspace.path : active_workspace_path_);
    const std::string seed = scope + "\n" + workspace + "\n" + active_project_name_;
    uint64_t hash = 14695981039346656037ull;
    for (const unsigned char character : seed) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    std::ostringstream name;
    name << "pending-" << scope << "-" << std::hex << hash << ".json";
    return JoinPath(JoinPath(config_.storage.data_dir, "drafts"), name.str());
}

void AppWindow::SaveComposerState() {
    if (SessionLoading()) return;
    const std::string path = ComposerStatePath();
    if (path.empty() || composer_ == nullptr) return;
    std::error_code directory_error;
    spark::fs::create_directories(Utf8ToWide(ParentPath(path)), directory_error);
    const nlohmann::json state = {
        {"draft", ComposerText()},
        {"goal", goal_},
        {"plan_mode", plan_mode_},
        {"attachments", attachments_},
        {"snapshot_contexts", snapshot_contexts_},
    };
    std::string write_error;
    WriteFileUtf8Atomic(path, state.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), &write_error);
}

void AppWindow::ClearComposerState() {
    snapshot_capture_.Cancel();
    KillTimer(window_, kSnapshotPollTimer);
    composer_dirty_ = false;
    attachments_.clear();
    snapshot_contexts_.clear();
    SaveComposerState();
}

void AppWindow::RestoreComposerState() {
    composer_dirty_ = false;
    goal_.clear();
    plan_mode_ = false;
    attachments_.clear();
    snapshot_contexts_.clear();
    std::string draft;
    const std::string path = ComposerStatePath();
    if (!path.empty()) {
        std::string read_error;
        const auto contents = ReadFileUtf8(path, &read_error);
        if (contents.has_value()) {
            try {
                const nlohmann::json state = nlohmann::json::parse(*contents);
                draft = state.value("draft", std::string{});
                goal_ = state.value("goal", std::string{});
                plan_mode_ = state.value("plan_mode", false);
                const auto snapshots = state.find("snapshot_contexts");
                if (snapshots != state.end() && snapshots->is_object()) {
                    for (auto item = snapshots->begin(); item != snapshots->end() && snapshot_contexts_.size() < 64; ++item) {
                        if (item.value().is_string()) snapshot_contexts_[item.key()] =
                            TruncateUtf8(item.value().get<std::string>(), 2048);
                    }
                }
                const auto items = state.find("attachments");
                if (items != state.end() && items->is_array()) {
                    for (const auto& entry : *items) {
                        if (entry.is_string() && attachments_.size() < 64) attachments_.push_back(entry.get<std::string>());
                    }
                }
            } catch (const nlohmann::json::exception&) {
            }
        }
    }
    SetWindowTextW(composer_, Utf8ToWide(draft).c_str());
    UpdateSendButton();
    LayoutControls();
    InvalidateRect(window_, nullptr, FALSE);
}

std::string AppWindow::ReadActiveSessionPath() const {
    std::string read_error;
    const auto contents = ReadFileUtf8(JoinPath(config_.storage.data_dir, "ui-state.json"), &read_error);
    if (!contents.has_value()) return {};
    try {
        const nlohmann::json state = nlohmann::json::parse(*contents);
        return state.value("active_session", std::string{});
    } catch (const nlohmann::json::exception&) {
        return {};
    }
}

void AppWindow::WriteActiveSessionPath() const {
    if (pending_new_conversation_) return;
    if (journal_.Path().empty()) return;
    const nlohmann::json state = {{"active_session", journal_.Path()}};
    std::string write_error;
    WriteFileUtf8Atomic(JoinPath(config_.storage.data_dir, "ui-state.json"),
                        state.dump(2, ' ', false, nlohmann::json::error_handler_t::replace), &write_error);
}

std::string AppWindow::DataDirectory() const { return config_.storage.data_dir; }

int AppWindow::Dpi(int pixels) const { return ScaleForSystemDpi(pixels); }

HBRUSH AppWindow::DarkBrush(COLORREF color) { return CachedBrush(color); }

LRESULT CALLBACK AppWindow::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    AppWindow* self = reinterpret_cast<AppWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<AppWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->window_ = window;
    }
    return self != nullptr ? self->HandleMessage(message, wparam, lparam) : DefWindowProcW(window, message, wparam, lparam);
}

bool AppWindow::RouteMouseWheelToChat(WPARAM wparam, LPARAM lparam) {
    const POINT screen_point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const HWND target = WindowFromPoint(screen_point);
    for (HWND view : {side_chat_view_ != nullptr ? side_chat_view_->hwnd() : nullptr,
                      summary_panel_ != nullptr ? summary_panel_->hwnd() : nullptr,
                      chat_view_ != nullptr ? chat_view_->hwnd() : nullptr}) {
        if (view == nullptr || !IsWindowVisible(view)) continue;
        if (target != view && !IsChild(view, target)) continue;
        SendMessageW(view, WM_MOUSEWHEEL, wparam, lparam);
        return true;
    }
    return false;
}

LRESULT CALLBACK AppWindow::ComposerSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<AppWindow*>(reference_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_PASTE && self->AttachClipboardImage()) return 0;
    if (message == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (wparam == ',') {
            self->OpenSettingsPage();
            return 0;
        }
        if (wparam == 'L') {
            SetFocus(self->composer_);
            return 0;
        }
    }
    if (message == WM_KEYDOWN && wparam == VK_ESCAPE && self->IsActiveSessionRunning()) {
        self->InterruptTurn();
        return 0;
    }
    if (message == WM_MOUSEWHEEL && self->RouteMouseWheelToChat(wparam, lparam)) return 0;
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool send_with_enter = self->config_.ui.send_key != "ctrl+enter";
        const bool should_send = send_with_enter ? !shift : control;
        bool composing = false;
        HIMC context = ImmGetContext(window);
        if (context != nullptr) {
            composing = ImmGetOpenStatus(context) != FALSE && ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
            ImmReleaseContext(window, context);
        }
        if (should_send && !composing) {
            self->ToggleSendAction();
            return 0;
        }
    }
    if (message == WM_CHAR && (wparam == VK_RETURN || wparam == L'\n')) {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool send_with_enter = self->config_.ui.send_key != "ctrl+enter";
        const bool should_send = send_with_enter ? !shift : control;
        if (!should_send) return DefSubclassProc(window, message, wparam, lparam);
        HIMC context = ImmGetContext(window);
        const bool composing = context != nullptr && ImmGetCompositionStringW(context, GCS_COMPSTR, nullptr, 0) > 0;
        if (context != nullptr) ImmReleaseContext(window, context);
        if (!composing) return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK AppWindow::HistorySubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<AppWindow*>(reference_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) {
        // The default list box class paints its background with COLOR_WINDOW, which flashes white
        // on Win7 before the owner-drawn rows are blitted.
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, CachedBrush(kSidebar));
        return 1;
    }
    if (message == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->history_paths_.size()) +
                                                       (self->history_more_index_ == static_cast<size_t>(-1) ? 0 : 1));
        if (index != self->hovered_history_item_) {
            self->hovered_history_item_ = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    if (message == WM_MOUSELEAVE) {
        self->hovered_history_item_ = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (delta != 0) self->ScrollSidebarBy(delta > 0 ? -3 : 3);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        self->pending_history_action_path_.clear();
        self->pending_history_action_ = 0;
        self->pending_history_more_ = false;
        self->pending_history_click_path_.clear();
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int count = static_cast<int>(self->history_paths_.size()) +
                          (self->history_more_index_ == static_cast<size_t>(-1) ? 0 : 1);
        const int index = ListItemAtPoint(window, x, y, count);
        if (index >= 0) {
            if (index == static_cast<int>(self->history_more_index_)) {
                self->pending_history_more_ = true;
                self->pending_history_action_path_.clear();
                self->pending_history_action_ = 0;
                SetCapture(window);
                return 0;
            }
            RECT rect{};
            SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect));
            const int action = SidebarChatActionForX(x, rect);
            if (action != 0) {
                if (index < static_cast<int>(self->history_paths_.size())) {
                    self->pending_history_action_path_ = self->history_paths_[index];
                    self->pending_history_action_ = action;
                    self->pending_history_more_ = false;
                }
                SetCapture(window);
                return 0;
            }
            if (index < static_cast<int>(self->history_paths_.size())) {
                self->pending_history_click_path_ = self->history_paths_[index];
            }
        }
    }
    if (message == WM_LBUTTONUP) {
        if (GetCapture() == window) ReleaseCapture();
        if (self->pending_history_more_) {
            self->pending_history_more_ = false;
            self->recent_show_all_ = true;
            self->RefreshHistory();
            return 0;
        }
        if (self->pending_history_action_ != 0 && !self->pending_history_action_path_.empty()) {
            const std::string path = self->pending_history_action_path_;
            const int action = self->pending_history_action_;
            self->pending_history_action_path_.clear();
            self->pending_history_action_ = 0;
            self->HandleSessionAction(path, action);
            return 0;
        }
        if (!self->pending_history_click_path_.empty()) {
            RECT client{};
            GetClientRect(window, &client);
            const int release_x = GET_X_LPARAM(lparam);
            const int release_y = GET_Y_LPARAM(lparam);
            const std::string path = self->pending_history_click_path_;
            self->pending_history_click_path_.clear();
            if (release_x >= client.left && release_x < client.right && release_y >= client.top &&
                release_y < client.bottom) {
                // A single click switches conversations; the double-click handler below stays as
                // a fallback for keyboard-driven selection.
                self->PostSessionActivation(path);
                return 0;
            }
        }
    }
    if (message == WM_RBUTTONUP) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->history_paths_.size()));
        if (index >= 0 && index < static_cast<int>(self->history_paths_.size())) {
            POINT anchor{x, y};
            ClientToScreen(window, &anchor);
            self->ShowChatContextMenu(anchor, self->history_paths_[index]);
            return 0;
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK AppWindow::SidebarListSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                    UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<AppWindow*>(reference_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) {
        // Match the sidebar instead of the list box class's white window background.
        RECT client{};
        GetClientRect(window, &client);
        FillRect(reinterpret_cast<HDC>(wparam), &client, CachedBrush(kSidebar));
        return 1;
    }
    if (message == WM_MOUSEMOVE) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->project_rows_.size()));
        if (index != self->hovered_project_item_) {
            self->hovered_project_item_ = index;
            InvalidateRect(window, nullptr, FALSE);
        }
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        return 0;
    }
    if (message == WM_MOUSELEAVE) {
        self->hovered_project_item_ = -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (delta != 0) self->ScrollSidebarBy(delta > 0 ? -3 : 3);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        self->pending_project_action_path_.clear();
        self->pending_project_action_ = 0;
        self->pending_project_more_ = false;
        self->pending_project_click_path_.clear();
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->project_rows_.size()));
        if (index >= 0 && index < static_cast<int>(self->project_rows_.size())) {
            RECT rect{};
            SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect));
            const SidebarRow& row = self->project_rows_[index];
            if (row.kind == SidebarRowKind::Project && x >= rect.right - ScaleForSystemDpi(64)) {
                if (x >= rect.right - ScaleForSystemDpi(34)) self->NewConversationInProject(row.project);
                else {
                    POINT anchor{rect.right - ScaleForSystemDpi(12), rect.bottom};
                    ClientToScreen(window, &anchor);
                    self->ShowProjectMenu(anchor, row.project);
                }
                return 0;
            }
            if (row.kind == SidebarRowKind::More) {
                self->pending_project_more_ = true;
                self->pending_project_action_path_.clear();
                self->pending_project_action_ = 0;
                SetCapture(window);
                return 0;
            }
            if (row.kind == SidebarRowKind::Project) return 0;
            const int action = row.kind == SidebarRowKind::Chat ? SidebarChatActionForX(x, rect) : 0;
            if (action != 0) {
                self->pending_project_action_path_ = row.session_path;
                self->pending_project_action_ = action;
                self->pending_project_more_ = false;
                SetCapture(window);
                return 0;
            }
            if (row.kind == SidebarRowKind::Chat && !row.session_path.empty()) {
                self->pending_project_click_path_ = row.session_path;
            }
        }
    }
    if (message == WM_LBUTTONUP) {
        if (GetCapture() == window) ReleaseCapture();
        if (self->pending_project_more_) {
            self->pending_project_more_ = false;
            const int x = GET_X_LPARAM(lparam);
            const int y = GET_Y_LPARAM(lparam);
            const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->project_rows_.size()));
            if (index >= 0 && index < static_cast<int>(self->project_rows_.size()) &&
                self->project_rows_[index].kind == SidebarRowKind::More) {
                const size_t project = self->project_rows_[index].project;
                if (project < self->projects_.size()) {
                    self->project_show_all_[NormalizePathForComparison(self->projects_[project].workspace)] = true;
                    self->RefreshProjectRows();
                }
            }
            return 0;
        }
        if (self->pending_project_action_ != 0 && !self->pending_project_action_path_.empty()) {
            const std::string path = self->pending_project_action_path_;
            const int action = self->pending_project_action_;
            self->pending_project_action_path_.clear();
            self->pending_project_action_ = 0;
            self->HandleSessionAction(path, action);
            return 0;
        }
        if (!self->pending_project_click_path_.empty()) {
            RECT client{};
            GetClientRect(window, &client);
            const int release_x = GET_X_LPARAM(lparam);
            const int release_y = GET_Y_LPARAM(lparam);
            const std::string path = self->pending_project_click_path_;
            self->pending_project_click_path_.clear();
            if (release_x >= client.left && release_x < client.right && release_y >= client.top &&
                release_y < client.bottom) {
                self->PostSessionActivation(path);
                return 0;
            }
        }
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->project_rows_.size()));
        if (index >= 0 && index < static_cast<int>(self->project_rows_.size())) {
            const SidebarRow& row = self->project_rows_[index];
            if (row.kind == SidebarRowKind::More) {
                return 0;
            }
            if (row.kind == SidebarRowKind::Project) {
                RECT rect{};
                SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect));
                if (x < rect.right - ScaleForSystemDpi(64)) {
                    self->ToggleProjectExpanded(row.project);
                    return 0;
                }
            }
            if (row.kind == SidebarRowKind::Chat && !row.session_path.empty()) {
                RECT rect{};
                SendMessageW(window, LB_GETITEMRECT, index, reinterpret_cast<LPARAM>(&rect));
                const int action = SidebarChatActionForX(x, rect);
                if (action != 0) {
                    self->HandleSessionAction(row.session_path, action);
                    return 0;
                }
            }
        }
    }
    if (message == WM_RBUTTONUP) {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        const int index = ListItemAtPoint(window, x, y, static_cast<int>(self->project_rows_.size()));
        if (index >= 0 && index < static_cast<int>(self->project_rows_.size())) {
            const SidebarRow& row = self->project_rows_[index];
            if (row.kind == SidebarRowKind::Chat && !row.session_path.empty()) {
                POINT anchor{x, y};
                ClientToScreen(window, &anchor);
                self->ShowChatContextMenu(anchor, row.session_path);
                return 0;
            }
        }
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK AppWindow::SidebarScrollbarProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                 UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<AppWindow*>(reference_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_NCHITTEST) return HTCLIENT;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(device, &client, CachedBrush(kSidebar));
        const CombinedSidebarMetrics metrics = self->MeasureSidebarScrollbar();
        if (metrics.needed) {
            const int right = client.right - ScaleForSystemDpi(3);
            const int left = std::max(static_cast<int>(client.left), right - ScaleForSystemDpi(4));
            RECT track{left, metrics.track_top, right, metrics.track_bottom};
            FillRect(device, &track, CachedBrush(RGB(55, 56, 60)));
            POINT cursor{};
            GetCursorPos(&cursor);
            ScreenToClient(window, &cursor);
            const bool hovered = cursor.x >= client.left && cursor.x <= client.right && cursor.y >= metrics.thumb_top &&
                                 cursor.y <= metrics.thumb_bottom;
            RECT thumb{left, metrics.thumb_top, right, metrics.thumb_bottom};
            FillRect(device, &thumb, CachedBrush(hovered || self->sidebar_scrollbar_dragging_ ? kRailHover : kRailIdle));
        }
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    }
    if (message == WM_MOUSEMOVE) {
        if (self->sidebar_scrollbar_dragging_) {
            self->ScrollSidebarToPoint(GET_Y_LPARAM(lparam));
            return 0;
        }
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        if (delta != 0) self->ScrollSidebarBy(delta > 0 ? -3 : 3);
        return 0;
    }
    if (message == WM_LBUTTONDOWN) {
        self->sidebar_scrollbar_dragging_ = true;
        SetCapture(window);
        const CombinedSidebarMetrics metrics = self->MeasureSidebarScrollbar();
        const int y = GET_Y_LPARAM(lparam);
        if (y < metrics.thumb_top || y > metrics.thumb_bottom) self->ScrollSidebarToPoint(y);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_LBUTTONUP) {
        self->sidebar_scrollbar_dragging_ = false;
        if (GetCapture() == window) ReleaseCapture();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    if (message == WM_CAPTURECHANGED) {
        self->sidebar_scrollbar_dragging_ = false;
        InvalidateRect(window, nullptr, FALSE);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK AppWindow::ModelComboSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                   UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<AppWindow*>(reference_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(device, &client, CachedBrush(kPanel));
        wchar_t text[512]{};
        const int selected = static_cast<int>(SendMessageW(window, CB_GETCURSEL, 0, 0));
        if (selected >= 0) {
            SendMessageW(window, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text));
        }
        SetBkMode(device, TRANSPARENT);
        SetTextColor(device, IsWindowEnabled(window) ? kText : kMuted);
        HFONT old_font = static_cast<HFONT>(SelectObject(device, self->ui_font_));
        RECT text_rect = client;
        text_rect.left += 6;
        text_rect.right -= 24;
        DrawTextW(device, text, -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(device, old_font);
        const int center_y = (client.top + client.bottom) / 2;
        HPEN chevron = CreatePen(PS_SOLID, 1, kMuted);
        HGDIOBJ old_pen = SelectObject(device, chevron);
        MoveToEx(device, client.right - 17, center_y - 2, nullptr);
        LineTo(device, client.right - 13, center_y + 2);
        LineTo(device, client.right - 9, center_y - 2);
        SelectObject(device, old_pen);
        DeleteObject(chevron);
        EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS) {
        InvalidateRect(window, nullptr, FALSE);
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK AppWindow::StatusLabelSubclassProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                                    UINT_PTR subclass_id, DWORD_PTR reference_data) {
    auto* self = reinterpret_cast<AppWindow*>(reference_data);
    if (self == nullptr) return DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{};
        HDC device = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(device, &client, self->background_brush_);
        SetBkMode(device, TRANSPARENT);
        int text_left = client.left;
        if (self->IsActiveSessionRunning()) {
            const int center_y = (client.top + client.bottom) / 2;
            DrawActivityGlyph(device, client.left + ScaleForSystemDpi(7), center_y, ScaleForSystemDpi(5),
                              self->activity_frame_, kText, kSubtle);
            text_left += ScaleForSystemDpi(20);
        }
        const int text_length = std::max(0, GetWindowTextLengthW(window));
        std::wstring text(static_cast<size_t>(text_length) + 1, L'\0');
        if (text_length > 0) GetWindowTextW(window, text.data(), text_length + 1);
        text.resize(static_cast<size_t>(text_length));
        SetTextColor(device, kMuted);
        HFONT old_font = static_cast<HFONT>(SelectObject(device, self->ui_font_));
        RECT text_rect{text_left, client.top, client.right, client.bottom};
        DrawTextW(device, text.c_str(), -1, &text_rect,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(device, old_font);
        EndPaint(window, &paint);
        return 0;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT AppWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == TaskbarCreatedMessage()) {
        tray_icon_added_ = false;
        AddTrayIcon();
        return 0;
    }
    switch (message) {
        case WM_CREATE:
            CreateControls();
            LoadOrCreateConfiguration();
            SetTimer(window_, kComposerTimer, 10000, nullptr);
            SetTimer(window_, kExternalPollTimer, 1000, nullptr);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            const auto work = WindowsWindowWorkArea(window_);
            const auto minimum = FitWindowSize({Dpi(760), Dpi(520)},
                {work.right - work.left, work.bottom - work.top}, {1, 1}, Dpi(12));
            info->ptMinTrackSize.x = minimum.width;
            info->ptMinTrackSize.y = minimum.height;
            return 0;
        }
        case WM_SIZE:
            LayoutControls();
            if (settings_.IsOpen()) settings_.Resize();
            return 0;
        case WM_MOUSEWHEEL:
            if (RouteMouseWheelToChat(wparam, lparam)) return 0;
            break;
        case WM_LBUTTONUP: {
            const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            for (const auto& hit : attachment_hit_areas_) {
                if (!PtInRect(&hit.preview, point)) continue;
                if (!hit.path.empty() && PtInRect(&hit.remove, point)) {
                    const std::string path = hit.path;
                    const auto attachment = std::find(attachments_.begin(), attachments_.end(), path);
                    if (attachment != attachments_.end()) attachments_.erase(attachment);
                    snapshot_contexts_.erase(path);
                    const auto bitmap = attachment_bitmaps_.find(path);
                    if (bitmap != attachment_bitmaps_.end()) {
                        if (bitmap->second != nullptr) DeleteObject(bitmap->second);
                        attachment_bitmaps_.erase(bitmap);
                    }
                    attachment_hit_areas_.clear();
                    SaveComposerState();
                    LayoutControls();
                    UpdateSendButton();
                    InvalidateRect(window_, nullptr, FALSE);
                } else if (!hit.path.empty()) {
                    OpenAttachment(hit.path);
                }
                return 0;
            }
            break;
        }
        case kSettingsClosedMessage:
            SetSettingsMode(false);
            return 0;
        case kEmptyStateRefreshMessage:
            if (empty_mark_ != nullptr && IsWindowVisible(empty_mark_)) {
                RedrawWindow(empty_mark_, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
            if (empty_title_ != nullptr && IsWindowVisible(empty_title_)) {
                RedrawWindow(empty_title_, nullptr, nullptr,
                             RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
            }
            return 0;
        case kSingleInstanceMessage:
            RestoreFromTray();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PRINTCLIENT:
        case WM_PAINT: {
            const bool printing = message == WM_PRINTCLIENT;
            PAINTSTRUCT paint{};
            HDC target = printing ? reinterpret_cast<HDC>(wparam) : BeginPaint(window_, &paint);
            RECT client{};
            GetClientRect(window_, &client);
            HDC buffer = CreateCompatibleDC(target);
            HBITMAP bitmap = buffer != nullptr
                ? CreateCompatibleBitmap(target, std::max<LONG>(1, client.right), std::max<LONG>(1, client.bottom))
                : nullptr;
            HGDIOBJ previous_bitmap = bitmap != nullptr ? SelectObject(buffer, bitmap) : nullptr;
            HDC device = bitmap != nullptr ? buffer : target;
            FillRect(device, &client, background_brush_);
            const LayoutMetrics metrics = MeasureLayout();
            RECT sidebar_rect{0, 0, metrics.sidebar, client.bottom};
            FillRect(device, &sidebar_rect, panel_brush_);
            if (side_chat_open_ && metrics.side_width > 0) {
                RECT side_rect{metrics.side_left, 0, client.right, client.bottom};
                FillRect(device, &side_rect, DarkBrush(kPanel));
            }
            HPEN divider = CreatePen(PS_SOLID, 1, kBorder);
            HGDIOBJ old_pen = SelectObject(device, divider);
            MoveToEx(device, metrics.sidebar, 0, nullptr);
            LineTo(device, metrics.sidebar, client.bottom);
            MoveToEx(device, metrics.main_x, metrics.topbar_height, nullptr);
            LineTo(device, metrics.main_right, metrics.topbar_height);
            if (side_chat_open_ && metrics.side_width > 0) {
                MoveToEx(device, metrics.side_left, 0, nullptr);
                LineTo(device, metrics.side_left, client.bottom);
                MoveToEx(device, metrics.side_left, metrics.topbar_height, nullptr);
                LineTo(device, client.right, metrics.topbar_height);
            }
            SelectObject(device, old_pen);
            DeleteObject(divider);

            RECT composer_panel{metrics.panel_left, metrics.panel_top, metrics.panel_right, metrics.panel_bottom};
            HPEN panel_border = CreatePen(PS_SOLID, 1, kBorder);
            old_pen = SelectObject(device, panel_border);
            HGDIOBJ old_brush = SelectObject(device, DarkBrush(kPanel));
            RoundRect(device, composer_panel.left, composer_panel.top, composer_panel.right, composer_panel.bottom,
                      14, 14);
            SelectObject(device, old_brush);
            SelectObject(device, old_pen);
            DeleteObject(panel_border);
            DrawAttachmentPreview(device, metrics);
            if (bitmap != nullptr) {
                const RECT redraw = printing ? client : paint.rcPaint;
                BitBlt(target, redraw.left, redraw.top, redraw.right - redraw.left, redraw.bottom - redraw.top,
                       device, redraw.left, redraw.top, SRCCOPY);
                SelectObject(buffer, previous_bitmap);
                DeleteObject(bitmap);
            }
            if (buffer != nullptr) DeleteDC(buffer);
            if (!printing) EndPaint(window_, &paint);
            return 0;
        }
        case WM_CTLCOLORSTATIC: {
            HDC device = reinterpret_cast<HDC>(wparam);
            HWND control = reinterpret_cast<HWND>(lparam);
            // A disabled EDIT sends WM_CTLCOLORSTATIC instead of WM_CTLCOLOREDIT.
            if (control == composer_) {
                SetBkMode(device, OPAQUE);
                SetTextColor(device, kText);
                SetBkColor(device, kPanel);
                return reinterpret_cast<LRESULT>(DarkBrush(kPanel));
            }
            SetBkMode(device, TRANSPARENT);
            if (control == empty_mark_ || control == empty_title_) {
                SetTextColor(device, control == empty_mark_ ? kMuted : kText);
                SetBkColor(device, kBackground);
                return reinterpret_cast<LRESULT>(background_brush_);
            }
            const bool sidebar_control =
                control == sidebar_title_ || control == project_label_ || control == recent_label_ ||
                control == sidebar_scrollbar_;
            const bool panel_control = control == workspace_label_ || control == model_label_ ||
                                       control == side_title_label_;
            SetTextColor(device, control == workspace_label_ || control == status_label_ || control == context_label_ ? kMuted : kText);
            SetBkColor(device, sidebar_control ? kSidebar : panel_control ? kPanel : kBackground);
            return reinterpret_cast<LRESULT>(sidebar_control ? panel_brush_ : panel_control ? DarkBrush(kPanel) : background_brush_);
        }
        case WM_CTLCOLOREDIT: {
            HDC device = reinterpret_cast<HDC>(wparam);
            SetTextColor(device, kText);
            SetBkColor(device, kPanel);
            return reinterpret_cast<LRESULT>(DarkBrush(kPanel));
        }
        case WM_CTLCOLORLISTBOX: {
            HDC device = reinterpret_cast<HDC>(wparam);
            SetTextColor(device, kText);
            SetBkColor(device, kSidebar);
            return reinterpret_cast<LRESULT>(panel_brush_);
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_MENU) {
                if (item->itemID >= kModelHeaderBase && item->itemID < kModelItemBase) {
                    item->itemWidth = 360;
                    item->itemHeight = 30;
                    return TRUE;
                }
                if (model_menu_targets_.find(static_cast<UINT>(item->itemID)) != model_menu_targets_.end()) {
                    item->itemWidth = 360;
                    item->itemHeight = 36;
                    return TRUE;
                }
                if (reasoning_menu_targets_.find(static_cast<UINT>(item->itemID)) != reasoning_menu_targets_.end() ||
                    item->itemID == kModelReasoningEntry) {
                    item->itemWidth = 286;
                    item->itemHeight = 38;
                    return TRUE;
                }
                if (item->itemID == kPermissionAsk || item->itemID == kPermissionApprove ||
                    item->itemID == kPermissionFull) {
                    item->itemWidth = 468;
                    item->itemHeight = 62;
                    return TRUE;
                }
                const auto text = menu_item_texts_.find(static_cast<UINT>(item->itemID));
                const std::wstring label = text != menu_item_texts_.end() ? text->second : std::wstring();
                HDC device = GetDC(window_);
                HFONT old_font = static_cast<HFONT>(SelectObject(device, ui_font_));
                SIZE text_size{};
                GetTextExtentPoint32W(device, label.c_str(), static_cast<int>(label.size()), &text_size);
                SelectObject(device, old_font);
                ReleaseDC(window_, device);
                const bool submenu = menu_submenu_items_.find(static_cast<UINT>(item->itemID)) != menu_submenu_items_.end();
                item->itemWidth = static_cast<UINT>(text_size.cx + (submenu ? 68 : 52));
                item->itemHeight = 36;
                return TRUE;
            }
            if (item->CtlType == ODT_COMBOBOX && item->CtlID == kModelCombo) item->itemHeight = 28;
            return TRUE;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            const bool disabled = (item->itemState & ODS_DISABLED) != 0;
            if (item->CtlType == ODT_MENU) {
                const bool grayed = (item->itemState & ODS_GRAYED) != 0;
                if (item->itemID >= kModelHeaderBase && item->itemID < kModelItemBase) {
                    FillRect(item->hDC, &item->rcItem, CachedBrush(kPanel));
                    SetBkMode(item->hDC, TRANSPARENT);
                    HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                    SetTextColor(item->hDC, kMuted);
                    RECT text_rect = item->rcItem;
                    text_rect.left += 18;
                    text_rect.right -= 16;
                    DrawTextW(item->hDC, menu_item_texts_[item->itemID].c_str(), -1, &text_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(item->hDC, old_font);
                    return TRUE;
                }
                const auto model_target = model_menu_targets_.find(static_cast<UINT>(item->itemID));
                if (model_target != model_menu_targets_.end()) {
                    const bool selected = (item->itemState & ODS_CHECKED) != 0;
                    FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kPanel));
                    SetBkMode(item->hDC, TRANSPARENT);
                    const std::wstring title = Utf8ToWide(model_target->second.model);
                    HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                    SetTextColor(item->hDC, kText);
                    RECT title_rect = item->rcItem;
                    title_rect.left += 18;
                    title_rect.right -= selected ? 38 : 16;
                    DrawTextW(item->hDC, title.c_str(), -1, &title_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(item->hDC, old_font);
                    if (selected) {
                        HPEN check = CreatePen(PS_SOLID, 2, kText);
                        HGDIOBJ old_pen = SelectObject(item->hDC, check);
                        const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                        const int check_x = item->rcItem.right - 25;
                        MoveToEx(item->hDC, check_x - 4, center_y, nullptr);
                        LineTo(item->hDC, check_x, center_y + 4);
                        LineTo(item->hDC, check_x + 7, center_y - 6);
                        SelectObject(item->hDC, old_pen);
                        DeleteObject(check);
                    }
                    return TRUE;
                }
                if (reasoning_menu_targets_.find(static_cast<UINT>(item->itemID)) != reasoning_menu_targets_.end() ||
                    item->itemID == kModelReasoningEntry) {
                    FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kPanel));
                    SetBkMode(item->hDC, TRANSPARENT);
                    HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                    SetTextColor(item->hDC, kText);
                    RECT text_rect = item->rcItem;
                    text_rect.left += 18;
                    text_rect.right -= 36;
                    DrawTextW(item->hDC, menu_item_texts_[item->itemID].c_str(), -1, &text_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(item->hDC, old_font);
                    if ((item->itemState & ODS_CHECKED) != 0) {
                        HPEN check = CreatePen(PS_SOLID, 2, kText);
                        HGDIOBJ old_pen = SelectObject(item->hDC, check);
                        const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                        const int check_x = item->rcItem.right - 25;
                        MoveToEx(item->hDC, check_x - 4, center_y, nullptr);
                        LineTo(item->hDC, check_x, center_y + 4);
                        LineTo(item->hDC, check_x + 7, center_y - 6);
                        SelectObject(item->hDC, old_pen);
                        DeleteObject(check);
                    }
                    return TRUE;
                }
                if (item->itemID == kPermissionAsk || item->itemID == kPermissionApprove ||
                    item->itemID == kPermissionFull) {
                    const bool selected = (item->itemState & ODS_CHECKED) != 0;
                    FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kPanel));
                    SetBkMode(item->hDC, TRANSPARENT);
                    const int icon_center_x = item->rcItem.left + 28;
                    const int icon_center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                    HPEN icon_pen = CreatePen(PS_SOLID, 2,
                                              item->itemID == kPermissionFull ? kWarning : kText);
                    HGDIOBJ old_pen = SelectObject(item->hDC, icon_pen);
                    HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
                    if (item->itemID == kPermissionAsk) {
                        MoveToEx(item->hDC, icon_center_x - 7, icon_center_y + 6, nullptr);
                        LineTo(item->hDC, icon_center_x - 7, icon_center_y - 1);
                        LineTo(item->hDC, icon_center_x - 2, icon_center_y - 6);
                        LineTo(item->hDC, icon_center_x + 4, icon_center_y - 6);
                        LineTo(item->hDC, icon_center_x + 7, icon_center_y - 3);
                        LineTo(item->hDC, icon_center_x + 7, icon_center_y + 6);
                    } else if (item->itemID == kPermissionApprove) {
                        MoveToEx(item->hDC, icon_center_x, icon_center_y - 8, nullptr);
                        LineTo(item->hDC, icon_center_x + 8, icon_center_y - 4);
                        LineTo(item->hDC, icon_center_x + 6, icon_center_y + 5);
                        LineTo(item->hDC, icon_center_x, icon_center_y + 8);
                        LineTo(item->hDC, icon_center_x - 6, icon_center_y + 5);
                        LineTo(item->hDC, icon_center_x - 8, icon_center_y - 4);
                        LineTo(item->hDC, icon_center_x, icon_center_y - 8);
                        MoveToEx(item->hDC, icon_center_x - 4, icon_center_y, nullptr);
                        LineTo(item->hDC, icon_center_x - 1, icon_center_y + 3);
                        LineTo(item->hDC, icon_center_x + 5, icon_center_y - 4);
                    } else {
                        Ellipse(item->hDC, icon_center_x - 8, icon_center_y - 8, icon_center_x + 8,
                                icon_center_y + 8);
                        MoveToEx(item->hDC, icon_center_x, icon_center_y - 4, nullptr);
                        LineTo(item->hDC, icon_center_x, icon_center_y + 1);
                        SetPixel(item->hDC, icon_center_x, icon_center_y + 4, kWarning);
                    }
                    SelectObject(item->hDC, old_brush);
                    SelectObject(item->hDC, old_pen);
                    DeleteObject(icon_pen);

                    HFONT old_title_font = static_cast<HFONT>(SelectObject(item->hDC, brand_font_));
                    SetTextColor(item->hDC, kText);
                    RECT title_rect = item->rcItem;
                    title_rect.left += 54;
                    title_rect.right -= 34;
                    title_rect.top += 8;
                    title_rect.bottom = title_rect.top + 25;
                    DrawTextW(item->hDC, menu_item_texts_[item->itemID].c_str(), -1, &title_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(item->hDC, old_title_font);

                    std::wstring description;
                    if (item->itemID == kPermissionAsk) description = i18n::Text(L"Ask before editing external files or using the internet");
                    else if (item->itemID == kPermissionApprove) description = i18n::Text(L"Ask only for operations with detected risks");
                    else description = i18n::Text(L"Allow internet access and access to files on this computer");
                    HFONT old_description_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                    SetTextColor(item->hDC, selected ? kText : kMuted);
                    RECT description_rect = item->rcItem;
                    description_rect.left += 54;
                    description_rect.right -= 34;
                    description_rect.top += 30;
                    description_rect.bottom -= 4;
                    DrawTextW(item->hDC, description.c_str(), -1, &description_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(item->hDC, old_description_font);

                    if (selected) {
                        HPEN check = CreatePen(PS_SOLID, 2, kText);
                        old_pen = SelectObject(item->hDC, check);
                        const int check_x = item->rcItem.right - 25;
                        MoveToEx(item->hDC, check_x - 4, icon_center_y, nullptr);
                        LineTo(item->hDC, check_x, icon_center_y + 4);
                        LineTo(item->hDC, check_x + 7, icon_center_y - 6);
                        SelectObject(item->hDC, old_pen);
                        DeleteObject(check);
                    }
                    return TRUE;
                }
                const auto text = menu_item_texts_.find(static_cast<UINT>(item->itemID));
                const bool submenu = menu_submenu_items_.find(static_cast<UINT>(item->itemID)) != menu_submenu_items_.end();
                FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kPanel));
                if ((item->itemState & ODS_CHECKED) != 0) {
                    HPEN check = CreatePen(PS_SOLID, 2, grayed ? kMuted : kText);
                    HGDIOBJ old_check_pen = SelectObject(item->hDC, check);
                    const int check_x = item->rcItem.left + 12;
                    const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                    MoveToEx(item->hDC, check_x - 3, center_y, nullptr);
                    LineTo(item->hDC, check_x, center_y + 4);
                    LineTo(item->hDC, check_x + 6, center_y - 5);
                    SelectObject(item->hDC, old_check_pen);
                    DeleteObject(check);
                }
                if (text != menu_item_texts_.end()) {
                    HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                    SetBkMode(item->hDC, TRANSPARENT);
                    SetTextColor(item->hDC, grayed ? kMuted : kText);
                    RECT text_rect = item->rcItem;
                    text_rect.left += 28;
                    text_rect.right -= submenu ? 28 : 12;
                    DrawTextW(item->hDC, text->second.c_str(), -1, &text_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    SelectObject(item->hDC, old_font);
                    if (submenu) {
                        HPEN arrow = CreatePen(PS_SOLID, 1, grayed ? kMuted : kText);
                        HGDIOBJ old_arrow_pen = SelectObject(item->hDC, arrow);
                        const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                        MoveToEx(item->hDC, item->rcItem.right - 18, center_y - 4, nullptr);
                        LineTo(item->hDC, item->rcItem.right - 14, center_y);
                        LineTo(item->hDC, item->rcItem.right - 18, center_y + 4);
                        SelectObject(item->hDC, old_arrow_pen);
                        DeleteObject(arrow);
                    }
                }
                return TRUE;
            }
            if (item->CtlType == ODT_STATIC && item->CtlID == kEmptyMark) {
                FillRect(item->hDC, &item->rcItem, background_brush_);
                const int center_x = (item->rcItem.left + item->rcItem.right) / 2;
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                RECT badge{center_x - 23, center_y - 23, center_x + 23, center_y + 23};
                HPEN badge_pen = CreatePen(PS_SOLID, 1, kBorder);
                HGDIOBJ old_pen = SelectObject(item->hDC, badge_pen);
                HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
                RoundRect(item->hDC, badge.left, badge.top, badge.right, badge.bottom, 14, 14);
                SelectObject(item->hDC, old_brush);
                SelectObject(item->hDC, old_pen);
                DeleteObject(badge_pen);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, mono_font_));
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, kMuted);
                RECT text_rect{center_x - 20, center_y - 12, center_x + 20, center_y + 12};
                DrawTextW(item->hDC, L">_", -1, &text_rect,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                return TRUE;
            }
            if (item->CtlType == ODT_STATIC && (item->CtlID == kProjectLabel || item->CtlID == kRecentLabel)) {
                FillRect(item->hDC, &item->rcItem, CachedBrush(kSidebar));
                wchar_t text[64]{};
                GetWindowTextW(item->hwndItem, text, 63);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, kText);
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                RECT text_rect = item->rcItem;
                text_rect.right -= ScaleForSystemDpi(20);
                DrawTextW(item->hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                const bool has_children =
                    item->CtlID == kProjectLabel ? !projects_.empty() : !history_paths_.empty();
                if (has_children) {
                    const bool expanded =
                        item->CtlID == kProjectLabel ? project_section_expanded_ : recent_section_expanded_;
                    HPEN chevron = CreatePen(PS_SOLID, 1, kMuted);
                    HGDIOBJ old_pen = SelectObject(item->hDC, chevron);
                    const int cx = item->rcItem.right - ScaleForSystemDpi(10);
                    const int cy = (item->rcItem.top + item->rcItem.bottom) / 2;
                    if (expanded) {
                        MoveToEx(item->hDC, cx - 4, cy - 2, nullptr);
                        LineTo(item->hDC, cx, cy + 2);
                        LineTo(item->hDC, cx + 4, cy - 2);
                    } else {
                        MoveToEx(item->hDC, cx - 2, cy - 4, nullptr);
                        LineTo(item->hDC, cx + 2, cy);
                        LineTo(item->hDC, cx - 2, cy + 4);
                    }
                    SelectObject(item->hDC, old_pen);
                    DeleteObject(chevron);
                }
                SelectObject(item->hDC, old_font);
                return TRUE;
            }
            if (item->CtlType == ODT_COMBOBOX && item->CtlID == kModelCombo) {
                FillRect(item->hDC, &item->rcItem, CachedBrush(kInput));
                wchar_t text[512]{};
                if (item->itemID != static_cast<UINT>(-1)) SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID,
                                                                       reinterpret_cast<LPARAM>(text));
                else GetWindowTextW(item->hwndItem, text, 511);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, disabled ? kMuted : kText);
                RECT text_rect = item->rcItem;
                text_rect.left += 8;
                text_rect.right -= 30;
                DrawTextW(item->hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kHistoryList) {
                const size_t item_index = static_cast<size_t>(item->itemID);
                const bool valid_item = item->itemID != static_cast<UINT>(-1) &&
                                        (item_index < history_paths_.size() || item_index == history_more_index_);
                if (!valid_item) {
                    FillRect(item->hDC, &item->rcItem, CachedBrush(kSidebar));
                    return TRUE;
                }
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                const bool hovered = static_cast<int>(item->itemID) == hovered_history_item_;
                const bool active = selected || hovered;
                FillRect(item->hDC, &item->rcItem, CachedBrush(active ? kSidebarHover : kSidebar));
                wchar_t text[512]{};
                SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
                SetBkMode(item->hDC, TRANSPARENT);
                const bool more_row = item->itemID == history_more_index_;
                if (more_row) {
                    SetTextColor(item->hDC, active ? kSidebarHoverText : kMuted);
                    RECT more_rect = item->rcItem;
                    more_rect.left += ScaleForSystemDpi(10);
                    more_rect.right -= ScaleForSystemDpi(10);
                    DrawTextW(item->hDC, text, -1, &more_rect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                    return TRUE;
                }
                SetTextColor(item->hDC, active ? kSidebarHoverText : kText);
                RECT text_rect = item->rcItem;
                text_rect.left += 10;
                text_rect.right -= hovered ? ScaleForSystemDpi(96) : ScaleForSystemDpi(8);
                const bool row_running = item_index < history_paths_.size() &&
                                         IsSessionRunning(history_paths_[item_index]);
                if (row_running) text_rect.right -= ScaleForSystemDpi(26);
                DrawTextW(item->hDC, text, -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                if (row_running) {
                    DrawActivityGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(hovered ? 104 : 20), center_y,
                                      ScaleForSystemDpi(5), activity_frame_, active ? kSidebarHoverText : kText,
                                      kSubtle);
                }
                if (hovered) {
                    DrawTrashGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(78), center_y, kSidebarHoverText);
                    DrawPinGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(50), center_y, kSidebarHoverText);
                    DrawArchiveGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(22), center_y, kSidebarHoverText);
                }
                return TRUE;
            }
            if (item->CtlType == ODT_LISTBOX && item->CtlID == kProjectsList) {
                const size_t row_index = item->itemID;
                if (item->itemID == static_cast<UINT>(-1) || row_index >= project_rows_.size()) {
                    FillRect(item->hDC, &item->rcItem, CachedBrush(kSidebar));
                    return TRUE;
                }
                const SidebarRow* row = row_index < project_rows_.size() ? &project_rows_[row_index] : nullptr;
                const bool selected = (item->itemState & ODS_SELECTED) != 0;
                const bool hovered = static_cast<int>(item->itemID) == hovered_project_item_;
                const bool active = selected || hovered;
                const bool project_row = row != nullptr && row->kind == SidebarRowKind::Project;
                const bool more_row = row != nullptr && row->kind == SidebarRowKind::More;
                FillRect(item->hDC, &item->rcItem, CachedBrush(active ? kSidebarHover : kSidebar));
                wchar_t text[512]{};
                SendMessageW(item->hwndItem, LB_GETTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, active ? kSidebarHoverText : kText);
                RECT text_rect = item->rcItem;
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                const bool running_chat = !project_row && !more_row && row != nullptr &&
                                          !row->session_path.empty() && IsSessionRunning(row->session_path);
                if (project_row) {
                    const bool expanded = row->project < project_expanded_.size() && project_expanded_[row->project];
                    HPEN chevron = CreatePen(PS_SOLID, 1, active ? kSidebarHoverText : kMuted);
                    HGDIOBJ old_pen = SelectObject(item->hDC, chevron);
                    const int cx = item->rcItem.left + ScaleForSystemDpi(9);
                    if (expanded) {
                        MoveToEx(item->hDC, cx - 3, center_y - 2, nullptr);
                        LineTo(item->hDC, cx, center_y + 1);
                        LineTo(item->hDC, cx + 3, center_y - 2);
                    } else {
                        MoveToEx(item->hDC, cx - 2, center_y - 3, nullptr);
                        LineTo(item->hDC, cx + 1, center_y);
                        LineTo(item->hDC, cx - 2, center_y + 3);
                    }
                    SelectObject(item->hDC, old_pen);
                    DeleteObject(chevron);
                    DrawFolderGlyph(item->hDC, item->rcItem.left + ScaleForSystemDpi(24), center_y,
                                    active ? kSidebarHoverText : kMuted);
                    text_rect.left += ScaleForSystemDpi(38);
                    text_rect.right -= hovered ? ScaleForSystemDpi(66) : ScaleForSystemDpi(10);
                } else if (more_row) {
                    text_rect.left += ScaleForSystemDpi(30);
                    text_rect.right -= ScaleForSystemDpi(10);
                    SetTextColor(item->hDC, active ? kSidebarHoverText : kMuted);
                } else {
                    HPEN branch = CreatePen(PS_SOLID, 1, active ? kSidebarHoverText : kSubtle);
                    HGDIOBJ old_pen = SelectObject(item->hDC, branch);
                    const int x = item->rcItem.left + ScaleForSystemDpi(16);
                    MoveToEx(item->hDC, x, center_y, nullptr);
                    LineTo(item->hDC, x + ScaleForSystemDpi(8), center_y);
                    SelectObject(item->hDC, old_pen);
                    DeleteObject(branch);
                    text_rect.left += ScaleForSystemDpi(30);
                    text_rect.right -= hovered ? ScaleForSystemDpi(96) : ScaleForSystemDpi(10);
                }
                if (running_chat) text_rect.right -= ScaleForSystemDpi(26);
                DrawTextW(item->hDC, text, -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                if (running_chat) {
                    DrawActivityGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(hovered ? 104 : 20), center_y,
                                      ScaleForSystemDpi(5), activity_frame_, active ? kSidebarHoverText : kText,
                                      kSubtle);
                }
                if (hovered && project_row) {
                    DrawPencilPaperGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(38),
                                         item->rcItem.top, item->rcItem.right - ScaleForSystemDpi(16),
                                         item->rcItem.bottom, kSidebarHoverText);
                    HBRUSH dot = CreateSolidBrush(kSidebarHoverText);
                    HGDIOBJ old_dot = SelectObject(item->hDC, dot);
                    const int x = item->rcItem.right - ScaleForSystemDpi(58);
                    Ellipse(item->hDC, x - 1, center_y - 7, x + 2, center_y - 4);
                    Ellipse(item->hDC, x - 1, center_y - 2, x + 2, center_y + 1);
                    Ellipse(item->hDC, x - 1, center_y + 3, x + 2, center_y + 6);
                    SelectObject(item->hDC, old_dot);
                    DeleteObject(dot);
                } else if (hovered && row != nullptr && row->kind == SidebarRowKind::Chat) {
                    DrawTrashGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(78), center_y, kSidebarHoverText);
                    DrawPinGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(50), center_y, kSidebarHoverText);
                    DrawArchiveGlyph(item->hDC, item->rcItem.right - ScaleForSystemDpi(22), center_y,
                                     kSidebarHoverText);
                }
                return TRUE;
            }
            if (item->CtlType != ODT_BUTTON) break;
            if (item->CtlID == kProjectMenuButton || item->CtlID == kRecentMenuButton ||
                item->CtlID == kProjectAddButton || item->CtlID == kRecentNewButton) {
                const bool hovered = (item->itemState & ODS_HOTLIGHT) != 0;
                const bool active = pressed || hovered;
                FillRect(item->hDC, &item->rcItem, CachedBrush(active ? kSidebarHover : kSidebar));
                const int center_x = (item->rcItem.left + item->rcItem.right) / 2;
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                HPEN pen = CreatePen(PS_SOLID, 1, disabled ? kMuted : (active ? kSidebarHoverText : kMuted));
                HGDIOBJ old_pen = SelectObject(item->hDC, pen);
                if (item->CtlID == kProjectMenuButton || item->CtlID == kRecentMenuButton) {
                    HBRUSH dot = CreateSolidBrush(disabled ? kMuted : (active ? kSidebarHoverText : kMuted));
                    HGDIOBJ old_dot = SelectObject(item->hDC, dot);
                    Ellipse(item->hDC, center_x - 1, center_y - 7, center_x + 2, center_y - 4);
                    Ellipse(item->hDC, center_x - 1, center_y - 2, center_x + 2, center_y + 1);
                    Ellipse(item->hDC, center_x - 1, center_y + 3, center_x + 2, center_y + 6);
                    SelectObject(item->hDC, old_dot);
                    DeleteObject(dot);
                } else if (item->CtlID == kProjectAddButton) {
                    MoveToEx(item->hDC, center_x - 6, center_y, nullptr);
                    LineTo(item->hDC, center_x + 6, center_y);
                    MoveToEx(item->hDC, center_x, center_y - 6, nullptr);
                    LineTo(item->hDC, center_x, center_y + 6);
                } else {
                    DrawPencilPaperGlyph(item->hDC, item->rcItem.left + 4, item->rcItem.top + 4,
                                         item->rcItem.right - 4, item->rcItem.bottom - 4,
                                         active ? kSidebarHoverText : kMuted);
                }
                SelectObject(item->hDC, old_pen);
                DeleteObject(pen);
                return TRUE;
            }
            if (item->CtlID == kNewConversation || item->CtlID == kSettingsButton) {
                const bool hovered = (item->itemState & ODS_HOTLIGHT) != 0;
                const bool active = pressed || hovered;
                FillRect(item->hDC, &item->rcItem, CachedBrush(active ? kSidebarHover : kSidebar));
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, disabled ? kMuted : (active ? kSidebarHoverText : kText));
                RECT text_rect = item->rcItem;
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                HPEN icon_pen = CreatePen(PS_SOLID, 1,
                                          disabled ? kMuted : (active ? kSidebarHoverText : kText));
                HGDIOBJ old_pen = SelectObject(item->hDC, icon_pen);
                HGDIOBJ old_brush = SelectObject(item->hDC, GetStockObject(NULL_BRUSH));
                if (item->CtlID == kNewConversation) {
                    const int icon_x = text_rect.left + 15;
                    Rectangle(item->hDC, icon_x - 6, center_y - 6, icon_x + 7, center_y + 6);
                    MoveToEx(item->hDC, icon_x - 2, center_y + 2, nullptr);
                    LineTo(item->hDC, icon_x + 4, center_y - 4);
                } else {
                    Ellipse(item->hDC, text_rect.left + 11, center_y - 6, text_rect.left + 23, center_y + 6);
                    Ellipse(item->hDC, text_rect.left + 14, center_y - 3, text_rect.left + 20, center_y + 3);
                }
                SelectObject(item->hDC, old_brush);
                SelectObject(item->hDC, old_pen);
                DeleteObject(icon_pen);
                text_rect.left += 34;
                wchar_t text[128]{};
                GetWindowTextW(item->hwndItem, text, 127);
                DrawTextW(item->hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                return TRUE;
            }
            if (item->CtlID == kPlusButton) {
                FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kPanel));
                const int center_x = (item->rcItem.left + item->rcItem.right) / 2;
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                HPEN plus = CreatePen(PS_SOLID, 1, disabled ? kMuted : kText);
                HGDIOBJ old_pen = SelectObject(item->hDC, plus);
                MoveToEx(item->hDC, center_x - 6, center_y, nullptr);
                LineTo(item->hDC, center_x + 6, center_y);
                MoveToEx(item->hDC, center_x, center_y - 6, nullptr);
                LineTo(item->hDC, center_x, center_y + 6);
                SelectObject(item->hDC, old_pen);
                DeleteObject(plus);
                return TRUE;
            }
            if (item->CtlID == kSummaryButton) {
                POINT cursor{};
                GetCursorPos(&cursor);
                ScreenToClient(item->hwndItem, &cursor);
                const bool hovered = PtInRect(&item->rcItem, cursor) != FALSE;
                FillRect(item->hDC, &item->rcItem, CachedBrush(pressed || hovered ? kSelection : kBackground));
                HDC dc = GetDC(window_);
                const int dpi = GetDeviceCaps(dc, LOGPIXELSX);
                ReleaseDC(window_, dc);
                DrawSummaryGlyph(item->hDC, item->rcItem, disabled ? kMuted : kText, dpi);
                return TRUE;
            }
            if (item->CtlID == kSideClose) {
                const bool hovered = (item->itemState & ODS_HOTLIGHT) != 0;
                FillRect(item->hDC, &item->rcItem, CachedBrush(hovered ? kSidebarHover : kPanel));
                HPEN pen = CreatePen(PS_SOLID, 1, hovered ? kSidebarHoverText : kMuted);
                HGDIOBJ old_pen = SelectObject(item->hDC, pen);
                MoveToEx(item->hDC, item->rcItem.left + 8, item->rcItem.top + 8, nullptr);
                LineTo(item->hDC, item->rcItem.right - 8, item->rcItem.bottom - 8);
                MoveToEx(item->hDC, item->rcItem.right - 8, item->rcItem.top + 8, nullptr);
                LineTo(item->hDC, item->rcItem.left + 8, item->rcItem.bottom - 8);
                SelectObject(item->hDC, old_pen);
                DeleteObject(pen);
                return TRUE;
            }
            if (item->CtlID == kModelButton) {
                FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kInput));
                SetBkMode(item->hDC, TRANSPARENT);
                const ModelConfig* model = ActiveModel(config_);
                const std::wstring model_name = model == nullptr ? i18n::Text(L"No model configured") : ModelDisplayName(*model);
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                int right = item->rcItem.right - 24;
                const std::string active_effort = model != nullptr && config_.active.reasoning_effort.empty()
                    ? EffectiveDefaultReasoningEffort(*model) : config_.active.reasoning_effort;
                if (!active_effort.empty()) {
                    const std::wstring effort = ReasoningLabel(active_effort);
                    HDC measure = item->hDC;
                    HFONT old_font = static_cast<HFONT>(SelectObject(measure, ui_font_));
                    SIZE effort_size{};
                    GetTextExtentPoint32W(measure, effort.c_str(), static_cast<int>(effort.size()), &effort_size);
                    SelectObject(measure, old_font);
                    const int badge_width = effort_size.cx + 18;
                    RECT badge{right - badge_width, item->rcItem.top + 4, right, item->rcItem.bottom - 4};
                    HBRUSH badge_brush = CreateSolidBrush(kSelection);
                    HPEN badge_pen = CreatePen(PS_SOLID, 1, kBorder);
                    HGDIOBJ old_brush = SelectObject(item->hDC, badge_brush);
                    HGDIOBJ old_pen = SelectObject(item->hDC, badge_pen);
                    RoundRect(item->hDC, badge.left, badge.top, badge.right, badge.bottom, 12, 12);
                    SelectObject(item->hDC, old_brush);
                    SelectObject(item->hDC, old_pen);
                    DeleteObject(badge_brush);
                    DeleteObject(badge_pen);
                    HFONT old_text_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                    SetTextColor(item->hDC, kAccent);
                    DrawTextW(item->hDC, effort.c_str(), -1, &badge, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                    SelectObject(item->hDC, old_text_font);
                    right = badge.left - 10;
                }
                HFONT old_font = static_cast<HFONT>(SelectObject(item->hDC, ui_font_));
                SetTextColor(item->hDC, disabled ? kMuted : kText);
                RECT text_rect = item->rcItem;
                text_rect.left += 14;
                text_rect.right = right;
                DrawTextW(item->hDC, model_name.c_str(), -1, &text_rect,
                          DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
                SelectObject(item->hDC, old_font);
                HPEN chevron = CreatePen(PS_SOLID, 1, kMuted);
                HGDIOBJ old_pen = SelectObject(item->hDC, chevron);
                const int cx = item->rcItem.right - 12;
                MoveToEx(item->hDC, cx - 3, center_y - 2, nullptr);
                LineTo(item->hDC, cx, center_y + 1);
                LineTo(item->hDC, cx + 3, center_y - 2);
                SelectObject(item->hDC, old_pen);
                DeleteObject(chevron);
                return TRUE;
            }
            if (item->CtlID == kPermissionButton) {
                FillRect(item->hDC, &item->rcItem, CachedBrush(pressed ? kSelection : kPanel));
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                HBRUSH dot = CreateSolidBrush(PermissionColor(permission_mode_));
                HGDIOBJ old_dot = SelectObject(item->hDC, dot);
                HPEN dot_pen = CreatePen(PS_SOLID, 1, PermissionColor(permission_mode_));
                HGDIOBJ old_pen = SelectObject(item->hDC, dot_pen);
                Ellipse(item->hDC, item->rcItem.left + 10, center_y - 5, item->rcItem.left + 20, center_y + 5);
                SelectObject(item->hDC, old_dot);
                SelectObject(item->hDC, old_pen);
                DeleteObject(dot_pen);
                DeleteObject(dot);
                wchar_t text[128]{};
                GetWindowTextW(item->hwndItem, text, 127);
                SetBkMode(item->hDC, TRANSPARENT);
                SetTextColor(item->hDC, disabled ? kMuted : kText);
                RECT text_rect = item->rcItem;
                text_rect.left += 26;
                text_rect.right -= 20;
                DrawTextW(item->hDC, text, -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                HPEN chevron = CreatePen(PS_SOLID, 1, kMuted);
                old_pen = SelectObject(item->hDC, chevron);
                const int cx = item->rcItem.right - 13;
                MoveToEx(item->hDC, cx - 3, center_y - 2, nullptr);
                LineTo(item->hDC, cx, center_y + 1);
                LineTo(item->hDC, cx + 3, center_y - 2);
                SelectObject(item->hDC, old_pen);
                DeleteObject(chevron);
                return TRUE;
            }
            if (item->CtlID == kSend) {
                const bool busy = IsActiveSessionRunning();
                const bool paused = !busy && HasPausedTurn() && Trim(ComposerText()).empty();
                const COLORREF fill =
                    disabled ? RGB(65, 66, 70) : busy ? kError : pressed ? kAccentPressed : kAccent;
                FillRect(item->hDC, &item->rcItem, CachedBrush(kPanel));
                HBRUSH brush = CreateSolidBrush(fill);
                HPEN border = CreatePen(PS_SOLID, 1, fill);
                HGDIOBJ old_brush = SelectObject(item->hDC, brush);
                HGDIOBJ old_pen = SelectObject(item->hDC, border);
                Ellipse(item->hDC, item->rcItem.left, item->rcItem.top, item->rcItem.right, item->rcItem.bottom);
                SelectObject(item->hDC, old_pen);
                SelectObject(item->hDC, old_brush);
                DeleteObject(border);
                DeleteObject(brush);
                const int center_x = (item->rcItem.left + item->rcItem.right) / 2;
                const int center_y = (item->rcItem.top + item->rcItem.bottom) / 2;
                if (busy) {
                    HBRUSH stop = CreateSolidBrush(disabled ? kMuted : kBackground);
                    RECT square{center_x - 5, center_y - 5, center_x + 5, center_y + 5};
                    FillRect(item->hDC, &square, stop);
                    DeleteObject(stop);
                } else if (paused) {
                    HBRUSH play = CreateSolidBrush(disabled ? kMuted : kBackground);
                    HGDIOBJ old_play = SelectObject(item->hDC, play);
                    POINT triangle[3] = {
                        {center_x - 4, center_y - 7},
                        {center_x - 4, center_y + 7},
                        {center_x + 7, center_y},
                    };
                    Polygon(item->hDC, triangle, 3);
                    SelectObject(item->hDC, old_play);
                    DeleteObject(play);
                } else {
                    HPEN arrow = CreatePen(PS_SOLID, 2, disabled ? kMuted : kBackground);
                    old_pen = SelectObject(item->hDC, arrow);
                    MoveToEx(item->hDC, center_x, center_y + 7, nullptr);
                    LineTo(item->hDC, center_x, center_y - 7);
                    MoveToEx(item->hDC, center_x - 5, center_y - 2, nullptr);
                    LineTo(item->hDC, center_x, center_y - 7);
                    LineTo(item->hDC, center_x + 5, center_y - 2);
                    SelectObject(item->hDC, old_pen);
                    DeleteObject(arrow);
                }
                return TRUE;
            }
            break;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int notification = HIWORD(wparam);
            if (id == kTraySettings) {
                RestoreFromTray();
                OpenSettingsPage();
                return 0;
            }
            if (id == kTrayExit) {
                RequestExit();
                return 0;
            }
            if (id == kNewConversation && notification == BN_CLICKED) {
                BeginNewSession();
                return 0;
            }
            if (id == kProjectMenuButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(project_menu_button_, &rect);
                ShowSectionMenu(POINT{rect.left, rect.bottom}, true);
                return 0;
            }
            if (id == kProjectAddButton && notification == BN_CLICKED) {
                CreateProjectInteractive();
                return 0;
            }
            if (id == kRecentMenuButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(recent_menu_button_, &rect);
                ShowSectionMenu(POINT{rect.left, rect.bottom}, false);
                return 0;
            }
            if (id == kRecentNewButton && notification == BN_CLICKED) {
                BeginNewSession();
                return 0;
            }
            if (id == kSummaryButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(summary_button_, &rect);
                ShowSummaryMenu(POINT{rect.right, rect.bottom});
                return 0;
            }
            if (id == kSubAgentButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(subagent_button_, &rect);
                ShowSubAgentMenu(POINT{rect.left, rect.bottom});
                return 0;
            }
            if (id == kSideClose && notification == BN_CLICKED) {
                CloseSubAgentChat();
                return 0;
            }
            if (id == kProjectsList && notification == LBN_SELCHANGE && !populating_) {
                const int index = static_cast<int>(SendMessageW(projects_list_, LB_GETCURSEL, 0, 0));
                if (index >= 0 && index < static_cast<int>(project_rows_.size())) {
                    const SidebarRow& row = project_rows_[index];
                    if (row.kind == SidebarRowKind::Project) {
                        ToggleProjectExpanded(row.project);
                    } else if (row.kind == SidebarRowKind::More) {
                        if (row.project < projects_.size()) {
                            project_show_all_[NormalizePathForComparison(projects_[row.project].workspace)] = true;
                            RefreshProjectRows();
                        }
                    } else if (!row.session_path.empty()) {
                        PostSessionActivation(row.session_path);
                    }
                }
                return 0;
            }
            if (id == kSettingsButton && notification == BN_CLICKED) {
                OpenSettingsPage();
                return 0;
            }
            if (id == kSend && notification == BN_CLICKED) {
                ToggleSendAction();
                return 0;
            }
            if (id == kPlusButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(plus_button_, &rect);
                POINT anchor{rect.left, rect.top};
                ShowAddMenu(anchor);
                return 0;
            }
            if (id == kPermissionButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(permission_button_, &rect);
                POINT anchor{rect.left, rect.top};
                ShowPermissionMenu(anchor);
                return 0;
            }
            if (id == kModelButton && notification == BN_CLICKED) {
                RECT rect{};
                GetWindowRect(model_button_, &rect);
                ShowModelMenu(POINT{rect.left, rect.top});
                return 0;
            }
            if (id == kModelCombo && notification == CBN_SELCHANGE && !populating_) {
                ApplyModelSelection();
                return 0;
            }
            if (id == kModelCombo && notification == CBN_DROPDOWN) {
                ResizeModelDropdown();
                return 0;
            }
            if (id == kComposer && notification == EN_CHANGE) {
                composer_dirty_ = true;
                UpdateSendButton();
                return 0;
            }
            if (id == kHistoryList && notification == LBN_SELCHANGE && !populating_) {
                const int index = static_cast<int>(SendMessageW(history_list_, LB_GETCURSEL, 0, 0));
                if (index < 0 || index >= static_cast<int>(history_paths_.size())) return 0;
                ResumeSelectedSession();
                return 0;
            }
            if (id == kHistoryList && notification == LBN_DBLCLK) {
                const int index = static_cast<int>(SendMessageW(history_list_, LB_GETCURSEL, 0, 0));
                if (index < 0 || index >= static_cast<int>(history_paths_.size())) return 0;
                ResumeSelectedSession();
                return 0;
            }
            if ((id == kProjectLabel || id == kRecentLabel) && notification == STN_CLICKED) {
                if (id == kProjectLabel) {
                    if (projects_.empty()) return 0;
                    for (const ProjectEntry& project : projects_) {
                        project_show_all_[NormalizePathForComparison(project.workspace)] = false;
                    }
                    project_section_expanded_ = !project_section_expanded_;
                } else {
                    if (history_paths_.empty()) return 0;
                    recent_show_all_ = false;
                    recent_section_expanded_ = !recent_section_expanded_;
                }
                InvalidateRect(id == kProjectLabel ? project_label_ : recent_label_, nullptr, TRUE);
                LayoutControls();
                RefreshProjectRows();
                if (id == kRecentLabel) RefreshHistory();
                return 0;
            }
            break;
        }
        case kAddActionMessage:
            if (wparam <= static_cast<WPARAM>(AddAction::Plan)) HandleAddAction(static_cast<AddAction>(wparam));
            return 0;
        case kSnapshotMessage:
            AttachSmartSnapshot();
            return 0;
        case kUiEventMessage:
            HandleUiEvent(reinterpret_cast<UiEvent*>(lparam));
            return 0;
        case kActivateSessionMessage: {
            std::unique_ptr<std::string> queued(reinterpret_cast<std::string*>(lparam));
            if (queued != nullptr) ResumeSessionPath(*queued);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kSessionLoadTimer) {
                PollSessionLoad();
                return 0;
            }
            if (wparam == kSnapshotPollTimer) {
                PollSmartSnapshot();
                return 0;
            }
            if (wparam == kComposerTimer) {
                if (composer_dirty_) {
                    SaveComposerState();
                    composer_dirty_ = false;
                }
                return 0;
            }
            if (wparam == kTurnWatchdogTimer) {
                TickTurnWatchdog();
                return 0;
            }
            if (wparam == kActivityTimer) {
                if (AnyTurnRunning()) TickActivity();
                else StopActivityAnimation();
                return 0;
            }
            if (wparam == kExternalPollTimer) {
                PollExternalRuns();
                return 0;
            }
            return 0;
        case kApprovalMessage: {
            auto* context = reinterpret_cast<ApprovalContext*>(lparam);
            if (exiting_ || (context->runtime != nullptr && context->runtime->cancellation.IsCancelled())) {
                context->approved = false;
                return 0;
            }
            std::wstring details = Utf8ToWide(Trim(context->request.detail));
            if (details.size() > 4000) details.resize(4000);
            const std::wstring message = Utf8ToWide(context->request.title) + L"\n\n" + details +
                                         i18n::Text(L"\n\nThis is application-level approval, not an operating-system sandbox.\n\nChoose Yes to allow once, No to deny, or Cancel to allow for this turn.");
            const int result = MessageBoxW(window_, message.c_str(), i18n::Text(L"Approval required"), MB_YESNOCANCEL | MB_ICONWARNING);
            if (result == IDYES) context->approved = true;
            else if (result == IDCANCEL) {
                if (context->runtime != nullptr) context->runtime->approve_turn.store(true);
                context->approved = true;
            }
            return 0;
        }
        case kTrayMessage:
            if (wparam == kTrayIconId) {
                if (lparam == WM_LBUTTONUP || lparam == WM_LBUTTONDBLCLK) RestoreFromTray();
                else if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) ShowTrayMenu();
                return 0;
            }
            break;
        case WM_CLOSE:
            SaveWindowsWindowGeometry(window_, DataDirectory());
            if (exiting_) {
                RequestExit();
                return 0;
            }
            SaveComposerState();
            AddTrayIcon();
            ShowWindow(window_, SW_HIDE);
            return 0;
        case WM_DESTROY:
            SaveWindowsWindowGeometry(window_, DataDirectory());
            session_loader_.Stop();
            session_replay_step_ = {};
            KillTimer(window_, kSessionLoadTimer);
            snapshot_shortcut_.Stop();
            snapshot_capture_.Cancel();
            KillTimer(window_, kSnapshotPollTimer);
            add_menu_.reset();
            RemoveTrayIcon();
            KillTimer(window_, kComposerTimer);
            KillTimer(window_, kTurnWatchdogTimer);
            KillTimer(window_, kActivityTimer);
            KillTimer(window_, kExternalPollTimer);
            WriteActiveSessionPath();
            CancelAndJoinAllTurns();
            settings_.Close();
            if (mcp_ != nullptr) mcp_->StopAll();
            if (ui_font_ != nullptr) DeleteObject(ui_font_);
            if (chat_font_ != nullptr) DeleteObject(chat_font_);
            if (mono_font_ != nullptr) DeleteObject(mono_font_);
            if (brand_font_ != nullptr) DeleteObject(brand_font_);
            if (empty_mark_font_ != nullptr) DeleteObject(empty_mark_font_);
            if (empty_title_font_ != nullptr) DeleteObject(empty_title_font_);
            ui_font_ = chat_font_ = mono_font_ = brand_font_ = empty_mark_font_ = empty_title_font_ = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

}  // namespace spark
