#include "gui/native/chat_surface.h"
#include "gui/native/chat_usage.h"
#include "gui/native/popup_menu.h"
#include "gui/localization.h"
#include "platform/clock.h"
#include "nlohmann/json.hpp"

#include <wx/clipbrd.h>
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
enum class DurationKind { Tool, Turn };
wxString Duration(uint64_t ms, DurationKind kind = DurationKind::Tool) {
    const auto seconds = std::to_wstring(ms / 1000);
    const auto minutes = std::to_wstring(ms / 60000);
    if (kind == DurationKind::Turn) {
        if (ms < 60000) return wxString(i18n::Format(L"{seconds} sec", {{L"seconds", seconds}}));
        if (ms < 3600000) return wxString(i18n::Format(L"{minutes} min {seconds} sec",
            {{L"minutes", minutes}, {L"seconds", std::to_wstring(ms / 1000 % 60)}}));
        return wxString(i18n::Format(L"{hours} hr {minutes} min",
            {{L"hours", std::to_wstring(ms / 3600000)}, {L"minutes", std::to_wstring(ms / 60000 % 60)}}));
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
        if (rotation) ScrollBy((rotation > 0 ? -1 : 1) * FromDIP(54)
            * std::max(1, std::abs(rotation) / std::max(1, event.GetWheelDelta())));
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
        UnsetToolTip();
        SetCursor(wxCURSOR_ARROW);
        Refresh(false);
    });
}
wxColour NativeChatSurface::Foreground() const { return dark_ ? wxColour(236, 237, 240) : wxColour(31, 32, 35); }
wxColour NativeChatSurface::Muted() const { return dark_ ? wxColour(150, 153, 160) : wxColour(103, 106, 112); }
void NativeChatSurface::SetDark(bool dark) { dark_ = dark; row_cache_.clear(); dirty_ = true; Refresh(false); }
void NativeChatSurface::SetEmptyTitle(const wxString& title) { empty_title_ = title; Refresh(false); }
void NativeChatSurface::SetLoading(uint64_t started) { loading_ = started; Refresh(false); }
void NativeChatSurface::SetTranscript(Transcript* transcript, bool reset_scroll) {
    if (transcript != transcript_) { transcript_ = transcript; markdown_.clear(); row_cache_.clear(); }
    hovered_ = kNoChatItem;
    hovered_action_ = Action::None;
    UnsetToolTip();
    if (reset_scroll) { follow_ = true; Scroll(0, 0); }
    dirty_ = true;
    Refresh(false);
}
void NativeChatSurface::Changed() {
    if (transcript_ && generation_ != transcript_->Generation()) {
        generation_ = transcript_->Generation();
        markdown_.clear();
        row_cache_.clear();
    }
    dirty_ = true;
    Refresh(false);
}
void NativeChatSurface::ScrollToLatest() {
    follow_ = true;
    dirty_ = true;
    Scroll(0, std::max(0, GetVirtualSize().y - GetClientSize().y));
    Refresh(false);
}
void NativeChatSurface::ScrollBy(int pixels) {
    const int bottom = std::max(0, GetVirtualSize().y - GetClientSize().y);
    const int next = std::clamp(GetViewStart().y + pixels, 0, bottom);
    follow_ = pixels >= 0 && next >= bottom - FromDIP(2);
    hovered_ = kNoChatItem;
    hovered_action_ = Action::None;
    UnsetToolTip();
    SetCursor(wxCURSOR_ARROW);
    Scroll(0, next);
    Refresh(false);
}
void NativeChatSurface::Tick() {
    const auto now = MonotonicMillis();
    if (transcript_ && transcript_->NeedsAnimation() && now / 100 != elapsed_frame_) {
        elapsed_frame_ = now / 100;
        dirty_ = true;
    }
    if ((loading_ && now - loading_ >= 150) || (transcript_ && transcript_->NeedsAnimation()))
        Refresh(false);
}

void NativeChatSurface::Reflow(wxDC& dc) {
    if (transcript_ && generation_ != transcript_->Generation()) {
        generation_ = transcript_->Generation();
        row_cache_.clear();
        markdown_.clear();
    }
    rows_.clear();
    rail_.clear();
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
        const auto groups = transcript_->Groups(MonotonicMillis());
        const auto last_user = transcript_->LastUserIndex();
        for (size_t index = 0; index < items.size(); ++index) {
            const auto& item = items[index];
            if (!item.visible || groups[index].hidden) continue;
            const auto cached = row_cache_.find(index);
            const bool message = item.kind == ChatItemKind::Assistant || item.kind == ChatItemKind::User;
            const bool editable = item.kind == ChatItemKind::User && index == last_user && edit;
            if (message && cached != row_cache_.end() && cached->second.revision == item.revision
                && (!cached->second.edit.IsEmpty()) == editable) {
                auto& row = cached->second;
                const int offset = top - row.bounds.y;
                if (offset != 0) {
                    for (auto* rect : {&row.bounds, &row.message, &row.bubble, &row.header,
                                       &row.group_header, &row.copy, &row.edit, &row.time})
                        if (!rect->IsEmpty()) rect->Offset(0, offset);
                    for (auto& part : row.fragments) part.origin.y += offset;
                    for (auto& panel : row.panels) panel.bounds.Offset(0, offset);
                }
                top += row.bounds.height;
                rows_.push_back(index);
                continue;
            }
            Row row;
            row.item = index;
            row.revision = item.revision;
            const int before = top;
            const bool status = item.kind == ChatItemKind::TurnStatus;
            const bool tool = item.kind == ChatItemKind::Tool;
            row.user = item.kind == ChatItemKind::User;
            if (status || tool || item.kind == ChatItemKind::Reasoning) {
                dc.SetFont(body_);
                row.expanded = item.expanded;
                if (status) {
                    const auto elapsed = Transcript::Elapsed(item, MonotonicMillis());
                    auto label = T(item.paused ? L"Interrupted" : item.finished ? L"Processed" : L"Processing");
                    if (elapsed) label = wxString(i18n::Format(item.paused ? L"Interrupted {elapsed}"
                        : item.finished ? L"Processed {elapsed}" : L"Processing {elapsed}",
                        {{L"elapsed", Duration(elapsed, DurationKind::Turn).ToStdWstring()}}));
                    row.fragments.push_back({wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
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
                        row.group_header = wxRect(left, top, width, FromDIP(28));
                        auto label = GroupLabel(group, item.tool_name);
                        if (group.active) {
                            for (size_t member = index; member <= group.last_member && member < items.size(); ++member) {
                                if (items[member].kind == ChatItemKind::Tool && !items[member].finished) {
                                    label += "  \u00b7  " + U(ToolText(items[member], ToolTextKind::Summary));
                                    break;
                                }
                            }
                        }
                        if (group.elapsed_ms) label += "  \u00b7  " + Duration(group.elapsed_ms);
                        row.fragments.push_back({wxControl::Ellipsize(label, dc, wxELLIPSIZE_END,
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
                        row.fragments.push_back({duration, {row.header.GetRight() + 1 - FromDIP(30)
                            - dc.GetTextExtent(duration).x,
                            top + std::max(0, (FromDIP(28) - dc.GetTextExtent(duration).y) / 2)},
                            time_, dark_ ? wxColour(112, 115, 122) : wxColour(135, 138, 144)});
                        top += FromDIP(28);
                        if (item.expanded) {
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
            } else if (item.kind == ChatItemKind::Compaction) {
                dc.SetFont(body_);
                const int height = std::max(FromDIP(22), dc.GetTextExtent("Ag").y);
                const auto label = wxControl::Ellipsize(T(item.success ? L"Context automatically compacted"
                    : L"Context manually compacted"), dc, wxELLIPSIZE_END, std::max(1, width - FromDIP(22)));
                row.fragments.push_back({label, {left + FromDIP(22),
                    top + std::max(0, (height - dc.GetTextExtent(label).y) / 2)}, body_, Muted(), false, true});
                row.message = wxRect(left, top, width, height);
                top += height + FromDIP(6);
            } else {
                int message_width = width;
                if (row.user) {
                    dc.SetFont(body_);
                    int measured = 0;
                    for (const auto& part : wxSplit(U(item.text), '\n'))
                        measured = std::max(measured, dc.GetTextExtent(part).x);
                    const int maximum = std::min(width, std::max(FromDIP(180), width * 78 / 100));
                    message_width = std::min(maximum, std::max(FromDIP(120), measured + FromDIP(30)));
                    top += FromDIP(11);
                }
                const int message_left = row.user ? left + width - message_width : left;
                if (row.user) {
                    LayoutText(dc, row, PlainLines(item.text, markdown::MarkdownStyle::Body),
                               message_left + FromDIP(14), std::max(1, message_width - FromDIP(28)), top, false);
                    top = std::max(top, before + FromDIP(31)) + FromDIP(11);
                    row.bubble = wxRect(message_left, before, message_width, top - before);
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
                row.message = wxRect(message_left, before, message_width, top - before);
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
    if (follow_) Scroll(0, std::max(0, GetVirtualSize().y - GetClientSize().y));
}

void NativeChatSurface::UpdateRail(int content_height) {
    const int viewport = GetClientSize().y;
    if (!transcript_ || content_height <= viewport || viewport <= FromDIP(20)) return;
    for (const auto index : rows_) {
        const auto* item = transcript_->Item(index);
        const auto cached = row_cache_.find(index);
        if (item && item->kind == ChatItemKind::User && cached != row_cache_.end())
            rail_.push_back({index, cached->second.bounds.y, 0});
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

void NativeChatSurface::DrawRail(wxDC& dc) {
    if (rail_.empty()) return;
    const auto saved_origin = dc.GetDeviceOrigin();
    dc.SetDeviceOrigin(0, 0);
    size_t current = rail_.front().item;
    for (const auto& dash : rail_)
        if (dash.content_top <= GetViewStart().y + FromDIP(24)) current = dash.item;
    if (follow_ && transcript_ && transcript_->NeedsAnimation()) current = transcript_->LastUserIndex();
    int hovered_y = -1;
    dc.SetPen(*wxTRANSPARENT_PEN);
    for (const auto& dash : rail_) {
        const bool hovered = hovered_action_ == Action::Rail && hovered_ == dash.item;
        const bool active = !hovered && dash.item == current;
        const int length = FromDIP(hovered ? 18 : active ? 13 : 8);
        const int thickness = std::max(1, FromDIP(2));
        dc.SetBrush(wxBrush(hovered ? (dark_ ? wxColour(244, 245, 248) : wxColour(72, 74, 79))
            : active ? (dark_ ? wxColour(150, 153, 160) : wxColour(112, 115, 120))
                     : (dark_ ? wxColour(74, 76, 82) : wxColour(180, 183, 188))));
        dc.DrawRectangle(FromDIP(6), dash.y - thickness / 2, length, thickness);
        if (hovered) hovered_y = dash.y;
    }
    const auto* item = transcript_ ? transcript_->Item(hovered_) : nullptr;
    if (hovered_y >= 0 && item) {
        auto preview = U(item->text);
        preview.Trim().Trim(false);
        const auto newline = preview.find_first_of("\r\n");
        if (newline != wxString::npos) preview = preview.Left(newline) + "\u2026";
        if (preview.length() > 72) preview = preview.Left(72) + "\u2026";
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
        const int box_width = std::min(max_width, width) + padding_x * 2;
        const int box_height = std::max(FromDIP(30), height + padding_y * 2);
        const int left = std::max(FromDIP(12), std::min(FromDIP(36),
            GetClientSize().x - box_width - FromDIP(12)));
        const int top = std::max(FromDIP(8), std::min(hovered_y - box_height / 2,
            GetClientSize().y - box_height - FromDIP(8)));
        dc.SetPen(wxPen(dark_ ? wxColour(78, 79, 85) : wxColour(196, 198, 201)));
        dc.SetBrush(wxBrush(dark_ ? wxColour(46, 47, 51) : wxColour(255, 255, 255)));
        dc.DrawRoundedRectangle(left, top, box_width, box_height, FromDIP(10));
        wxDCClipper clip(dc, left + padding_x, top + padding_y, max_width, height);
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
    if (rows_.empty()) {
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
    for (const auto index : rows_) {
        const auto cached = row_cache_.find(index);
        if (cached == row_cache_.end()) continue;
        const auto& row = cached->second;
        if (!row.bounds.Intersects(viewport)) continue;
        const auto* item = transcript_ ? transcript_->Item(row.item) : nullptr;
        if (!item) continue;
        if (row.user) {
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
            dc.SetPen(wxPen(row.item == hovered_ ? Foreground() : Muted()));
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
                dc.SetTextForeground(row.item == hovered_ ? Foreground() : Muted());
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
    for (const auto index : rows_) {
        const auto cached = row_cache_.find(index);
        if (cached == row_cache_.end()) continue;
        const auto& row = cached->second;
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
    if (!transcript_->Item(hit.item)) return;
    switch (hit.action) {
        case Action::None: return;
        case Action::Copy: Copy(hit.item); return;
        case Action::Edit:
            if (edit && hit.item == transcript_->LastUserIndex()) edit(hit.item);
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
            && item_index == transcript_->LastUserIndex() && edit;
        wxMenu menu;
        menu.Append(wxID_COPY, T(item->kind == ChatItemKind::Assistant ? L"Copy reply" : L"Copy message"));
        if (editable) menu.Append(wxID_EDIT, T(L"Edit message"));
        const int choice = ShowNativeMenu(this, menu, anchor);
        if (transcript_ != transcript || transcript_->Generation() != generation || !transcript_->Item(item_index)) return;
        if (choice == wxID_COPY) Copy(item_index);
        if (choice == wxID_EDIT && edit && item_index == transcript_->LastUserIndex()) edit(item_index);
        break;
    }
}
}  // namespace spark
