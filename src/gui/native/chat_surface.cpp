#include "gui/native/chat_surface.h"
#include "gui/native/chat_usage.h"
#include "gui/native/popup_menu.h"
#include "gui/chat_timeline.h"
#include "gui/chat_ui_helpers.h"
#include "gui/localization.h"
#include "core/common.h"
#include "platform/clock.h"
#include "nlohmann/json.hpp"

#include <wx/clipbrd.h>
#include <wx/filename.h>
#include <wx/uri.h>
#include <wx/dcbuffer.h>
#include <wx/menu.h>
#include <wx/settings.h>
#include <wx/control.h>
#include <wx/datetime.h>
#include <wx/utils.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace spark {
namespace {
wxString U(const std::string& value) { return wxString::FromUTF8(value.data(), value.size()); }
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
struct NativeLink { wxString target; bool web = false; };
NativeLink ResolveNativeLink(const std::wstring& destination) {
    const wxString raw(destination);
    const auto lower = raw.Lower();
    if (lower.StartsWith("https://") || lower.StartsWith("http://")) return {raw, true};
    const wxString target(ui::FileLinkTarget(destination));
    if (target.empty() || target[0] == '#') return {};
    wxFileName file;
    if (lower.StartsWith("file://")) file = wxFileName::URLToFileName(target);
    else {
        const int colon = target.Find(':');
        const bool drive = target.length() >= 3 && colon == 1 &&
            ((target[0] >= 'A' && target[0] <= 'Z') || (target[0] >= 'a' && target[0] <= 'z')) &&
            (target[2] == '/' || target[2] == '\\');
        if (colon != wxNOT_FOUND && !drive) return {};
        file = wxFileName(wxURI::Unescape(target));
    }
    if (!file.Normalize(wxPATH_NORM_DOTS | wxPATH_NORM_ABSOLUTE | wxPATH_NORM_TILDE)) return {};
    const auto path = file.GetFullPath();
    if (path.empty() || path.find(wxChar(0)) != wxString::npos) return {};
    return {path, false};
}
bool RevealNativeFile(wxWindow* owner, const wxString& path) {
#ifdef _WIN32
    return ui::RevealFileInManager(reinterpret_cast<HWND>(owner->GetHandle()), path.ToStdWstring());
#else
    (void)owner;
#ifdef __WXOSX__
    constexpr auto platform = ui::FileRevealPlatform::MacOS;
#else
    constexpr auto platform = ui::FileRevealPlatform::Linux;
#endif
    const auto args = ui::FileRevealCommand(platform, path.ToStdString(wxConvUTF8));
    if (args.empty()) return false;
    wxArrayString strings;
    for (const auto& arg : args) strings.Add(U(arg));
    std::vector<const wxChar*> argv;
    for (const auto& arg : strings) argv.push_back(arg.c_str());
    argv.push_back(nullptr);
    return wxExecute(argv.data(), wxEXEC_ASYNC) > 0;
#endif
}
enum class DurationKind { Tool, Turn };
wxString Duration(uint64_t ms, DurationKind kind = DurationKind::Tool) {
    const auto seconds = std::to_wstring(ms / 1000);
    const auto minutes = std::to_wstring(ms / 60000);
    if (kind == DurationKind::Turn) {
        if (ms < 60000) return wxString(i18n::Format(L"{seconds} sec", {{L"seconds", seconds}}));
        if (ms < 3600000) return wxString(i18n::Format(L"{minutes} min {seconds} sec",
            {{L"minutes", minutes}, {L"seconds", std::to_wstring(ms / 1000 % 60)}}));
        return wxString(i18n::Format(L"{hours}h {minutes}m {seconds}s",
            {{L"hours", std::to_wstring(ms / 3600000)}, {L"minutes", std::to_wstring(ms / 60000 % 60)},
             {L"seconds", std::to_wstring(ms / 1000 % 60)}}));
    }
    if (ms < 1000) return wxString(i18n::Format(L"{milliseconds}ms", {{L"milliseconds", std::to_wstring(ms)}}));
    if (ms < 60000) return wxString(i18n::Format(L"{seconds}s",
        {{L"seconds", seconds + L"." + std::to_wstring(ms / 100 % 10)}}));
    return wxString(i18n::Format(L"{minutes}m {seconds}s",
        {{L"minutes", minutes}, {L"seconds", std::to_wstring(ms / 1000 % 60)}}));
}
wxString ReasoningLine(const std::string& source) {
    const auto text = U(source);
    size_t start = 0;
    for (size_t index = 0; index < text.length(); ++index) {
        const auto c = text[index].GetValue();
        const bool end = index + 1 == text.length() || text[index + 1] == ' ' || text[index + 1] == '\n';
        if (c == '\r' || c == '\n' || c == 0x3002 || c == 0xff01 || c == 0xff1f
            || ((c == '.' || c == '!' || c == '?') && end)) start = index + 1;
    }
    auto sentence = text.Mid(start);
    if (sentence.Trim().Trim(false).empty()) sentence = text;
    sentence.Replace("\r", " "); sentence.Replace("\n", " "); sentence.Replace("\t", " ");
    return sentence.Left(2048);
}
bool ImageTool(const std::string& name) {
    const auto lower = LowerAscii(name);
    for (const auto* word : {"image", "vision", "screenshot", "photo", "picture", "ocr"})
        if (lower.find(word) != std::string::npos) return true;
    return false;
}
wxString ToolLabel(const ChatItem& item) {
    if (!item.finished) return T(item.tool_name == "run_command" ? L"Running command" : L"Running tool");
    if (ImageTool(item.tool_name)) return T(L"Viewed image");
    for (const auto& entry : {std::make_pair("run_command", L"Ran command"), {"list_files", L"Listed files"},
             {"read_file", L"Read file"}, {"read_skill", L"Read skill"}, {"write_file", L"Wrote file"},
             {"apply_patch", L"Applied patch"}, {"search_text", L"Searched text"}})
        if (item.tool_name == entry.first) return T(entry.second);
    return item.tool_name.empty() ? T(L"Ran tool")
        : wxString(i18n::Format(L"Ran {tool}", {{L"tool", U(item.tool_name).ToStdWstring()}}));
}
enum class ToolTextKind { Summary, Details };
std::string ToolText(const ChatItem& item, ToolTextKind kind) {
    if (ImageTool(item.tool_name) && !item.image_paths.empty()) return {};
    const auto args = nlohmann::json::parse(item.arguments.empty() ? "{}" : item.arguments, nullptr, false);
    const auto argument = [&](const char* key) {
        if (args.is_object()) {
            const auto found = args.find(key);
            if (found != args.end() && found->is_string()) return found->get<std::string>();
        }
        return std::string{};
    };
    if (kind == ToolTextKind::Details) {
        std::string text = item.tool_name == "run_command"
            ? (args.is_object() ? "$ " + argument("command") : item.arguments)
            : "[" + item.tool_name + "]\n" + item.arguments;
        if (!item.result.empty()) text += (text.empty() ? "" : "\n\n") + item.result;
        return text;
    }
    std::string summary;
    if (item.tool_name == "run_command") summary = argument("command");
    else if (item.tool_name == "search_text") summary = argument("query");
    else if (item.tool_name == "read_skill") {
        summary = argument("name");
        const auto path = argument("path");
        if (!path.empty()) summary += (summary.empty() ? "" : "/") + path;
    } else summary = argument("path");
    if (summary.empty()) summary = item.arguments;
    for (auto& c : summary) if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    return summary;
}
wxString GroupLabel(const TranscriptGroup& group, const std::string& name) {
    if (!group.active && group.read_count == 1 && group.command_count == 1 && group.search_count == 0)
        return T(L"Read a file and ran a command");
    std::vector<std::wstring> parts;
    const auto add = [&](size_t n, const wchar_t* singular, const wchar_t* plural) {
        if (n) parts.push_back(n == 1 ? std::wstring(i18n::Text(singular))
            : i18n::Format(plural, {{L"n", std::to_wstring(n)}}));
    };
    add(group.read_count, group.active ? L"Reading a file" : L"Read a file",
        group.active ? L"Reading {n} files" : L"Read {n} files");
    add(group.search_count, group.active ? L"Searching text" : L"Searched text",
        group.active ? L"Searching {n} times" : L"Searched {n} times");
    add(group.command_count, group.active ? L"Running a command" : L"Ran a command",
        group.active ? L"Running {n} commands" : L"Ran {n} commands");
    if (parts.size() == 1) return wxString(parts[0]);
    if (parts.size() == 2) return wxString(i18n::Format(L"{first}, {second}",
        {{L"first", parts[0]}, {L"second", parts[1]}}));
    if (parts.size() == 3) return wxString(i18n::Format(L"{first}, {second}, {third}",
        {{L"first", parts[0]}, {L"second", parts[1]}, {L"third", parts[2]}}));
    const wchar_t* key = group.active ? L"Running {n} tools" : L"Ran {n} tools";
    if (ImageTool(name)) key = group.active ? L"Viewing {n} images" : L"Viewed {n} images";
    else if (name == "write_file" || name == "apply_patch")
        key = group.active ? L"Modifying {n} files" : L"Modified {n} files";
    return wxString(i18n::Format(key, {{L"n", std::to_wstring(group.size)}}));
}
std::vector<markdown::MarkdownLine> PlainLines(const std::string& text, markdown::MarkdownStyle style) {
    std::vector<markdown::MarkdownLine> lines;
    for (const auto& part : wxSplit(U(text), '\n')) {
        auto line = part.ToStdWstring();
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        lines.push_back({{{std::move(line), style}}, style == markdown::MarkdownStyle::Code, false, 0});
    }
    return lines;
}
}  // namespace

NativeChatSurface::NativeChatSurface(wxWindow* parent)
    : wxScrolledWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxVSCROLL | wxBORDER_NONE) {
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    SetScrollRate(0, 1);
    ShowScrollbars(wxSHOW_SB_NEVER, wxSHOW_SB_NEVER);
    body_ = wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT);
    body_.SetPointSize(11);
    mono_ = wxFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
    time_ = body_;
    time_.SetPointSize(8);
    SetFont(body_);
    Bind(wxEVT_PAINT, &NativeChatSurface::Paint, this);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) { dirty_ = true; Refresh(false); event.Skip(); });
    Bind(wxEVT_LEFT_UP, &NativeChatSurface::Click, this);
    Bind(wxEVT_CONTEXT_MENU, &NativeChatSurface::Context, this);
    Bind(wxEVT_MOUSEWHEEL, [this](wxMouseEvent& event) {
        if (loading_) return;
        if (event.GetWheelAxis() != wxMOUSE_WHEEL_VERTICAL) { event.Skip(); return; }
        const int rotation = event.GetWheelRotation();
        if (!rotation) return;
        const int steps = std::max(1, std::abs(rotation) / std::max(1, event.GetWheelDelta()));
        if (history_browser_ && event.GetX() <= FromDIP(24) && !rail_.empty() &&
            history_browser_->Count() > rail_.size()) {
            auto& skip = history_browser_->skip_latest;
            const auto advance = static_cast<size_t>(steps) * 3;
            skip = rotation > 0 ? std::min(skip + advance, history_browser_->Count() - rail_.size())
                               : skip - std::min(skip, advance);
            UpdateRail(GetVirtualSize().y);
            const auto hit = HitAt(event.GetPosition());
            hovered_ = hit.item;
            hovered_action_ = hit.action;
            history_browser_->Hover(hit.action == Action::Rail ? hit.item : kNoChatItem, MonotonicMillis());
            Refresh(false);
            return;
        }
        ScrollBy((rotation > 0 ? -1 : 1) * FromDIP(54) * steps);
    });
    Bind(wxEVT_KEY_DOWN, [this](wxKeyEvent& event) {
        if (loading_) return;
        if (event.HasModifiers()) { event.Skip(); return; }
        const int page = std::max(FromDIP(40), GetClientSize().y - FromDIP(32));
        switch (event.GetKeyCode()) {
            case WXK_UP: ScrollBy(-FromDIP(24)); break;
            case WXK_DOWN: ScrollBy(FromDIP(24)); break;
            case WXK_PAGEUP: ScrollBy(-page); break;
            case WXK_PAGEDOWN: ScrollBy(page); break;
            case WXK_HOME: ScrollBy(-GetVirtualSize().y); break;
            case WXK_END: ScrollToLatest(); break;
            default: event.Skip(); break;
        }
    });
    for (const auto& type : {wxEVT_SCROLLWIN_THUMBTRACK, wxEVT_SCROLLWIN_THUMBRELEASE,
                            wxEVT_SCROLLWIN_LINEUP, wxEVT_SCROLLWIN_LINEDOWN,
                            wxEVT_SCROLLWIN_PAGEUP, wxEVT_SCROLLWIN_PAGEDOWN,
                            wxEVT_SCROLLWIN_TOP, wxEVT_SCROLLWIN_BOTTOM}) {
        Bind(type, [this](wxScrollWinEvent& event) {
            if (loading_) return;
            if (event.GetOrientation() != wxVERTICAL) { event.Skip(); return; }
            const auto type = event.GetEventType();
            const int page = std::max(FromDIP(40), GetClientSize().y - FromDIP(32));
            if (type == wxEVT_SCROLLWIN_LINEUP) ScrollBy(-FromDIP(24));
            else if (type == wxEVT_SCROLLWIN_LINEDOWN) ScrollBy(FromDIP(24));
            else if (type == wxEVT_SCROLLWIN_PAGEUP) ScrollBy(-page);
            else if (type == wxEVT_SCROLLWIN_PAGEDOWN) ScrollBy(page);
            else if (type == wxEVT_SCROLLWIN_TOP) ScrollBy(-GetVirtualSize().y);
            else if (type == wxEVT_SCROLLWIN_BOTTOM) ScrollToLatest();
            else ScrollBy(event.GetPosition() - GetViewStart().y);
        });
    }
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent& event) {
        SetFocus();
        event.Skip();
    });
    Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
        const auto hit = HitAt(event.GetPosition());
        if (hit.item != hovered_ || hit.action != hovered_action_) {
            hovered_ = hit.item;
            hovered_action_ = hit.action;
            if (history_browser_)
                history_browser_->Hover(hit.action == Action::Rail ? hit.item : kNoChatItem, MonotonicMillis());
            UnsetToolTip();
            if (hit.action == Action::Copy) {
                const auto* item = transcript_ ? transcript_->Item(hit.item) : nullptr;
                SetToolTip(T(item && item->kind == ChatItemKind::User ? L"Copy message" : L"Copy reply"));
            } else if (hit.action == Action::Edit) SetToolTip(T(L"Edit message"));
            Refresh(false);
        }
        SetCursor(hit.action == Action::None ? wxCURSOR_ARROW : wxCURSOR_HAND);
    });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
        hovered_ = kNoChatItem;
        hovered_action_ = Action::None;
        if (history_browser_) history_browser_->Hover(kNoChatItem, 0);
        UnsetToolTip();
        SetCursor(wxCURSOR_ARROW);
        Refresh(false);
    });
}
wxColour NativeChatSurface::Foreground() const { return dark_ ? wxColour(236, 237, 240) : wxColour(31, 32, 35); }
wxColour NativeChatSurface::Muted() const { return dark_ ? wxColour(150, 153, 160) : wxColour(103, 106, 112); }
void NativeChatSurface::SetDark(bool dark) {
    if (dark_ == dark) return;
    dark_ = dark;
    row_cache_.clear();
    Changed();
}

void NativeChatSurface::RefreshLocalization() {
    if (!follow_) {
        const int scroll = GetViewStart().y;
        for (const auto index : rows_) {
            const auto found = row_cache_.find(index);
            if (found != row_cache_.end() && found->second.bounds.GetBottom() >= scroll) {
                anchor_item_ = index;
                anchor_offset_ = scroll - found->second.bounds.y;
                break;
            }
        }
    }
    Changed();
}
void NativeChatSurface::SetEmptyTitle(const wxString& title) { empty_title_ = title; Refresh(false); }
void NativeChatSurface::SetLoading(uint64_t started) { loading_ = started; Refresh(false); }
void NativeChatSurface::SetTranscript(Transcript* transcript, bool reset_scroll) {
    if (history_browser_ && transcript != &history_browser_->transcript) {
        live_transcript_ = transcript;
        if (reset_scroll) history_browser_->Reset();
        else if (history_browser_->page.end_turn) return;
    }
    if (transcript != transcript_) {
        transcript_ = transcript; markdown_.clear(); row_cache_.clear();
        ui::ClearNativeMarkdownLinks(this);
        ResetThumbnails();
    }
    hovered_ = kNoChatItem;
    hovered_action_ = Action::None;
    UnsetToolTip();
    earlier_loading_ = false;
    if (reset_scroll) { anchor_item_ = kNoChatItem; follow_ = true; Scroll(0, 0); }
    dirty_ = true;
    Refresh(false);
}
void NativeChatSurface::EnableHistory(HistoryBrowser::Source source) {
    history_browser_ = std::make_unique<HistoryBrowser>(std::move(source));
}
void NativeChatSurface::CloseHistory() {
    if (!history_browser_ || !history_browser_->page.end_turn) return;
    history_browser_->page = {};
    history_browser_->transcript.Clear();
    history_browser_->Hover(kNoChatItem, 0);
    SetTranscript(live_transcript_);
    loading_ = 0;
}
void NativeChatSurface::BrowseTurn(size_t turn) {
    auto& browser = *history_browser_;
    browser.Update();
    if (!live_transcript_) return;
    browser.Reveal(turn, rail_.size());
    const auto live_first = browser.Turn(live_transcript_->HistoryFirstMessage());
    if (turn >= live_first) {
        CloseHistory();
        JumpToTurn(turn);
        return;
    }
    if (browser.page.Contains(turn) && !browser.transcript.Loading()) {
        JumpToTurn(turn);
        return;
    }
    browser.Open(turn, live_first, {UnixTimeSeconds(), MonotonicMillis()});
    SetTranscript(&browser.transcript);
    follow_ = false;
    SetLoading(MonotonicMillis());
}
void NativeChatSurface::JumpToTurn(size_t turn) {
    if (!transcript_) return;
    size_t ordinal = history_browser_->Turn(transcript_->HistoryFirstMessage());
    const auto& items = transcript_->Items();
    for (size_t index = 0; index < items.size(); ++index) {
        if (items[index].kind != ChatItemKind::User || ordinal++ != turn) continue;
        if (transcript_ == live_transcript_ && index == transcript_->LastUserIndex()) {
            ScrollToLatest();
            return;
        }
        follow_ = false;
        anchor_item_ = index;
        anchor_offset_ = -FromDIP(12);
        Changed();
        return;
    }
}
void NativeChatSurface::SetEarlierLoading(bool loading) {
    earlier_loading_ = loading;
    Refresh(false);
}
void NativeChatSurface::RequestEarlier() {
    if (history_browser_ && transcript_ && !loading_) {
        history_browser_->Update();
        const auto first = history_browser_->Turn(transcript_->HistoryFirstMessage());
        if (first) BrowseTurn(first - 1);
        return;
    }
    if (!loading_ && !earlier_loading_ && transcript_ && transcript_->HistoryFirstMessage() && load_earlier) {
        follow_ = false;
        load_earlier();
    }
}
void NativeChatSurface::PrependTranscript(Transcript* transcript, size_t added_items) {
    size_t anchor = kNoChatItem;
    int offset = 0;
    const int scroll = GetViewStart().y;
    for (const auto index : rows_) {
        const auto found = row_cache_.find(index);
        if (found != row_cache_.end() && found->second.bounds.GetBottom() >= scroll) {
            anchor = index + added_items;
            offset = scroll - found->second.bounds.y;
            break;
        }
    }
    SetTranscript(transcript);
    follow_ = false;
    anchor_item_ = anchor;
    anchor_offset_ = offset;
}
void NativeChatSurface::Changed() {
    if (transcript_ && generation_ != transcript_->Generation()) {
        generation_ = transcript_->Generation();
        markdown_.clear();
        row_cache_.clear();
        ui::ClearNativeMarkdownLinks(this);
        ResetThumbnails();
    }
    dirty_ = true;
    Refresh(false);
}
void NativeChatSurface::ScrollToLatest() {
    CloseHistory();
    if (history_browser_) history_browser_->skip_latest = 0;
    follow_ = true;
    dirty_ = true;
    Scroll(0, std::max(0, GetVirtualSize().y - GetClientSize().y));
    Refresh(false);
}
void NativeChatSurface::ScrollBy(int pixels) {
    const int bottom = std::max(0, GetVirtualSize().y - GetClientSize().y);
    if (history_browser_ && transcript_) {
        const auto& page = history_browser_->page;
        const size_t first = history_browser_->Turn(transcript_->HistoryFirstMessage());
        if (pixels < 0 && GetViewStart().y <= 0 && first) { BrowseTurn(first - 1); return; }
        if (pixels > 0 && GetViewStart().y >= bottom && page.end_turn) { BrowseTurn(page.end_turn); return; }
    }
    const int next = std::clamp(GetViewStart().y + pixels, 0, bottom);
    follow_ = pixels >= 0 && next >= bottom - FromDIP(2);
    hovered_ = kNoChatItem;
    hovered_action_ = Action::None;
    UnsetToolTip();
    SetCursor(wxCURSOR_ARROW);
    Scroll(0, next);
    Refresh(false);
    if (!history_browser_ && pixels < 0 && next == 0) RequestEarlier();
}
void NativeChatSurface::Tick() {
    if (PollThumbnails()) Refresh(false);
    const auto now = MonotonicMillis();
    if (history_browser_) {
        auto& browser = *history_browser_;
        if (browser.UpdatePreview(now)) Refresh(false);
        if (browser.page.end_turn && browser.transcript.Loading() && browser.Step()) {
            SetLoading(0);
            JumpToTurn(browser.target);
        }
    }
    if (((loading_ && now - loading_ >= 150) || (transcript_ && transcript_->NeedsAnimation()) || earlier_loading_) &&
        now / 100 != elapsed_frame_) {
        elapsed_frame_ = now / 100;
        Refresh(false);
    }
}

void NativeChatSurface::ResetThumbnails() {
    thumbnails_.Clear(); thumbnail_paths_.clear();
    thumbnail_bitmaps_.clear(); thumbnail_lru_.clear();
}

bool NativeChatSurface::PollThumbnails() {
    if (!thumbnails_.Poll()) return false;
    for (const auto& path : thumbnail_paths_) {
        const auto bitmap = thumbnails_.Get(path, thumbnail_edge_);
        if (!bitmap.IsOk() || thumbnail_bitmaps_.count(path)) continue;
        if (thumbnail_lru_.size() >= 32) {
            thumbnail_bitmaps_.erase(thumbnail_lru_.front());
            thumbnail_lru_.pop_front();
        }
        thumbnail_bitmaps_[path] = bitmap;
        thumbnail_lru_.push_back(path);
    }
    return true;
}

wxBitmap NativeChatSurface::Thumbnail(const std::string& path, const wxSize& bounds) const {
    const auto found = thumbnail_bitmaps_.find(path);
    if (found == thumbnail_bitmaps_.end() || bounds.x <= 0 || bounds.y <= 0) return {};
    const auto& bitmap = found->second;
    const double scale = std::min({1.0, static_cast<double>(bounds.x) / bitmap.GetWidth(),
                                  static_cast<double>(bounds.y) / bitmap.GetHeight()});
    if (scale >= 1.0) return bitmap;
    return wxBitmap(bitmap.ConvertToImage().Scale(std::max(1, static_cast<int>(bitmap.GetWidth() * scale)),
        std::max(1, static_cast<int>(bitmap.GetHeight() * scale)), wxIMAGE_QUALITY_NORMAL));
}

wxString NativeChatSurface::ActivityText() const {
    if (!transcript_) return {};
    const auto* status = transcript_->Item(activity_status_);
    if (status && !status->finished && !status->text.empty()) return U(status->text);
    const auto* item = transcript_->Item(activity_item_);
    if (item && item->visible && !item->finished) {
        if (item->kind == ChatItemKind::Reasoning) return ReasoningLine(item->text);
        if (item->kind == ChatItemKind::Tool)
            return ToolLabel(*item) + "  \u00b7  " + U(ToolText(*item, ToolTextKind::Summary)) +
                "  \u00b7  " + Duration(Transcript::Elapsed(*item, MonotonicMillis()));
    }
    if (status && !status->finished)
        return T(L"Processing") + "  \u00b7  " +
            Duration(Transcript::Elapsed(*status, MonotonicMillis()), DurationKind::Turn);
    return {};
}

void NativeChatSurface::Reflow(wxDC& dc) {
    if (transcript_ && generation_ != transcript_->Generation()) {
        generation_ = transcript_->Generation();
        row_cache_.clear();
        markdown_.clear();
        ui::ClearNativeMarkdownLinks(this);
        ResetThumbnails();
    }
    rows_.clear();
    rail_.clear();
    activity_item_ = activity_status_ = kNoChatItem;
    earlier_bounds_ = {};
    dirty_ = false;
    if (layout_width_ != GetClientSize().x) row_cache_.clear();
    layout_width_ = GetClientSize().x;
    const int available_left = std::min(FromDIP(46), layout_width_ / 4);
    const int available = std::max(1, layout_width_ - available_left - std::min(FromDIP(22), layout_width_ / 4));
    const int width = std::min(FromDIP(920), available);
    const int left = available_left + (available - width) / 2;
    int top = FromDIP(18);
    if (transcript_) {
        const auto& items = transcript_->Items();
        if (transcript_->HistoryFirstMessage() && load_earlier) {
            earlier_bounds_ = wxRect(left, top, width, FromDIP(32));
            top += FromDIP(42);
        }
        for (size_t index = 0; index < items.size(); ++index) {
            const auto& item = items[index];
            if (!item.visible || item.finished) continue;
            if (item.kind == ChatItemKind::TurnStatus) activity_status_ = index;
            if (item.kind == ChatItemKind::Reasoning || item.kind == ChatItemKind::Tool) activity_item_ = index;
        }
        const auto groups = transcript_->Groups(MonotonicMillis());
        const auto last_user = history_browser_ && history_browser_->page.end_turn
            ? kNoChatItem : transcript_->LastUserIndex();
        for (size_t index = 0; index < items.size(); ++index) {
            const auto& item = items[index];
            if (!item.visible || groups[index].hidden ||
                (item.kind == ChatItemKind::Assistant && !HasAssistantText(item.text))) continue;
            if (item.kind == ChatItemKind::TurnStatus && !item.finished && item.text.empty()
                && activity_item_ != kNoChatItem) continue;
            const auto cached = row_cache_.find(index);
            const bool message = item.kind == ChatItemKind::Assistant || item.kind == ChatItemKind::User;
            const bool editable = item.kind == ChatItemKind::User && index == last_user && edit;
            if (message && cached != row_cache_.end() && cached->second.revision == item.revision
                && (!cached->second.edit.IsEmpty()) == editable) {
                auto& row = cached->second;
                const int offset = top - row.bounds.y;
                if (offset != 0) {
                    for (auto* rect : {&row.bounds, &row.message, &row.bubble, &row.header,
                                       &row.group_header, &row.copy, &row.edit, &row.time, &row.separator})
                        if (!rect->IsEmpty()) rect->Offset(0, offset);
                    for (auto& image : row.images) image.Offset(0, offset);
                    for (auto& part : row.fragments) part.origin.y += offset;
                    for (auto& panel : row.panels) panel.bounds.Offset(0, offset);
                }
                top += row.bounds.height;
                rows_.push_back(index);
                continue;
            }
            ui::ClearNativeMarkdownLinks(this, index);
            Row row;
            row.item = index;
            row.revision = item.revision;
            const int before = top;
            const bool status = item.kind == ChatItemKind::TurnStatus;
            const bool tool = item.kind == ChatItemKind::Tool;
            const auto layout_images = [&](int x, int available, bool right_align) {
                if (item.image_paths.empty()) return;
                const int tile_width = std::min(std::max(1, available), FromDIP(144));
                const int tile_height = FromDIP(108), gap = FromDIP(8);
                const size_t columns = std::max(1, (available + gap) / (tile_width + gap));
                for (size_t start = 0; start < item.image_paths.size(); start += columns) {
                    const size_t count = std::min(columns, item.image_paths.size() - start);
                    const int image_left = right_align ? x + available - static_cast<int>(count) * (tile_width + gap) + gap : x;
                    for (size_t image = 0; image < count; ++image)
                        row.images.emplace_back(image_left + static_cast<int>(image) * (tile_width + gap),
                                                top, tile_width, tile_height);
                    top += tile_height + gap;
                }
            };
            row.user = item.kind == ChatItemKind::User;
            if (row.user && item.timeline_separator && item.timestamp) {
                row.timeline = wxString(FormatTimelineTime(item.timestamp));
                row.separator = wxRect(left, top, width, FromDIP(24));
                top += FromDIP(34);
            }
            if (status || tool || item.kind == ChatItemKind::Reasoning) {
                dc.SetFont(body_);
                row.expanded = item.expanded;
                if (status) {
                    const auto elapsed = Transcript::Elapsed(item, MonotonicMillis());
                    auto label = T(item.paused ? L"Interrupted" : item.finished ? L"Processed" : L"Processing");
                    if (elapsed) label = wxString(i18n::Format(item.paused ? L"Interrupted {elapsed}"
                        : item.finished ? L"Processed {elapsed}" : L"Processing {elapsed}",
                        {{L"elapsed", Duration(elapsed, DurationKind::Turn).ToStdWstring()}}));
                    if (item.finished) row.fragments.push_back({wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
                        std::max(1, width - FromDIP(24))),
                        {left, top + std::max(0, (FromDIP(22) - dc.GetTextExtent(label).y) / 2)},
                        body_, Muted(), false, true});
                    if (item.finished) row.header = wxRect(left, top, width, FromDIP(22));
                    top += FromDIP(32);
                } else if (!tool) {
                    row.fragments.push_back({wxControl::Ellipsize(ReasoningLine(item.text), dc,
                        wxELLIPSIZE_END, width), {left, top}, body_, Muted(), !item.finished});
                    top += FromDIP(34);
                } else {
                    const auto& group = groups[index];
                    if (group.leader) {
                        row.group_expanded = !item.group_collapsed;
                        row.group_active = group.active;
                        row.group_failed = group.failed;
                        row.reasoning = group.reasoning;
                        row.group_header = wxRect(left, top, width, FromDIP(28));
                        auto label = GroupLabel(group, item.tool_name);
                        if (group.active) {
                            for (size_t member = index; member <= group.last_member && member < items.size(); ++member) {
                                if (items[member].kind == ChatItemKind::Tool && !items[member].finished) {
                                    row.active_tool = member;
                                    label += "  \u00b7  " + U(ToolText(items[member], ToolTextKind::Summary));
                                    break;
                                }
                            }
                        }
                        if (group.elapsed_ms && !group.active) label += "  \u00b7  " + Duration(group.elapsed_ms);
                        if (!group.active) row.fragments.push_back({wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
                            std::max(1, width - FromDIP(128))), {left + FromDIP(34), top + FromDIP(3)},
                            body_, group.failed ? wxColour(242, 128, 128) : Muted(), group.active, true});
                        top += FromDIP(28);
                        if (!item.group_collapsed) top += FromDIP(4);
                    }
                    if (!group.leader || !item.group_collapsed) {
                        const int indent = group.leader || group.indented ? FromDIP(18) : 0;
                        row.header = wxRect(left + indent, top, width - indent, FromDIP(28));
                        auto label = ToolLabel(item);
                        const auto summary = U(ToolText(item, ToolTextKind::Summary));
                        if (!summary.empty()) label += "  \u00b7  " + summary;
                        row.fragments.push_back({wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
                            std::max(1, row.header.width - FromDIP(126))),
                            {row.header.x + FromDIP(34), top + FromDIP(3)}, body_,
                            item.finished && !item.success ? wxColour(242, 128, 128) : Muted(), !item.finished, true});
                        dc.SetFont(time_);
                        const auto duration = wxControl::Ellipsize(Duration(Transcript::Elapsed(item, MonotonicMillis())),
                            dc, wxELLIPSIZE_END, FromDIP(58));
                        if (item.finished) row.fragments.push_back({duration, {row.header.GetRight() + 1 - FromDIP(30)
                            - dc.GetTextExtent(duration).x,
                            top + std::max(0, (FromDIP(28) - dc.GetTextExtent(duration).y) / 2)},
                            time_, dark_ ? wxColour(112, 115, 122) : wxColour(135, 138, 144)});
                        top += FromDIP(28);
                        if (item.expanded && !item.image_paths.empty()) {
                            top += FromDIP(3);
                            layout_images(row.header.x + FromDIP(12), row.header.width - FromDIP(24), false);
                        }
                        if (item.expanded && !(ImageTool(item.tool_name) && !item.image_paths.empty())) {
                            top += FromDIP(3);
                            const auto start = row.fragments.size();
                            LayoutText(dc, row, PlainLines(ToolText(item, ToolTextKind::Details),
                                markdown::MarkdownStyle::Code), row.header.x + FromDIP(12),
                                std::max(1, row.header.width - FromDIP(28)), top, true);
                            if (!item.success) for (size_t part = start; part < row.fragments.size(); ++part)
                                row.fragments[part].color = wxColour(242, 128, 128);
                            top += FromDIP(8);
                        }
                    }
                    top += FromDIP(8);
                }
            } else if (item.kind == ChatItemKind::UsageStats) {
                top += FromDIP(7);
                LayoutMarkdown(dc, row, PlainLines(NativeUsageText(item).ToStdString(wxConvUTF8),
                    markdown::MarkdownStyle::Muted), left + FromDIP(10), std::max(1, width - FromDIP(20)), top);
                top += FromDIP(7);
                row.bubble = wxRect(left, before, width, top - before);
                row.message = row.bubble;
                top += FromDIP(10);
            } else if (item.kind == ChatItemKind::Compaction || item.kind == ChatItemKind::Reconnect) {
                dc.SetFont(body_);
                const int height = std::max(FromDIP(22), dc.GetTextExtent("Ag").y);
                const auto label = wxControl::Ellipsize(item.kind == ChatItemKind::Reconnect
                    ? U(item.text) : T(CompactionLabel(item.finished, item.success)),
                    dc, wxELLIPSIZE_END, std::max(1, width - FromDIP(22)));
                row.fragments.push_back({label, {left + FromDIP(22),
                    top + std::max(0, (height - dc.GetTextExtent(label).y) / 2)}, body_, Muted(), false, true});
                row.message = wxRect(left, top, width, height);
                top += height + FromDIP(6);
            } else {
                if (row.user) layout_images(left, width, true);
                const auto display_text = row.user ? ui::UserMessageText(item.text) : std::string{};
                const int message_top = top;
                int message_width = width;
                if (row.user) {
                    dc.SetFont(body_);
                    int measured = 0;
                    for (const auto& part : wxSplit(U(display_text), '\n'))
                        measured = std::max(measured, dc.GetTextExtent(part).x);
                    const int maximum = std::min(width, std::max(FromDIP(180), width * 78 / 100));
                    message_width = std::min(maximum, std::max(FromDIP(120), measured + FromDIP(30)));
                    top += FromDIP(11);
                }
                const int message_left = row.user ? left + width - message_width : left;
                if (row.user) {
                    LayoutText(dc, row, PlainLines(display_text, markdown::MarkdownStyle::Body),
                               message_left + FromDIP(14), std::max(1, message_width - FromDIP(28)), top, false);
                    top = std::max(top, message_top + FromDIP(31)) + FromDIP(11);
                    row.bubble = wxRect(message_left, message_top, message_width, top - message_top);
                } else {
                    auto& message = markdown_[index];
                    if (message.source_bytes > item.text.size() ||
                        (message.finished && (!item.finished || message.source_bytes != item.text.size()))) message = {};
                    // Finish releases the parser input; track received bytes independently.
                    if (message.source_bytes < item.text.size()) {
                        message.parser.Append(std::string_view(item.text).substr(message.source_bytes));
                        message.source_bytes = item.text.size();
                    }
                    if (item.finished && !message.finished) {
                        message.parser.Finish();
                        message.finished = true;
                    }
                    const auto& stable = message.parser.StableLines();
                    LayoutMarkdown(dc, row, stable, message_left, message_width, top);
                    if (!message.parser.TailSource().empty()) {
                        if (!stable.empty()) top += FromDIP(8);
                        LayoutMarkdown(dc, row, *message.parser.TailLines(), message_left, message_width, top);
                    }
                    top = std::max(top, before + FromDIP(20));
                }
                if (item.kind == ChatItemKind::Error || item.kind == ChatItemKind::Quota)
                    for (auto& part : row.fragments) part.color = wxColour(225, 103, 103);
                if ((item.kind == ChatItemKind::Assistant && item.final_assistant) || row.user) {
                    row.timestamp = item.timestamp ?
                        wxDateTime(static_cast<time_t>(item.timestamp)).Format("%H:%M") : wxString{};
                    const int meta_top = top + FromDIP(3);
                    const int right = left + width;
                    if (editable) row.edit = wxRect(right - FromDIP(18), meta_top + FromDIP(1),
                                                    FromDIP(18), FromDIP(16));
                    const int copy_left = row.user ? right - FromDIP(editable ? 42 : 18) : left;
                    if (row.user || item.finished)
                        row.copy = wxRect(copy_left, meta_top + FromDIP(1), FromDIP(18), FromDIP(16));
                    dc.SetFont(time_);
                    const int time_width = dc.GetTextExtent(row.timestamp).x;
                    const int time_right = row.user ? copy_left - FromDIP(8) : right;
                    row.time = wxRect(time_right - time_width, meta_top, time_width, FromDIP(18));
                    top = meta_top + FromDIP(18);
                }
                row.message = wxRect(message_left, message_top, message_width, top - message_top);
                top += FromDIP(18);
            }
            if (row.message.IsEmpty()) row.message = wxRect(left, before, width, top - before);
            row.bounds = wxRect(left, before, width, std::max(26, top - before));
            row_cache_[index] = std::move(row);
            rows_.push_back(index);
        }
    }
    SetVirtualSize(GetClientSize().x, top + FromDIP(28));
    UpdateRail(top + FromDIP(4));
    if (anchor_item_ != kNoChatItem) {
        const auto anchor = row_cache_.find(anchor_item_);
        if (anchor != row_cache_.end()) Scroll(0, std::clamp(anchor->second.bounds.y + anchor_offset_, 0,
            std::max(0, GetVirtualSize().y - GetClientSize().y)));
        anchor_item_ = kNoChatItem;
    } else if (follow_) Scroll(0, std::max(0, GetVirtualSize().y - GetClientSize().y));
}

void NativeChatSurface::UpdateRail(int content_height) {
    const int viewport = GetClientSize().y;
    if (!transcript_ || (!history_browser_ && content_height <= viewport) || viewport <= FromDIP(20)) return;
    for (const auto index : rows_) {
        const auto* item = transcript_->Item(index);
        const auto cached = row_cache_.find(index);
        if (item && item->kind == ChatItemKind::User && cached != row_cache_.end()) {
            const int start = cached->second.bounds.y;
            if (!rail_.empty()) rail_.back().content_end = start;
            rail_.push_back({index, start, content_height, 0});
        }
    }
    if (history_browser_) {
        history_browser_->Update();
        const auto first = history_browser_->Turn(transcript_->HistoryFirstMessage());
        const auto total = std::max(history_browser_->Count(), first + rail_.size());
        const int pitch = FromDIP(12);
        const auto slots = HistoryRailLayout(total, history_browser_->skip_latest, viewport, FromDIP(10), pitch);
        auto rendered = std::move(rail_);
        rail_.clear();
        for (size_t i = 0; i < slots.count; ++i) {
            const auto turn = slots.first + i;
            const bool present = turn >= first && turn - first < rendered.size();
            rail_.push_back({turn, present ? rendered[turn - first].content_top : -1,
                             present ? rendered[turn - first].content_end : -1, slots.top + static_cast<int>(i) * pitch});
        }
        return;
    }
    if (rail_.empty()) return;
    const int margin = FromDIP(10);
    const int available = std::max(1, viewport - margin * 2);
    const auto compact = static_cast<int64_t>(rail_.size() - 1) * FromDIP(14);
    const int top = compact <= available ? std::max(margin, viewport / 2 - static_cast<int>(compact) / 2) : margin;
    const int bottom = compact <= available ? std::min(viewport - margin, top + static_cast<int>(compact))
                                            : viewport - margin;
    const int span = bottom - top;
    const int gap = std::max(FromDIP(2), rail_.size() > 1 ? span / static_cast<int>(rail_.size() - 1) : 0);
    int previous = top - gap;
    for (auto& dash : rail_) {
        const auto scaled = (static_cast<int64_t>(dash.content_top) * span + content_height / 2) / content_height;
        dash.y = std::min(bottom, std::max(top + static_cast<int>(scaled), previous + gap));
        previous = dash.y;
    }
}

bool NativeChatSurface::IsTurnVisible(const RailDash& dash) const {
    return dash.content_top >= 0 &&
        ConversationTurnVisible(dash.content_top, dash.content_end, GetViewStart().y, GetClientSize().y);
}

void NativeChatSurface::DrawRail(wxDC& dc) {
    if (rail_.empty()) return;
    const auto saved_origin = dc.GetDeviceOrigin();
    dc.SetDeviceOrigin(0, 0);
    int hovered_y = -1;
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (const auto& dash : rail_) {
        const bool hovered = hovered_action_ == Action::Rail && hovered_ == dash.item;
        const bool active = !hovered && IsTurnVisible(dash);
        const int length = FromDIP(hovered ? 18 : active ? 13 : 8);
        const int thickness = std::max(1, FromDIP(2));
        dc.SetBrush(wxBrush(hovered ? (dark_ ? wxColour(244, 245, 248) : wxColour(72, 74, 79))
            : active ? (dark_ ? wxColour(150, 153, 160) : wxColour(112, 115, 120))
                     : (dark_ ? wxColour(74, 76, 82) : wxColour(180, 183, 188))));
        dc.DrawRectangle(FromDIP(6), dash.y - thickness / 2, length, thickness);
        if (hovered) hovered_y = dash.y;
    }
    const auto* item = transcript_ ? transcript_->Item(hovered_) : nullptr;
    if (hovered_y >= 0 && (history_browser_ || item)) {
        const auto* browser = history_browser_.get();
        const bool skeleton = browser && !browser->PreviewReady() && !browser->PreviewUnavailable();
        auto preview = browser ? (browser->PreviewUnavailable() ? T(L"Preview unavailable")
            : U(browser->prompt + "\n\n" + browser->reply)) : U(item->text);
        preview.Trim().Trim(false);
        if (!browser) {
            const auto newline = preview.find_first_of("\r\n");
            if (newline != wxString::npos) preview = preview.Left(newline) + "\u2026";
            if (preview.length() > 72) preview = preview.Left(72) + "\u2026";
        }
        const int padding_x = FromDIP(12), padding_y = FromDIP(8);
        const int max_width = std::max(1, std::min(FromDIP(320), GetClientSize().x - FromDIP(48)));
        Row tooltip;
        int height = 0, width = 0;
        const auto utf8 = preview.ToUTF8();
        LayoutText(dc, tooltip, PlainLines(std::string(utf8.data() ? utf8.data() : "", utf8.length()), markdown::MarkdownStyle::Body),
                   0, max_width, height, false);
        for (const auto& part : tooltip.fragments) {
            dc.SetFont(part.font);
            width = std::max(width, part.origin.x + dc.GetTextExtent(part.text).x);
        }
        if (skeleton) { width = max_width; height = FromDIP(34); }
        const int box_width = std::min(max_width, width) + padding_x * 2;
        const int box_height = std::clamp(height + padding_y * 2, FromDIP(30),
            std::max(FromDIP(30), std::min(FromDIP(180), GetClientSize().y - FromDIP(16))));
        const int left = std::max(FromDIP(12), std::min(FromDIP(36),
            GetClientSize().x - box_width - FromDIP(12)));
        const int top = std::max(FromDIP(8), std::min(hovered_y - box_height / 2,
            GetClientSize().y - box_height - FromDIP(8)));
        dc.SetPen(wxPen(dark_ ? wxColour(78, 79, 85) : wxColour(196, 198, 201)));
        dc.SetBrush(wxBrush(dark_ ? wxColour(46, 47, 51) : wxColour(255, 255, 255)));
        dc.DrawRoundedRectangle(left, top, box_width, box_height, FromDIP(10));
        wxDCClipper clip(dc, left + padding_x, top + padding_y, max_width, box_height - padding_y * 2);
        if (skeleton) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(dark_ ? wxColour(78, 79, 85) : wxColour(218, 220, 224)));
            dc.DrawRoundedRectangle(left + padding_x, top + padding_y + FromDIP(4), max_width, FromDIP(6), FromDIP(3));
            dc.DrawRoundedRectangle(left + padding_x, top + padding_y + FromDIP(20), max_width * 2 / 3,
                                    FromDIP(6), FromDIP(3));
        }
        for (const auto& part : tooltip.fragments) {
            dc.SetFont(part.font);
            dc.SetTextForeground(Foreground());
            dc.DrawText(part.text, part.origin + wxPoint(left + padding_x, top + padding_y));
        }
    }
    dc.SetDeviceOrigin(saved_origin.x, saved_origin.y);
}

void NativeChatSurface::Paint(wxPaintEvent&) {
    wxAutoBufferedPaintDC dc(this);
    dc.SetBackground(wxBrush(dark_ ? wxColour(28, 29, 31) : wxColour(250, 250, 250)));
    dc.Clear();
    if (dirty_ || layout_width_ != GetClientSize().x) Reflow(dc);
    if (update_jump_visibility) {
        const bool browsing = history_browser_ && history_browser_->page.end_turn != 0;
        const bool above_bottom = !loading_ && transcript_ && transcript_->Count() &&
            GetViewStart().y < GetVirtualSize().y - GetClientSize().y;
        update_jump_visibility(browsing || above_bottom);
    }
    if (loading_) {
        if (MonotonicMillis() - loading_ < 150) return;
        const int cx = GetClientSize().x / 2, cy = GetClientSize().y / 2;
        const int frame = static_cast<int>(MonotonicMillis() / 100 % 8);
        for (int index = 0; index < 8; ++index) {
            const double angle = index * 3.141592653589793 / 4;
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(index == frame ? Foreground() : Muted()));
            dc.DrawCircle(cx + static_cast<int>(12 * std::cos(angle)), cy + static_cast<int>(12 * std::sin(angle)), 2);
        }
        return;
    }
    if (rows_.empty() && earlier_bounds_.IsEmpty()) {
        if (empty_title_.empty()) return;
        const int center_x = GetClientSize().x / 2, center_y = GetClientSize().y / 2;
        {
            wxDCClipper clip(dc, wxRect(0, center_y - FromDIP(52), GetClientSize().x, FromDIP(40)));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.SetPen(wxPen(dark_ ? wxColour(67, 68, 72) : wxColour(214, 216, 219)));
            dc.DrawRoundedRectangle(center_x - FromDIP(23), center_y - FromDIP(55),
                                     FromDIP(46), FromDIP(46), FromDIP(7));
            dc.SetFont(mono_); dc.SetTextForeground(Muted());
            dc.DrawLabel(">_", {center_x - FromDIP(20), center_y - FromDIP(44),
                                FromDIP(40), FromDIP(24)}, wxALIGN_CENTER);
        }
        auto title_font = body_;
        title_font.SetPointSize(15);
        title_font.SetWeight(wxFONTWEIGHT_SEMIBOLD);
        dc.SetFont(title_font); dc.SetTextForeground(Foreground());
        dc.DrawLabel(wxControl::Ellipsize(empty_title_, dc, wxELLIPSIZE_END,
                         std::max(1, GetClientSize().x - FromDIP(24))),
                     {0, center_y - FromDIP(8), GetClientSize().x, FromDIP(32)}, wxALIGN_CENTER);
        return;
    }
    PrepareDC(dc);
    const wxPoint origin = CalcUnscrolledPosition(wxPoint(0, 0));
    const wxRect viewport(origin, GetClientSize());
    // Retain 32 decoded previews independently of the decoder's request generation.
    // Navigation cancels stale queued work without discarding already displayed images.
    const int thumbnail_edge = std::min(240, FromDIP(144));
    if (thumbnail_edge_ != thumbnail_edge) {
        ResetThumbnails();
        thumbnail_edge_ = thumbnail_edge;
    }
    std::set<std::string> visible_images;
    for (const auto index : rows_) {
        const auto& row = row_cache_.at(index);
        const auto* item = transcript_ ? transcript_->Item(index) : nullptr;
        if (!item || !row.bounds.Intersects(viewport)) continue;
        for (size_t image_index = 0; image_index < row.images.size(); ++image_index) {
            const auto& image = row.images[image_index];
            if (image.Intersects(viewport) && image_index < item->image_paths.size())
                visible_images.insert(item->image_paths[image_index]);
        }
    }
    if (visible_images != thumbnail_paths_) {
        PollThumbnails();
        thumbnails_.Clear();
        thumbnail_paths_ = std::move(visible_images);
        for (const auto& path : thumbnail_paths_) {
            if (thumbnail_bitmaps_.count(path)) {
                thumbnail_lru_.erase(std::remove(thumbnail_lru_.begin(), thumbnail_lru_.end(), path), thumbnail_lru_.end());
                thumbnail_lru_.push_back(path);
                continue;
            }
            std::error_code error;
            const auto bytes = fs::file_size(NativePath(path), error);
            // Avoid wxImage's empty-stream diagnostics and consequent idle repaint.
            if (!error && bytes != 0) thumbnails_.Request(path, thumbnail_edge_);
        }
    }
    if (earlier_bounds_.Intersects(viewport)) {
        dc.SetFont(body_);
        dc.SetTextForeground(earlier_loading_ ? Muted() : Foreground());
        dc.DrawLabel(T(earlier_loading_ ? L"Loading earlier messages..." : L"Load earlier messages"),
                     earlier_bounds_, wxALIGN_CENTER);
    }
    for (const auto index : rows_) {
        const auto cached = row_cache_.find(index);
        if (cached == row_cache_.end()) continue;
        const auto& row = cached->second;
        if (!row.bounds.Intersects(viewport)) continue;
        const auto* item = transcript_ ? transcript_->Item(row.item) : nullptr;
        if (!item) continue;
        if (!row.images.empty()) {
            dc.SetFont(time_);
            dc.SetTextForeground(Muted());
            for (size_t image = 0; image < row.images.size() && image < item->image_paths.size(); ++image) {
                const auto& bounds = row.images[image];
                if (!bounds.Intersects(viewport)) continue;
                dc.SetBrush(wxBrush(dark_ ? wxColour(52, 53, 57) : wxColour(232, 233, 235)));
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.DrawRoundedRectangle(bounds, FromDIP(8));
                const auto bitmap = Thumbnail(item->image_paths[image], bounds.GetSize());
                if (bitmap.IsOk())
                    dc.DrawBitmap(bitmap, bounds.x + (bounds.width - bitmap.GetWidth()) / 2,
                                  bounds.y + (bounds.height - bitmap.GetHeight()) / 2, true);
                else {
                    const auto label = U(PathToUtf8(NativePath(item->image_paths[image]).filename()));
                    dc.DrawLabel(wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
                        std::max(1, bounds.width - FromDIP(8))), bounds, wxALIGN_CENTER);
                }
            }
        }
        if (row.user) {
            dc.SetFont(time_); dc.SetTextForeground(Muted());
            if (!row.separator.IsEmpty()) dc.DrawLabel(row.timeline, row.separator, wxALIGN_CENTER);
            dc.SetBrush(wxBrush(dark_ ? wxColour(52, 53, 57) : wxColour(232, 233, 235)));
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.DrawRoundedRectangle(row.bubble, FromDIP(12));
        } else if (item->kind == ChatItemKind::UsageStats) {
            dc.SetBrush(wxBrush(dark_ ? wxColour(28, 29, 31) : wxColour(250, 250, 250)));
            dc.SetPen(wxPen(dark_ ? wxColour(111, 190, 140) : wxColour(39, 132, 82)));
            dc.DrawRoundedRectangle(row.bubble, FromDIP(4));
        } else if (item->kind == ChatItemKind::Compaction) {
            const int x = row.message.x + FromDIP(11), y = row.message.y + row.message.height / 2;
            dc.SetPen(wxPen(row.item == hovered_ ? Foreground() : Muted()));
            dc.DrawLine(x - FromDIP(6), y - FromDIP(5), x + FromDIP(6), y - FromDIP(5));
            dc.DrawLine(x - FromDIP(6), y + FromDIP(5), x + FromDIP(6), y + FromDIP(5));
            dc.DrawLine(x, y - FromDIP(8), x, y + FromDIP(8));
        }
        PaintText(dc, row);
        if (row.group_active && !row.group_header.IsEmpty()) {
            const auto* reasoning = transcript_->Item(row.reasoning);
            const auto* tool = transcript_->Item(row.active_tool);
            wxString label;
            if (reasoning && reasoning->visible && !reasoning->finished)
                label = ReasoningLine(reasoning->text);
            else if (tool && !tool->finished)
                label = ToolLabel(*tool) + "  \u00b7  " + U(ToolText(*tool, ToolTextKind::Summary)) +
                    "  \u00b7  " + Duration(Transcript::Elapsed(*tool, MonotonicMillis()));
            dc.SetFont(body_);
            dc.SetTextForeground(Muted());
            auto bounds = row.group_header;
            bounds.x += FromDIP(34); bounds.width = std::max(1, bounds.width - FromDIP(68));
            dc.DrawLabel(wxControl::Ellipsize(label, dc, wxELLIPSIZE_END, bounds.width),
                         bounds, wxALIGN_CENTER_VERTICAL);
        }
        if (item->kind == ChatItemKind::TurnStatus && !item->finished) {
            dc.SetFont(body_);
            dc.SetTextForeground(Muted());
            dc.DrawLabel(wxControl::Ellipsize(ActivityText(), dc, wxELLIPSIZE_END,
                std::max(1, row.message.width - FromDIP(24))), row.message, wxALIGN_TOP);
        }
        if (item->kind == ChatItemKind::Tool && !item->finished && !row.header.IsEmpty()) {
            dc.SetFont(time_);
            dc.SetTextForeground(Muted());
            const auto duration = wxControl::Ellipsize(Duration(Transcript::Elapsed(*item, MonotonicMillis())),
                                                       dc, wxELLIPSIZE_END, FromDIP(58));
            dc.DrawText(duration, row.header.GetRight() + 1 - FromDIP(30) - dc.GetTextExtent(duration).x,
                        row.header.y + std::max(0, (row.header.height - dc.GetTextExtent(duration).y) / 2));
        }
        if (item->kind == ChatItemKind::TurnStatus) {
            dc.SetPen(wxPen(dark_ ? wxColour(50, 51, 56) : wxColour(214, 216, 219)));
            dc.DrawLine(row.message.x, row.message.GetBottom() - FromDIP(10),
                        row.message.GetRight(), row.message.GetBottom() - FromDIP(10));
        }
        for (const auto* rect : {&row.group_header, &row.header}) {
            if (rect->IsEmpty()) continue;
            const bool group = rect == &row.group_header;
            const bool open = group ? row.group_expanded : row.expanded;
            const int x = rect->GetRight() + 1 - FromDIP(17), y = rect->y + rect->height / 2;
            const auto action = group ? Action::Group
                : item->kind == ChatItemKind::TurnStatus ? Action::Turn : Action::Tool;
            const bool hovered = row.item == hovered_ && hovered_action_ == action;
            dc.SetPen(wxPen(hovered ? Foreground() : Muted()));
            if (open) {
                dc.DrawLine(x - FromDIP(4), y - FromDIP(1), x, y + FromDIP(3));
                dc.DrawLine(x, y + FromDIP(3), x + FromDIP(4), y - FromDIP(1));
            } else {
                dc.DrawLine(x - FromDIP(1), y - FromDIP(4), x + FromDIP(3), y);
                dc.DrawLine(x + FromDIP(3), y, x - FromDIP(1), y + FromDIP(4));
            }
            if (item->kind != ChatItemKind::Tool) continue;
            const bool active = group ? row.group_active : !item->finished;
            const bool failed = group ? row.group_failed : !item->success;
            const int glyph_x = rect->x + FromDIP(18);
            dc.SetPen(wxPen(failed ? wxColour(242, 128, 128) : active ? Foreground() : wxColour(111, 190, 140)));
            if (item->tool_name == "run_command") {
                dc.SetFont(body_);
                dc.SetTextForeground(hovered ? Foreground() : Muted());
                dc.DrawText(">_", rect->x + FromDIP(9), rect->y + FromDIP(3));
            } else if (active) {
                dc.SetBrush(wxBrush(Foreground()));
                dc.DrawCircle(glyph_x, y, FromDIP(2));
            } else if (failed) {
                dc.DrawLine(glyph_x - FromDIP(4), y - FromDIP(4), glyph_x + FromDIP(4), y + FromDIP(4));
                dc.DrawLine(glyph_x + FromDIP(4), y - FromDIP(4), glyph_x - FromDIP(4), y + FromDIP(4));
            } else {
                dc.DrawLine(glyph_x - FromDIP(4), y, glyph_x - FromDIP(1), y + FromDIP(3));
                dc.DrawLine(glyph_x - FromDIP(1), y + FromDIP(3), glyph_x + FromDIP(5), y - FromDIP(4));
            }
        }
        if (row.item == hovered_) {
            dc.SetPen(wxPen(Muted()));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            if (!row.copy.IsEmpty()) {
                auto rect = row.copy;
                rect.Deflate(FromDIP(3), FromDIP(2));
                dc.DrawRectangle(rect.x + FromDIP(3), rect.y, rect.width - FromDIP(3), rect.height - FromDIP(3));
                dc.DrawRectangle(rect.x, rect.y + FromDIP(3), rect.width - FromDIP(3), rect.height - FromDIP(3));
            }
            if (!row.edit.IsEmpty()) {
                auto rect = row.edit;
                rect.Deflate(FromDIP(3), FromDIP(2));
                dc.DrawLine(rect.x, rect.GetBottom(), rect.GetRight(), rect.y);
                dc.DrawLine(rect.x, rect.GetBottom(), rect.x + FromDIP(4), rect.GetBottom() - FromDIP(1));
            }
            dc.SetFont(time_);
            dc.SetTextForeground(Muted());
            dc.DrawText(row.timestamp, row.time.GetPosition());
        }
    }
    DrawRail(dc);
}

NativeChatSurface::Hit NativeChatSurface::HitAt(const wxPoint& client_point) const {
    if (dirty_ || loading_) return {};
    if (client_point.x >= 0 && client_point.x <= FromDIP(22) && !rail_.empty()) {
        for (const auto& dash : rail_)
            if (std::abs(client_point.y - dash.y) <= FromDIP(8)) return {dash.item, Action::Rail};
        return {};
    }
    const auto point = CalcUnscrolledPosition(client_point);
    if (!earlier_loading_ && earlier_bounds_.Contains(point)) return {kNoChatItem, Action::Earlier};
    for (const auto index : rows_) {
        const auto cached = row_cache_.find(index);
        if (cached == row_cache_.end()) continue;
        const auto& row = cached->second;
        for (size_t image = 0; image < row.images.size(); ++image)
            if (row.images[image].Contains(point)) return {row.item, Action::Image, image};
        if (!row.message.Contains(point)) continue;
        if (row.copy.Contains(point)) return {row.item, Action::Copy};
        if (row.edit.Contains(point)) return {row.item, Action::Edit};
        if (row.group_header.Contains(point)) return {row.item, Action::Group};
        if (row.header.Contains(point)) {
            const auto* item = transcript_ ? transcript_->Item(row.item) : nullptr;
            return {row.item, item && item->kind == ChatItemKind::TurnStatus ? Action::Turn : Action::Tool};
        }
        return {row.item, Action::None};
    }
    return {};
}

void NativeChatSurface::Click(wxMouseEvent& event) {
    if (!transcript_ || loading_) return;
    SetFocus();
    const auto hit = HitAt(event.GetPosition());
    if (hit.action == Action::Earlier) { RequestEarlier(); return; }
    if (hit.action == Action::Rail && history_browser_) { BrowseTurn(hit.item); return; }
    if (!transcript_->Item(hit.item)) return;
    switch (hit.action) {
        case Action::None: return;
        case Action::Earlier: return;
        case Action::Image: {
            const auto* item = transcript_->Item(hit.item);
            if (item && hit.image < item->image_paths.size()) {
                const auto path = item->image_paths[hit.image];
                std::error_code error;
                if (!spark::fs::is_regular_file(NativePath(path), error)) return;
                if (open_image) open_image(path);
                else wxLaunchDefaultApplication(U(path));
            }
            return;
        }
        case Action::Copy: Copy(hit.item); return;
        case Action::Edit:
            if (edit && (!history_browser_ || transcript_ == live_transcript_) && hit.item == transcript_->LastUserIndex())
                edit(hit.item);
            return;
        case Action::Turn: transcript_->ToggleTurnStatus(hit.item); break;
        case Action::Tool: transcript_->ToggleTool(hit.item); break;
        case Action::Group: transcript_->ToggleToolGroup(hit.item); break;
        case Action::Rail:
            for (const auto& dash : rail_) {
                if (dash.item != hit.item) continue;
                if (dash.item == rail_.back().item && !transcript_->NeedsAnimation()) ScrollToLatest();
                else {
                    follow_ = false;
                    Scroll(0, std::clamp(dash.content_top - FromDIP(12), 0,
                        std::max(0, GetVirtualSize().y - GetClientSize().y)));
                    Refresh(false);
                }
                break;
            }
            return;
    }
    follow_ = false;
    Changed();
}
void NativeChatSurface::Copy(size_t index) {
    const auto* item = transcript_ ? transcript_->Item(index) : nullptr;
    if (!item || !wxTheClipboard || !wxTheClipboard->Open()) return;
    wxTheClipboard->SetData(new wxTextDataObject(U(item->kind == ChatItemKind::Tool ? item->result : item->text)));
    wxTheClipboard->Close();
}
void NativeChatSurface::Context(wxContextMenuEvent& event) {
    if (!transcript_ || loading_ || dirty_) return;
    wxPoint anchor = event.GetPosition();
    if (anchor == wxDefaultPosition) {
        const auto cached = row_cache_.find(hovered_);
        anchor = cached != row_cache_.end() ? ClientToScreen(CalcScrolledPosition(cached->second.message.GetPosition()))
                                           : wxGetMousePosition();
    }
    const wxPoint point = CalcUnscrolledPosition(ScreenToClient(anchor));
    for (const auto index : rows_) {
        const auto cached = row_cache_.find(index);
        if (cached == row_cache_.end()) continue;
        const auto& row = cached->second;
        if (!row.message.Contains(point)) continue;
        const auto* item = transcript_->Item(row.item);
        if (!item) return;
        const size_t item_index = row.item;
        const auto generation = transcript_->Generation();
        auto* const transcript = transcript_;
        const bool editable = item->kind == ChatItemKind::User
            && (!history_browser_ || transcript_ == live_transcript_)
            && item_index == transcript_->LastUserIndex() && edit;
        std::wstring destination;
        for (size_t image = 0; image < row.images.size() && image < item->image_paths.size(); ++image) {
            if (row.images[image].Contains(point)) {
                destination = U(item->image_paths[image]).ToStdWstring();
                break;
            }
        }
        if (destination.empty() && !row.fragments.empty())
            destination = ui::NativeMarkdownLinkAt(this, row.item, point.x, point.y, row.fragments.front().origin.y);
        const auto link = ResolveNativeLink(destination);
        if (!link.target.empty()) {
            constexpr int open_link = wxID_HIGHEST + 601, copy_link = wxID_HIGHEST + 602,
                          show_file = wxID_HIGHEST + 603;
            wxMenu menu;
            menu.Append(open_link, T(link.web ? L"Open link" : L"Open file"));
            menu.Append(copy_link, T(link.web ? L"Copy link address" : L"Copy file path"));
            if (!link.web) {
                menu.AppendSeparator();
                menu.Append(show_file, T(L"Show in Explorer"));
            }
            const int choice = ShowNativeMenu(this, menu, anchor);
            if (transcript_ != transcript || transcript_->Generation() != generation || !transcript_->Item(item_index)) return;
            if (choice == open_link) {
                if (link.web) wxLaunchDefaultBrowser(link.target);
                else wxLaunchDefaultApplication(link.target);
            } else if (choice == copy_link && wxTheClipboard && wxTheClipboard->Open()) {
                wxTheClipboard->SetData(new wxTextDataObject(link.target));
                wxTheClipboard->Close();
            } else if (choice == show_file && !link.web) RevealNativeFile(this, link.target);
            return;
        }
        wxMenu menu;
        menu.Append(wxID_COPY, T(item->kind == ChatItemKind::Assistant ? L"Copy reply" : L"Copy message"));
        if (editable) menu.Append(wxID_EDIT, T(L"Edit message"));
        const int choice = ShowNativeMenu(this, menu, anchor);
        if (transcript_ != transcript || transcript_->Generation() != generation || !transcript_->Item(item_index)) return;
        if (choice == wxID_COPY) Copy(item_index);
        if (choice == wxID_EDIT && edit && (!history_browser_ || transcript_ == live_transcript_)
            && item_index == transcript_->LastUserIndex()) edit(item_index);
        break;
    }
}
}  // namespace spark
