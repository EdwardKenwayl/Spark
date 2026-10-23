#include "gui/provider_editor.h"
#include "gui/localization.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include <windowsx.h>
#include <algorithm>
#include <array>

namespace spark {
namespace {
const wchar_t* Text(std::wstring_view key) { return i18n::Text(key); }
constexpr std::array<const char*, 8> kReasoningEfforts{
    "none", "minimal", "low", "medium", "high", "xhigh", "max", "ultra"};

void SetCaption(HWND control, const std::wstring& text) {
    if (ReadWindowText(control) != text) SetWindowTextW(control, text.c_str());
}

std::wstring ReasoningSummary(const ModelConfig& model) {
    const auto efforts = EffectiveReasoningEfforts(model);
    if (efforts.empty()) return model.capabilities.reasoning ? Text(L"Auto") : Text(L"Off");
    std::vector<int> positions;
    positions.reserve(efforts.size());
    for (const std::string& effort : efforts) {
        const auto found = std::find_if(kReasoningEfforts.begin(), kReasoningEfforts.end(),
            [&](const char* value) { return effort == value; });
        positions.push_back(found == kReasoningEfforts.end() ? -1 : static_cast<int>(found - kReasoningEfforts.begin()));
    }
    const bool ordered = std::all_of(positions.begin(), positions.end(),
        [](int position) { return position >= 0; }) &&
        std::is_sorted(positions.begin(), positions.end()) &&
        std::adjacent_find(positions.begin(), positions.end()) == positions.end();
    if (!ordered) {
        std::wstring result;
        for (const std::string& effort : efforts) {
            if (!result.empty()) result += L", ";
            result += Utf8ToWide(effort);
        }
        return result;
    }
    std::wstring result;
    size_t first = 0;
    for (size_t end = 1; end <= efforts.size(); ++end) {
        if (end < efforts.size() && positions[end] == positions[end - 1] + 1) continue;
        if (!result.empty()) result += L", ";
        result += Utf8ToWide(efforts[first]);
        if (end - first >= 3) result += L" \u2192 " + Utf8ToWide(efforts[end - 1]);
        else for (size_t index = first + 1; index < end; ++index) result += L", " + Utf8ToWide(efforts[index]);
        first = end;
    }
    return result;
}
}  // namespace

int ProviderEditor::Scale(int value) const { return MulDiv(value, dpi_, 96); }

HWND ProviderEditor::Control(HWND parent, const wchar_t* type, const wchar_t* text, int id, DWORD style) {
    HWND control = CreateWindowW(type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    SetWindowSubclass(control, ControlProc, 1, reinterpret_cast<DWORD_PTR>(this));
    return control;
}

HWND ProviderEditor::Field(const wchar_t* text, int id, bool password) {
    HWND field = Control(body_, L"EDIT", text, id, WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0));
    SendMessageW(field, EM_SETLIMITTEXT, id == Key ? 16384 : 2048, 0);
    return field;
}

void ProviderEditor::Tip(HWND control, const wchar_t* text) {
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS; tool.hwnd = GetParent(control);
    tool.uId = reinterpret_cast<UINT_PTR>(control); tool.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
}

void ProviderEditor::Open(HWND parent, const std::string& preference, HFONT font, HFONT heading,
                          ProviderConfig provider, const ContextConfig& context,
                          std::function<Result<void>(ProviderConfig)> save,
                          std::function<void()> back) {
    Close();
    syncing_ = true; json_pending_ = false; json_valid_ = true; status_ok_ = false;
    draft_ = std::move(provider);
    context_ = context;
    on_save_ = std::move(save); on_back_ = std::move(back);
    palette_ = theme::For(preference); font_ = font; heading_ = heading;
    HDC dc = GetDC(parent); dpi_ = GetDeviceCaps(dc, LOGPIXELSY); ReleaseDC(parent, dc);
    background_ = CreateSolidBrush(palette_.background); input_ = CreateSolidBrush(palette_.input);
    WNDCLASSW cls{};
    cls.lpfnWndProc = WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW); cls.lpszClassName = L"SparkProviderEditor";
    RegisterClassW(&cls);
    window_ = CreateWindowW(cls.lpszClassName, L"", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 0, 0,
                           parent, nullptr, cls.hInstance, this);
    SetWindowLongPtrW(window_, GWL_EXSTYLE, WS_EX_CONTROLPARENT);
    tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                             0, 0, 0, 0, window_, nullptr, cls.hInstance, nullptr);
    back_ = Control(window_, L"BUTTON", L"", Back, WS_TABSTOP | BS_OWNERDRAW);
    title_ = Control(window_, L"STATIC", Text(draft_.label.empty() ? L"Add provider" : L"Edit provider"), 0, SS_ENDELLIPSIS);
    SendMessageW(title_, WM_SETFONT, reinterpret_cast<WPARAM>(heading_), FALSE);
    save_ = Control(window_, L"BUTTON", Text(L"Save provider"), SaveButton, WS_TABSTOP | BS_OWNERDRAW);
    status_ = Control(window_, L"STATIC", L"", Status, SS_LEFT);
    body_ = CreateWindowW(cls.lpszClassName, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, 0, 0,
                         window_, nullptr, cls.hInstance, this);
    SetWindowLongPtrW(body_, GWL_EXSTYLE, WS_EX_CONTROLPARENT);
    for (const wchar_t* label : {Text(L"Provider name"), L"Base URL", L"API Key", Text(L"Upstream protocol"),
                                 Text(L"Model list"), Text(L"Model ID"), Text(L"Context window"),
                                 Text(L"Auto compact (%)"), Text(L"Image handling"), Text(L"Reasoning levels"),
                                 Text(L"Test model")})
        labels_.push_back(Control(body_, L"STATIC", label, 0, SS_ENDELLIPSIS));
    name_ = Field(Utf8ToWide(draft_.label).c_str(), Name);
    url_ = Field(Utf8ToWide(draft_.base_url).c_str(), Url);
    key_ = Field(Utf8ToWide(draft_.api_key_expression.empty() ? draft_.api_key : draft_.api_key_expression).c_str(), Key, true);
    reveal_ = Control(body_, L"BUTTON", L"", Reveal, WS_TABSTOP | BS_OWNERDRAW);
    Tip(back_, Text(L"Provider configuration")); Tip(reveal_, Text(L"Show or hide API key"));
    responses_ = Control(body_, L"BUTTON", L"Responses API", Responses, WS_TABSTOP | BS_OWNERDRAW);
    chat_ = Control(body_, L"BUTTON", L"Chat Completions", Chat, WS_TABSTOP | BS_OWNERDRAW);
    anthropic_ = Control(body_, L"BUTTON", L"Anthropic Messages", Anthropic, WS_TABSTOP | BS_OWNERDRAW);
    add_model_ = Control(body_, L"BUTTON", Text(L"Add model"), AddModelButton, WS_TABSTOP | BS_OWNERDRAW);
    fetch_models_ = Control(body_, L"BUTTON", Text(L"Fetch from upstream"), FetchModels, WS_TABSTOP | BS_OWNERDRAW);
    clear_models_ = Control(body_, L"BUTTON", Text(L"Clear models"), ClearModels, WS_TABSTOP | BS_OWNERDRAW);
    test_model_ = Field(Utf8ToWide(draft_.test_model).c_str(), TestModel);
    preview_label_ = Control(body_, L"STATIC", Text(L"config.json preview"), 0, SS_ENDELLIPSIS);
    load_config_ = Control(body_, L"BUTTON", Text(L"Load configuration"), LoadConfig, WS_TABSTOP | BS_OWNERDRAW);
    preview_ = Control(body_, L"EDIT", L"", Preview, WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN);
    code_font_ = CreateFontW(-Scale(13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH, L"Consolas");
    if (code_font_) SendMessageW(preview_, WM_SETFONT, reinterpret_cast<WPARAM>(code_font_), FALSE);
    SendMessageW(preview_, EM_SETLIMITTEXT, 1024 * 1024, 0);
    SendMessageW(test_model_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Text(L"Test model")));
    scroll_ = 0; revealed_ = false;
    const auto models = draft_.models;
    for (const auto& model : models) AddModel(model);
    if (rows_.empty()) AddModel({});
    syncing_ = false; RefreshPreview();
    Layout();
    ShowWindow(window_, SW_SHOW);
}

void ProviderEditor::Close() {
    CancelModelFetch();
    syncing_ = true;
    CloseReasoningPopup();
    if (window_) DestroyWindow(window_);
    window_ = body_ = preview_ = reasoning_popup_ = nullptr; rows_.clear(); labels_.clear();
    if (background_) DeleteObject(background_);
    if (input_) DeleteObject(input_);
    if (code_font_) DeleteObject(code_font_);
    code_font_ = nullptr;
    background_ = input_ = nullptr;
}

void ProviderEditor::Resize(int x, int y, int width, int height) {
    if (!window_) return;
    RECT previous{}; GetClientRect(window_, &previous);
    SetWindowPos(window_, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    if (previous.right == 0 && GetForegroundWindow() == GetAncestor(window_, GA_ROOT)) SetFocus(name_);
}

void ProviderEditor::AddModel(ModelConfig model) {
    CloseReasoningPopup();
    const bool was_syncing = syncing_;
    syncing_ = true;
    ModelRow row;
    row.model = std::move(model);
    row.auto_compact_percent = row.model.capabilities.auto_compact_threshold_percent;
    row.auto_compact_options = {50, 60, 70, 75, 80, 85, 90, 95, 100};
    if (row.auto_compact_percent > 0 &&
        std::find(row.auto_compact_options.begin(), row.auto_compact_options.end(),
                  row.auto_compact_percent) == row.auto_compact_options.end()) {
        row.auto_compact_options.push_back(row.auto_compact_percent);
        std::sort(row.auto_compact_options.begin(), row.auto_compact_options.end());
    }
    row.id = Field(Utf8ToWide(row.model.id).c_str(), 0);
    row.context = Field(row.model.capabilities.context_window > 0
        ? std::to_wstring(row.model.capabilities.context_window).c_str() : L"", 0);
    row.auto_compact = Control(body_, L"BUTTON", L"", 0, WS_TABSTOP | BS_OWNERDRAW);
    row.images = Control(body_, L"BUTTON", Text(row.model.capabilities.images ? L"Send original images" : L"Do not send images"),
                         0, WS_TABSTOP | BS_OWNERDRAW);
    row.reasoning = Control(body_, L"BUTTON", L"", 0, WS_TABSTOP | BS_OWNERDRAW);
    row.remove = Control(body_, L"BUTTON", L"", 0, WS_TABSTOP | BS_OWNERDRAW);
    Tip(row.remove, Text(L"Remove model")); Tip(row.context, Text(L"Context window"));
    Tip(row.auto_compact, Text(L"Auto compact (%)")); Tip(row.reasoning, Text(L"Configure reasoning levels and default"));
    Tip(row.id, Text(L"Model ID"));
    SetAutoCompactButton(row);
    SetReasoningButton(row, true);
    rows_.push_back(std::move(row));
    syncing_ = was_syncing;
}

void ProviderEditor::SetAutoCompactButton(ModelRow& row) {
    const std::wstring label = row.auto_compact_percent > 0
        ? std::to_wstring(row.auto_compact_percent) + L"%"
        : i18n::Format(L"Default ({value}%)", {{L"value", std::to_wstring(context_.auto_compact_threshold_percent)}});
    SetCaption(row.auto_compact, label);
}

void ProviderEditor::ShowAutoCompactMenu(ModelRow& row) {
    CloseReasoningPopup();
    KillTimer(window_, 1);
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    constexpr UINT kDefaultCommand = 1;
    constexpr UINT kPercentCommandBase = 100;
    const std::wstring default_label = i18n::Format(L"Default ({value}%)",
        {{L"value", std::to_wstring(context_.auto_compact_threshold_percent)}});
    AppendMenuW(menu, MF_STRING | (row.auto_compact_percent == 0 ? MF_CHECKED : 0),
                kDefaultCommand, default_label.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (size_t index = 0; index < row.auto_compact_options.size(); ++index) {
        const int percent = row.auto_compact_options[index];
        const std::wstring label = std::to_wstring(percent) + L"%";
        AppendMenuW(menu, MF_STRING | (row.auto_compact_percent == percent ? MF_CHECKED : 0),
                    kPercentCommandBase + static_cast<UINT>(index), label.c_str());
    }
    for (int index = 0; index < GetMenuItemCount(menu); ++index) {
        MENUITEMINFOW item{sizeof(item)};
        item.fMask = MIIM_FTYPE;
        GetMenuItemInfoW(menu, index, TRUE, &item);
        if (item.fType & MFT_SEPARATOR) continue;
        item.fType |= MFT_OWNERDRAW;
        SetMenuItemInfoW(menu, index, TRUE, &item);
    }
    HBRUSH menu_background = CreateSolidBrush(palette_.panel);
    MENUINFO menu_info{};
    menu_info.cbSize = sizeof(menu_info);
    menu_info.fMask = MIM_BACKGROUND;
    menu_info.hbrBack = menu_background;
    SetMenuInfo(menu, &menu_info);
    RECT rect{};
    GetWindowRect(row.auto_compact, &rect);
    const int command = static_cast<int>(TrackPopupMenu(
        menu, TPM_RETURNCMD | TPM_LEFTALIGN | TPM_TOPALIGN, rect.left, rect.bottom, 0, window_, nullptr));
    DestroyMenu(menu);
    DeleteObject(menu_background);
    if (command == static_cast<int>(kDefaultCommand)) {
        row.auto_compact_percent = 0;
    } else if (command >= static_cast<int>(kPercentCommandBase) &&
               command < static_cast<int>(kPercentCommandBase + row.auto_compact_options.size())) {
        row.auto_compact_percent = row.auto_compact_options[static_cast<size_t>(command - kPercentCommandBase)];
    } else {
        return;
    }
    SetAutoCompactButton(row);
    RefreshPreview();
    Layout();
}

void ProviderEditor::SetImages(ModelRow& row, bool enabled) {
    row.model.capabilities.images = enabled;
    SetWindowTextW(row.images, Text(enabled ? L"Send original images" : L"Do not send images"));
}

void ProviderEditor::SetReasoningButton(ModelRow& row, bool include_label) {
    ModelConfig model = row.model;
    model.id = Trim(WideToUtf8(ReadWindowText(row.id)));
    std::wstring label = ReasoningSummary(model);
    if (include_label) label = std::wstring(Text(L"Reasoning")) + L": " + label;
    SetCaption(row.reasoning, label);
}

void ProviderEditor::ShowReasoningMenu(ModelRow& row) {
    if (reasoning_popup_) {
        CloseReasoningPopup();
        return;
    }
    reasoning_popup_row_ = static_cast<int>(&row - rows_.data());
    ModelConfig model = row.model;
    model.id = Trim(WideToUtf8(ReadWindowText(row.id)));
    const std::vector<std::string> configured = model.capabilities.reasoning
        ? EffectiveReasoningEfforts(model) : std::vector<std::string>{};
    ModelConfig built_in_model = model;
    built_in_model.capabilities.reasoning = true;
    built_in_model.capabilities.reasoning_efforts.clear();
    reasoning_popup_efforts_ = EffectiveReasoningEfforts(built_in_model);
    if (reasoning_popup_efforts_.empty()) {
        for (const char* effort : kReasoningEfforts)
            if (std::find(configured.begin(), configured.end(), effort) != configured.end())
                reasoning_popup_efforts_.push_back(effort);
    }
    reasoning_popup_hover_effort_ = -1;
    reasoning_popup_hover_chip_ = -1;
    reasoning_popup_hover_reset_ = false;
    reasoning_popup_hover_done_ = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    reasoning_popup_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"SparkProviderEditor", L"",
        WS_POPUP, 0, 0, 0, 0, window_, nullptr, instance, this);
    if (!reasoning_popup_) {
        reasoning_popup_row_ = -1;
        return;
    }
    LayoutReasoningPopup();
    ShowWindow(reasoning_popup_, SW_SHOW);
    SetFocus(reasoning_popup_);
    SetCapture(reasoning_popup_);
}

void ProviderEditor::CloseReasoningPopup() {
    HWND popup = reasoning_popup_;
    HWND button = reasoning_popup_row_ >= 0 && reasoning_popup_row_ < static_cast<int>(rows_.size())
        ? rows_[static_cast<size_t>(reasoning_popup_row_)].reasoning : nullptr;
    const bool focused = popup && GetFocus() == popup;
    reasoning_popup_ = nullptr;
    reasoning_popup_row_ = -1;
    if (popup) {
        if (GetCapture() == popup) ReleaseCapture();
        DestroyWindow(popup);
        if (focused && IsWindow(button) && GetForegroundWindow() == GetAncestor(button, GA_ROOT)) SetFocus(button);
    }
}

void ProviderEditor::LayoutReasoningPopup() {
    if (!reasoning_popup_ || reasoning_popup_row_ < 0 ||
        reasoning_popup_row_ >= static_cast<int>(rows_.size())) return;
    const int width = Scale(296), padding = Scale(12), row_height = Scale(32);
    reasoning_popup_done_rect_ = {width - Scale(36), Scale(6), width - Scale(6), Scale(36)};
    reasoning_popup_effort_rects_.clear();
    for (size_t index = 0; index < reasoning_popup_efforts_.size(); ++index) {
        const int y = Scale(42) + static_cast<int>(index) * row_height;
        reasoning_popup_effort_rects_.push_back({Scale(8), y, width - Scale(8), y + row_height});
    }
    int x = padding, y = Scale(42) + static_cast<int>(reasoning_popup_efforts_.size()) * row_height + Scale(44);
    reasoning_popup_chip_rects_.clear();
    HDC dc = GetDC(window_); HGDIOBJ old = SelectObject(dc, font_);
    for (size_t index = 0; index <= reasoning_popup_efforts_.size(); ++index) {
        const std::wstring label = index == 0 ? Text(L"Auto") : Utf8ToWide(reasoning_popup_efforts_[index - 1]);
        SIZE text{}; GetTextExtentPoint32W(dc, label.c_str(), static_cast<int>(label.size()), &text);
        const int chip_width = std::min(width - padding * 2, std::max(Scale(42), static_cast<int>(text.cx) + Scale(20)));
        if (x > padding && x + chip_width > width - padding) { x = padding; y += Scale(38); }
        reasoning_popup_chip_rects_.push_back({x, y, x + chip_width, y + row_height});
        x += chip_width + Scale(6);
    }
    SelectObject(dc, old); ReleaseDC(window_, dc);
    const int reset_y = y + row_height + Scale(8), height = reset_y + Scale(36);
    reasoning_popup_reset_rect_ = {padding, reset_y, width - padding, reset_y + Scale(28)};
    RECT anchor{}; GetWindowRect(rows_[static_cast<size_t>(reasoning_popup_row_)].reasoning, &anchor);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT work = monitor.rcWork;
    const int left = std::clamp(anchor.left, work.left, std::max(work.left, work.right - width));
    const int top = anchor.bottom + height <= work.bottom ? anchor.bottom : std::max(work.top, anchor.top - height);
    SetWindowPos(reasoning_popup_, HWND_TOPMOST, left, top, width, height, SWP_NOACTIVATE);
}

void ProviderEditor::HandleReasoningPopupClick(POINT point) {
    if (reasoning_popup_row_ < 0 || reasoning_popup_row_ >= static_cast<int>(rows_.size())) return;
    RECT bounds{}; GetClientRect(reasoning_popup_, &bounds);
    if (!PtInRect(&bounds, point) || PtInRect(&reasoning_popup_done_rect_, point)) {
        CloseReasoningPopup(); return;
    }
    ModelRow& row = rows_[static_cast<size_t>(reasoning_popup_row_)];
    ModelCapabilities& capabilities = row.model.capabilities;
    ModelConfig current_model = row.model;
    current_model.id = Trim(WideToUtf8(ReadWindowText(row.id)));
    const std::vector<std::string> selected = capabilities.reasoning
        ? EffectiveReasoningEfforts(current_model) : std::vector<std::string>{};
    if (PtInRect(&reasoning_popup_reset_rect_, point)) {
        capabilities.reasoning = true;
        capabilities.reasoning_efforts.clear();
        capabilities.default_reasoning_effort.clear();
    } else {
        bool changed = false;
        for (size_t index = 0; index < reasoning_popup_effort_rects_.size(); ++index) {
            if (!PtInRect(&reasoning_popup_effort_rects_[index], point)) continue;
            std::vector<std::string> next;
            for (size_t candidate = 0; candidate < reasoning_popup_efforts_.size(); ++candidate) {
                const auto& effort = reasoning_popup_efforts_[candidate];
                const bool checked = std::find(selected.begin(), selected.end(), effort) != selected.end();
                if (checked != (index == candidate)) next.push_back(effort);
            }
            capabilities.reasoning = !next.empty();
            capabilities.reasoning_efforts = std::move(next);
            if (std::find(capabilities.reasoning_efforts.begin(), capabilities.reasoning_efforts.end(),
                          capabilities.default_reasoning_effort) == capabilities.reasoning_efforts.end())
                capabilities.default_reasoning_effort.clear();
            changed = true; break;
        }
        for (size_t index = 0; !changed && index < reasoning_popup_chip_rects_.size(); ++index) {
            if (!PtInRect(&reasoning_popup_chip_rects_[index], point)) continue;
            if (index == 0) capabilities.default_reasoning_effort.clear();
            else {
                const auto& effort = reasoning_popup_efforts_[index - 1];
                if (std::find(selected.begin(), selected.end(), effort) == selected.end()) return;
                capabilities.default_reasoning_effort = effort;
            }
            changed = true;
        }
        if (!changed) return;
    }
    SetReasoningButton(row);
    RefreshPreview();
    UpdateReasoningPopup();
}

void ProviderEditor::UpdateReasoningPopup() {
    if (!reasoning_popup_ || reasoning_popup_row_ < 0 ||
        reasoning_popup_row_ >= static_cast<int>(rows_.size())) return;
    InvalidateRect(reasoning_popup_, nullptr, FALSE);
}

void ProviderEditor::DrawReasoningPopup(HDC dc) {
    if (reasoning_popup_row_ < 0 || reasoning_popup_row_ >= static_cast<int>(rows_.size())) return;
    const ModelRow& row = rows_[static_cast<size_t>(reasoning_popup_row_)];
    const ModelCapabilities& capabilities = row.model.capabilities;
    ModelConfig model = row.model;
    model.id = Trim(WideToUtf8(ReadWindowText(row.id)));
    const auto selected = capabilities.reasoning ? EffectiveReasoningEfforts(model) : std::vector<std::string>{};
    RECT bounds{};
    GetClientRect(reasoning_popup_, &bounds);
    HBRUSH panel = CreateSolidBrush(palette_.panel);
    FillRect(dc, &bounds, panel);
    DeleteObject(panel);
    HPEN border = CreatePen(PS_SOLID, 1, palette_.border);
    HGDIOBJ old_pen = SelectObject(dc, border);
    HGDIOBJ old_brush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    RoundRect(dc, 0, 0, bounds.right, bounds.bottom, Scale(8), Scale(8));
    SelectObject(dc, old_brush);
    SelectObject(dc, old_pen);
    DeleteObject(border);
    SetBkMode(dc, TRANSPARENT);
    HFONT old_font = static_cast<HFONT>(SelectObject(dc, font_));

    RECT heading{Scale(12), Scale(8), reasoning_popup_done_rect_.left, Scale(34)};
    SetTextColor(dc, palette_.muted);
    DrawTextW(dc, Text(L"Reasoning levels"), -1, &heading,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (size_t index = 0; index < reasoning_popup_effort_rects_.size(); ++index) {
        RECT item = reasoning_popup_effort_rects_[index];
        if (static_cast<int>(index) == reasoning_popup_hover_effort_) {
            HBRUSH hover = CreateSolidBrush(palette_.selection);
            FillRect(dc, &item, hover);
            DeleteObject(hover);
        }
        const std::string& effort = reasoning_popup_efforts_[index];
        const bool checked = std::find(selected.begin(), selected.end(), effort) != selected.end();
        const int center_x = item.left + Scale(18);
        const int center_y = (item.top + item.bottom) / 2;
        HPEN mark = CreatePen(PS_SOLID, Scale(1), checked ? palette_.text : palette_.muted);
        HGDIOBJ previous_pen = SelectObject(dc, mark);
        HBRUSH fill = CreateSolidBrush(checked ? palette_.selection : palette_.panel);
        HGDIOBJ previous_brush = SelectObject(dc, fill);
        Ellipse(dc, center_x - Scale(7), center_y - Scale(7), center_x + Scale(7), center_y + Scale(7));
        if (checked) {
            MoveToEx(dc, center_x - Scale(3), center_y, nullptr);
            LineTo(dc, center_x - Scale(1), center_y + Scale(3));
            LineTo(dc, center_x + Scale(4), center_y - Scale(3));
        }
        SelectObject(dc, previous_brush);
        SelectObject(dc, previous_pen);
        DeleteObject(fill);
        DeleteObject(mark);
        RECT label = item;
        label.left += Scale(34);
        SetTextColor(dc, palette_.text);
        const std::wstring text = Utf8ToWide(effort);
        DrawTextW(dc, text.c_str(), -1, &label,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    HPEN separator = CreatePen(PS_SOLID, 1, palette_.border);
    HGDIOBJ previous_pen = SelectObject(dc, separator);
    const int divider_y = Scale(42) + static_cast<int>(reasoning_popup_efforts_.size()) * Scale(32) + Scale(10);
    MoveToEx(dc, Scale(10), divider_y, nullptr);
    LineTo(dc, bounds.right - Scale(10), divider_y);
    SelectObject(dc, previous_pen);
    DeleteObject(separator);

    RECT default_label{Scale(12), divider_y + Scale(4), bounds.right - Scale(12), divider_y + Scale(26)};
    SetTextColor(dc, palette_.muted);
    DrawTextW(dc, Text(L"Default level"), -1, &default_label,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    for (size_t index = 0; index < reasoning_popup_chip_rects_.size(); ++index) {
        const RECT& item = reasoning_popup_chip_rects_[index];
        const bool automatic = index == 0;
        const bool checked = automatic ? capabilities.default_reasoning_effort.empty()
            : capabilities.default_reasoning_effort == reasoning_popup_efforts_[index - 1];
        const bool enabled = automatic || std::find(selected.begin(), selected.end(),
            reasoning_popup_efforts_[index - 1]) != selected.end();
        const bool hovered = enabled && static_cast<int>(index) == reasoning_popup_hover_chip_;
        HBRUSH chip_fill = CreateSolidBrush(checked || hovered ? palette_.selection : palette_.input);
        HPEN chip_border = CreatePen(PS_SOLID, 1, checked ? palette_.muted : palette_.border);
        HGDIOBJ previous_brush = SelectObject(dc, chip_fill);
        HGDIOBJ chip_old_pen = SelectObject(dc, chip_border);
        RoundRect(dc, item.left, item.top, item.right, item.bottom, Scale(6), Scale(6));
        SelectObject(dc, chip_old_pen);
        SelectObject(dc, previous_brush);
        DeleteObject(chip_fill);
        DeleteObject(chip_border);
        RECT text = item;
        SetTextColor(dc, enabled ? palette_.text : palette_.subtle);
        const std::wstring label = automatic ? std::wstring(Text(L"Auto"))
            : Utf8ToWide(reasoning_popup_efforts_[index - 1]);
        DrawTextW(dc, label.c_str(), -1, &text,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    RECT reset = reasoning_popup_reset_rect_;
    if (reasoning_popup_hover_reset_) {
        HBRUSH hover = CreateSolidBrush(palette_.selection);
        FillRect(dc, &reset, hover);
        DeleteObject(hover);
    }
    const bool overridden = !capabilities.reasoning_efforts.empty() ||
        !capabilities.default_reasoning_effort.empty() || !capabilities.reasoning;
    SetTextColor(dc, overridden ? palette_.muted : palette_.subtle);
    DrawTextW(dc, Text(L"Use built-in defaults"), -1, &reset,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    const RECT& done = reasoning_popup_done_rect_;
    if (reasoning_popup_hover_done_) FillRect(dc, &done, input_);
    HPEN close_pen = CreatePen(PS_SOLID, std::max(1, Scale(1)), palette_.muted);
    previous_pen = SelectObject(dc, close_pen);
    const int close_x = (done.left + done.right) / 2, close_y = (done.top + done.bottom) / 2;
    MoveToEx(dc, close_x - Scale(4), close_y - Scale(4), nullptr); LineTo(dc, close_x + Scale(4), close_y + Scale(4));
    MoveToEx(dc, close_x + Scale(4), close_y - Scale(4), nullptr); LineTo(dc, close_x - Scale(4), close_y + Scale(4));
    SelectObject(dc, previous_pen); DeleteObject(close_pen);
    SelectObject(dc, old_font);
}

void ProviderEditor::Layout() {
    if (!body_) return;
    RECT bounds{}; GetClientRect(window_, &bounds);
    const int width = bounds.right, gap = Scale(16), field_height = Scale(36);
    const bool compact = width < Scale(580);
    const bool compact_models = width < Scale(960);
    HDC dc = GetDC(window_); HGDIOBJ old = SelectObject(dc, font_); SIZE text{};
    const auto save_label = ReadWindowText(save_);
    GetTextExtentPoint32W(dc, save_label.c_str(), static_cast<int>(save_label.size()), &text);
    SelectObject(dc, old); ReleaseDC(window_, dc);
    const int save_width = std::min(width, static_cast<int>(text.cx) + Scale(46)), header = Scale(compact ? 132 : 84);
    dc = GetDC(window_); old = SelectObject(dc, font_);
    auto button_width = [&](HWND button) {
        const auto caption = ReadWindowText(button);
        GetTextExtentPoint32W(dc, caption.c_str(), static_cast<int>(caption.size()), &text);
        return static_cast<int>(text.cx) + Scale(button == add_model_ ? 54 : 24);
    };
    const int add_width = button_width(add_model_), fetch_width = button_width(fetch_models_);
    const int clear_width = button_width(clear_models_), load_width = button_width(load_config_);
    const int protocol_min_width = std::max({button_width(responses_), button_width(chat_),
                                            button_width(anthropic_)});
    SelectObject(dc, old); ReleaseDC(window_, dc);
    auto place = [](HWND control, int x, int y, int w, int h) {
        RECT rect{}; GetWindowRect(control, &rect);
        MapWindowPoints(nullptr, GetParent(control), reinterpret_cast<POINT*>(&rect), 2);
        w = std::max(1, w);
        if (rect.left == x && rect.top == y && rect.right - rect.left == w && rect.bottom - rect.top == h) return;
        SetWindowPos(control, nullptr, x, y, w, h,
            SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS);
    };
    place(back_, 0, 0, Scale(32), Scale(36));
    place(title_, Scale(42), Scale(2), width - Scale(42) - (compact ? 0 : save_width + gap), Scale(34));
    place(save_, width - save_width, Scale(compact ? 48 : 0), save_width, field_height);
    place(status_, 0, Scale(compact ? 94 : 50), width, Scale(32));
    place(body_, 0, header, width, std::max(1L, bounds.bottom - header));
    RECT body{}; GetClientRect(body_, &body);
    const int available = std::max(1, width - Scale(22)), column = (available - gap) / 2;
    auto field = [&](HWND control, int x, int y, int w) {
        place(control, x + Scale(9), y + Scale(8), w - Scale(18), Scale(20));
    };
    auto label_field = [&](HWND label, HWND control, int x, int y, int w) {
        place(label, x, y - scroll_, w, Scale(24)); field(control, x, y + Scale(28) - scroll_, w);
    };
    int y = Scale(8);
    label_field(labels_[0], name_, Scale(2), y, compact ? available - Scale(2) : column);
    const int protocol_y = compact ? y + Scale(86) : y, protocol_x = compact ? Scale(2) : column + gap;
    const int protocol_width = compact ? available - Scale(2) : column;
    place(labels_[3], protocol_x, protocol_y - scroll_, protocol_width, Scale(24));
    const int protocol_gap = Scale(8);
    const int protocol_columns = std::clamp((protocol_width + protocol_gap) /
        (protocol_min_width + protocol_gap), 1, 3);
    const int protocol_button_width = (protocol_width - (protocol_columns - 1) * protocol_gap) / protocol_columns;
    const HWND protocols[] = {responses_, chat_, anthropic_};
    for (int index = 0; index < 3; ++index) {
        place(protocols[index], protocol_x + (index % protocol_columns) * (protocol_button_width + protocol_gap),
              protocol_y + Scale(28) + (index / protocol_columns) * (field_height + protocol_gap) - scroll_,
              protocol_button_width, field_height);
    }
    y += Scale(compact ? 172 : 86) + (2 / protocol_columns) * (field_height + protocol_gap);
    label_field(labels_[1], url_, Scale(2), y, compact ? available - Scale(2) : column);
    const int key_y = compact ? y + Scale(86) : y, key_x = compact ? Scale(2) : column + gap;
    label_field(labels_[2], key_, key_x, key_y, (compact ? available : column) - Scale(40));
    place(reveal_, key_x + (compact ? available : column) - Scale(36), key_y + Scale(28) - scroll_, Scale(36), field_height);
    y += Scale(compact ? 184 : 98);
    place(labels_[4], Scale(2), y - scroll_, available, Scale(28));
    y += Scale(34);
    int action_x = Scale(2);
    for (const auto& action : {std::make_pair(add_model_, add_width), std::make_pair(fetch_models_, fetch_width),
                               std::make_pair(clear_models_, clear_width)}) {
        const int action_width = std::min(available - Scale(2), action.second);
        if (action_x > Scale(2) && action_x + action_width > available) { y += Scale(46); action_x = Scale(2); }
        place(action.first, action_x, y - scroll_, action_width, field_height);
        action_x += action_width + Scale(8);
    }
    y += Scale(50);
    for (int index = 5; index <= 9; ++index) ShowWindow(labels_[index], compact_models ? SW_HIDE : SW_SHOW);
    if (!compact_models) {
        const int remove_width = Scale(32), reasoning_width = Scale(184), images_width = Scale(136);
        dc = GetDC(window_); old = SelectObject(dc, font_);
        const std::wstring compact_default = i18n::Format(L"Default ({value}%)",
            {{L"value", std::to_wstring(context_.auto_compact_threshold_percent)}});
        GetTextExtentPoint32W(dc, compact_default.c_str(), static_cast<int>(compact_default.size()), &text);
        SelectObject(dc, old); ReleaseDC(window_, dc);
        const int compact_width = std::max(Scale(110), static_cast<int>(text.cx) + Scale(48));
        const int context_width = Scale(100), column_gap = Scale(8);
        const int id_width = std::max(Scale(120), available - remove_width - reasoning_width - images_width -
            compact_width - context_width - column_gap * 5);
        const int context_x = Scale(2) + id_width + column_gap;
        const int compact_x = context_x + context_width + column_gap;
        const int images_x = compact_x + compact_width + column_gap;
        const int reasoning_x = images_x + images_width + column_gap;
        place(labels_[5], Scale(2), y - scroll_, id_width, Scale(24));
        place(labels_[6], context_x, y - scroll_, context_width, Scale(24));
        place(labels_[7], compact_x, y - scroll_, compact_width, Scale(24));
        place(labels_[8], images_x, y - scroll_, images_width, Scale(24));
        place(labels_[9], reasoning_x, y - scroll_, reasoning_width, Scale(24));
        y += Scale(30);
        for (auto& row : rows_) {
            SetCaption(row.images, Text(row.model.capabilities.images
                ? L"Send original images" : L"Do not send images"));
            SetReasoningButton(row, false);
            field(row.id, Scale(2), y - scroll_, id_width);
            field(row.context, context_x, y - scroll_, context_width);
            place(row.auto_compact, compact_x, y - scroll_, compact_width, field_height);
            place(row.images, images_x, y - scroll_, images_width, field_height);
            place(row.reasoning, reasoning_x, y - scroll_, reasoning_width, field_height);
            place(row.remove, available - remove_width, y - scroll_, remove_width, field_height);
            y += Scale(50);
        }
    } else {
        for (auto& row : rows_) {
            const int half = (available - Scale(10)) / 2;
            SetCaption(row.images, std::wstring(Text(L"Image")) + L": " +
                Text(row.model.capabilities.images ? L"Send original images" : L"Do not send images"));
            SetReasoningButton(row, true);
            field(row.id, Scale(2), y - scroll_, available - Scale(42));
            place(row.remove, available - Scale(32), y - scroll_, Scale(32), field_height);
            field(row.context, Scale(2), y + Scale(42) - scroll_, half);
            place(row.auto_compact, Scale(2) + half + Scale(10), y + Scale(42) - scroll_,
                  half, field_height);
            place(row.images, Scale(2), y + Scale(84) - scroll_, half, field_height);
            place(row.reasoning, Scale(2) + half + Scale(10), y + Scale(84) - scroll_,
                  half, field_height);
            y += Scale(126);
        }
    }
    label_field(labels_[10], test_model_, Scale(2), y + Scale(12), available - Scale(2));
    y += Scale(98);
    place(preview_label_, Scale(2), y - scroll_, available, Scale(28));
    y += Scale(34);
    place(load_config_, Scale(2), y - scroll_, std::min(available, load_width), field_height);
    y += Scale(50);
    place(preview_, Scale(10), y + Scale(8) - scroll_, available - Scale(18), Scale(260));
    content_height_ = y + Scale(290);
    const int clamped = std::clamp(scroll_, 0, std::max(0, content_height_ - static_cast<int>(body.bottom)));
    if (scroll_ != clamped) { scroll_ = clamped; Layout(); return; }
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN | RDW_NOERASE);
}

RECT ProviderEditor::ScrollThumb() const {
    RECT rect{}; GetClientRect(body_, &rect);
    if (content_height_ <= rect.bottom) return {};
    const int height = std::max(Scale(32), MulDiv(rect.bottom, rect.bottom, content_height_));
    const int top = MulDiv(scroll_, rect.bottom - height, content_height_ - rect.bottom);
    return {rect.right - Scale(7), top, rect.right - Scale(2), top + height};
}

void ProviderEditor::Scroll(int position) {
    CloseReasoningPopup();
    RECT rect{}; GetClientRect(body_, &rect);
    const int next = std::clamp(position, 0, std::max(0, content_height_ - static_cast<int>(rect.bottom)));
    if (next != scroll_) { scroll_ = next; Layout(); }
}

void ProviderEditor::Save() {
    status_ok_ = false;
    if (json_pending_) { if (!ApplyPreview()) return; }
    else if (!json_valid_) return;
    else RefreshPreview();
    if (!json_valid_) return;
    const auto result = ReadDraft(true);
    if (!result.ok) { SetWindowTextW(status_, Utf8ToWide(result.error).c_str()); return; }
    const auto saved = on_save_(result.value);
    if (!saved.ok) {
        SetWindowTextW(status_, i18n::Format(L"Save failed: {error}", {{L"error", Utf8ToWide(saved.error)}}).c_str()); return;
    }
    on_back_();
}

void ProviderEditor::DrawButton(const DRAWITEMSTRUCT& item) {
    const int id = GetDlgCtrlID(item.hwndItem), saved = SaveDC(item.hDC);
    const bool responses = draft_.protocol == "responses" || draft_.protocol == "openai_responses";
    const bool chat = draft_.protocol == "chat_completions" || draft_.protocol == "openai_chat_completions";
    const bool selected = (id == Responses && responses) || (id == Chat && chat) ||
        (id == Anthropic && draft_.protocol == "anthropic_messages");
    HBRUSH brush = CreateSolidBrush(selected || (item.itemState & ODS_SELECTED) ? palette_.selection : palette_.panel);
    HPEN pen = CreatePen(PS_SOLID, 1, selected ? palette_.muted : palette_.border);
    FillRect(item.hDC, &item.rcItem, background_);
    SelectObject(item.hDC, brush); SelectObject(item.hDC, pen);
    RoundRect(item.hDC, 0, 0, item.rcItem.right, item.rcItem.bottom, Scale(6), Scale(6));
    HPEN glyph = CreatePen(PS_SOLID, std::max(1, Scale(1)), palette_.text);
    SelectObject(item.hDC, glyph);
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, IsWindowEnabled(item.hwndItem) ? palette_.text : palette_.subtle);
    SelectObject(item.hDC, font_);
    RECT text = item.rcItem; text.left += Scale(8); text.right -= Scale(8);
    if (id == SaveButton || id == AddModelButton) {
        const int x = Scale(18), y = item.rcItem.bottom / 2, s = Scale(6);
        text.left += Scale(24);
        if (id == SaveButton) {
            Rectangle(item.hDC, x - s, y - s, x + s, y + s);
            Rectangle(item.hDC, x - Scale(3), y + Scale(1), x + Scale(3), y + s);
        } else {
            MoveToEx(item.hDC, x - s, y, nullptr); LineTo(item.hDC, x + s, y);
            MoveToEx(item.hDC, x, y - s, nullptr); LineTo(item.hDC, x, y + s);
        }
    }
    const bool image_choice = std::any_of(rows_.begin(), rows_.end(),
        [&](const ModelRow& row) { return row.images == item.hwndItem; });
    const bool auto_compact_choice = std::any_of(rows_.begin(), rows_.end(),
        [&](const ModelRow& row) { return row.auto_compact == item.hwndItem; });
    const bool reasoning_choice = std::any_of(rows_.begin(), rows_.end(),
        [&](const ModelRow& row) { return row.reasoning == item.hwndItem; });
    if (image_choice || auto_compact_choice || reasoning_choice) {
        const int x = item.rcItem.right - Scale(18), y = item.rcItem.bottom / 2;
        text.right -= Scale(24);
        MoveToEx(item.hDC, x - Scale(4), y - Scale(2), nullptr);
        LineTo(item.hDC, x, y + Scale(2));
        LineTo(item.hDC, x + Scale(4), y - Scale(2));
    }
    const auto caption = ReadWindowText(item.hwndItem);
    if (!caption.empty()) DrawTextW(item.hDC, caption.c_str(), -1, &text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    else {
        const int x = item.rcItem.right / 2, y = item.rcItem.bottom / 2, s = Scale(6);
        SelectObject(item.hDC, GetStockObject(HOLLOW_BRUSH));
        if (id == Back) {
            MoveToEx(item.hDC, x + s, y, nullptr); LineTo(item.hDC, x - s, y);
            LineTo(item.hDC, x, y - s); MoveToEx(item.hDC, x - s, y, nullptr); LineTo(item.hDC, x, y + s);
        } else if (id == Reveal) {
            Ellipse(item.hDC, x - Scale(9), y - s, x + Scale(9), y + s);
            Ellipse(item.hDC, x - Scale(2), y - Scale(2), x + Scale(2), y + Scale(2));
        } else {
            Rectangle(item.hDC, x - s, y - s, x + s, y + Scale(8));
            MoveToEx(item.hDC, x - Scale(8), y - Scale(8), nullptr); LineTo(item.hDC, x + Scale(8), y - Scale(8));
        }
    }
    if (item.itemState & ODS_FOCUS) {
        RECT focus = item.rcItem; InflateRect(&focus, -Scale(3), -Scale(3)); DrawFocusRect(item.hDC, &focus);
    }
    RestoreDC(item.hDC, saved); DeleteObject(glyph); DeleteObject(pen); DeleteObject(brush);
}

LRESULT CALLBACK ProviderEditor::ControlProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
                                             UINT_PTR id, DWORD_PTR data) {
    auto* self = reinterpret_cast<ProviderEditor*>(data);
    if (message == WM_ERASEBKGND && (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_OWNERDRAW)
        return 1;
    if (message == WM_PAINT || message == WM_PRINTCLIENT) {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        const bool context_field = std::any_of(self->rows_.begin(), self->rows_.end(),
            [window](const ModelRow& row) { return row.context == window; });
        if (context_field && GetWindowTextLengthW(window) == 0) {
            HDC dc = message == WM_PAINT ? GetDC(window) : reinterpret_cast<HDC>(wparam);
            if (dc) {
                const int saved = SaveDC(dc);
                RECT rect{}; SendMessageW(window, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&rect));
                SelectObject(dc, self->font_);
                SetBkMode(dc, TRANSPARENT); SetTextColor(dc, self->palette_.muted);
                const std::wstring hint = std::to_wstring(self->context_.fallback_context_window);
                DrawTextW(dc, hint.c_str(), -1, &rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
                RestoreDC(dc, saved);
                if (message == WM_PAINT) ReleaseDC(window, dc);
            }
        }
        return result;
    }
    if (message == WM_MOUSEWHEEL && window == self->preview_) {
        UINT lines = 3; SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
        if (lines == WHEEL_PAGESCROLL) SendMessageW(window, EM_SCROLL,
            GET_WHEEL_DELTA_WPARAM(wparam) > 0 ? SB_PAGEUP : SB_PAGEDOWN, 0);
        else SendMessageW(window, EM_LINESCROLL, 0,
            -GET_WHEEL_DELTA_WPARAM(wparam) * static_cast<int>(std::min<UINT>(lines, 100)) / WHEEL_DELTA);
        return 0;
    }
    if (message == WM_MOUSEWHEEL && window != self->preview_) return SendMessageW(self->body_, message, wparam, lparam);
    if (window == self->preview_ && message == WM_KEYDOWN && wparam == 'A' && GetKeyState(VK_CONTROL) < 0) {
        SendMessageW(window, EM_SETSEL, 0, -1); return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_TAB) {
        HWND next = GetNextDlgTabItem(self->window_, window, GetKeyState(VK_SHIFT) < 0);
        if (next) SetFocus(next);
        return 0;
    }
    if (message == WM_SETFOCUS && GetParent(window) == self->body_) {
        RECT rect{}, bounds{}; GetWindowRect(window, &rect); GetClientRect(self->body_, &bounds);
        MapWindowPoints(nullptr, self->body_, reinterpret_cast<POINT*>(&rect), 2);
        if (rect.top < 0) self->Scroll(self->scroll_ + rect.top - self->Scale(8));
        else if (rect.bottom > bounds.bottom) self->Scroll(self->scroll_ + rect.bottom - bounds.bottom + self->Scale(8));
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, ControlProc, id);
    return DefSubclassProc(window, message, wparam, lparam);
}

LRESULT CALLBACK ProviderEditor::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<ProviderEditor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<ProviderEditor*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (!self) return DefWindowProcW(window, message, wparam, lparam);
    if (window == self->reasoning_popup_) {
        switch (message) {
            case WM_ERASEBKGND: return 1;
            case WM_PAINT:
            case WM_PRINTCLIENT: {
                PAINTSTRUCT paint{};
                HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
                self->DrawReasoningPopup(dc);
                if (message == WM_PAINT) EndPaint(window, &paint);
                return 0;
            }
            case WM_LBUTTONUP:
                self->HandleReasoningPopupClick({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
                return 0;
            case WM_LBUTTONDOWN: return 0;
            case WM_RBUTTONUP:
            case WM_MOUSEWHEEL: self->CloseReasoningPopup(); return 0;
            case WM_MOUSEMOVE: {
                const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                int effort = -1, chip = -1;
                for (size_t index = 0; index < self->reasoning_popup_effort_rects_.size(); ++index)
                    if (PtInRect(&self->reasoning_popup_effort_rects_[index], point)) effort = static_cast<int>(index);
                for (size_t index = 0; index < self->reasoning_popup_chip_rects_.size(); ++index)
                    if (PtInRect(&self->reasoning_popup_chip_rects_[index], point)) chip = static_cast<int>(index);
                const bool reset = PtInRect(&self->reasoning_popup_reset_rect_, point) != FALSE;
                const bool done = PtInRect(&self->reasoning_popup_done_rect_, point) != FALSE;
                if (effort != self->reasoning_popup_hover_effort_ || chip != self->reasoning_popup_hover_chip_ ||
                    reset != self->reasoning_popup_hover_reset_ || done != self->reasoning_popup_hover_done_) {
                    self->reasoning_popup_hover_effort_ = effort; self->reasoning_popup_hover_chip_ = chip;
                    self->reasoning_popup_hover_reset_ = reset; self->reasoning_popup_hover_done_ = done;
                    self->UpdateReasoningPopup();
                }
                return 0;
            }
            case WM_KEYDOWN: {
                if (wparam == VK_ESCAPE) { self->CloseReasoningPopup(); return 0; }
                if (wparam == VK_DOWN || wparam == VK_UP) {
                    const int count = static_cast<int>(self->reasoning_popup_efforts_.size());
                    if (count > 0) self->reasoning_popup_hover_effort_ =
                        (self->reasoning_popup_hover_effort_ + (wparam == VK_DOWN ? 1 : count - 1) + count) % count;
                    self->reasoning_popup_hover_chip_ = -1;
                } else if (wparam == VK_TAB || wparam == VK_LEFT || wparam == VK_RIGHT) {
                    const int count = static_cast<int>(self->reasoning_popup_chip_rects_.size());
                    const bool previous = wparam == VK_LEFT || (wparam == VK_TAB && GetKeyState(VK_SHIFT) < 0);
                    self->reasoning_popup_hover_chip_ =
                        (self->reasoning_popup_hover_chip_ + (previous ? count - 1 : 1) + count) % count;
                    self->reasoning_popup_hover_effort_ = -1;
                } else if (wparam == VK_RETURN || wparam == VK_SPACE) {
                    RECT rect{};
                    if (self->reasoning_popup_hover_effort_ >= 0)
                        rect = self->reasoning_popup_effort_rects_[static_cast<size_t>(self->reasoning_popup_hover_effort_)];
                    else if (self->reasoning_popup_hover_chip_ >= 0)
                        rect = self->reasoning_popup_chip_rects_[static_cast<size_t>(self->reasoning_popup_hover_chip_)];
                    else { self->CloseReasoningPopup(); return 0; }
                    self->HandleReasoningPopupClick({(rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2});
                }
                self->UpdateReasoningPopup(); return 0;
            }
            case WM_ACTIVATE:
                if (LOWORD(wparam) == WA_INACTIVE) self->CloseReasoningPopup();
                return 0;
            case WM_CAPTURECHANGED:
            case WM_CLOSE: self->CloseReasoningPopup(); return 0;
            case WM_NCDESTROY: self->reasoning_popup_ = nullptr; self->reasoning_popup_row_ = -1; break;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    switch (message) {
        case WM_SIZE:
            if (window == self->window_) { self->CloseReasoningPopup(); self->Layout(); }
            return 0;
        case WM_ERASEBKGND: return 1;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, reinterpret_cast<HWND>(lparam) == self->status_ && !self->status_ok_
                ? self->palette_.error : self->palette_.text);
            SetBkColor(dc, message == WM_CTLCOLOREDIT ? self->palette_.input : self->palette_.background);
            return reinterpret_cast<LRESULT>(message == WM_CTLCOLOREDIT ? self->input_ : self->background_);
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = reinterpret_cast<HDC>(wparam);
            SetTextColor(dc, self->palette_.text);
            SetBkColor(dc, self->palette_.input);
            return reinterpret_cast<LRESULT>(self->input_);
        }
        case WM_MEASUREITEM: {
            auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_MENU) {
                item->itemWidth = self->Scale(160);
                item->itemHeight = self->Scale(30);
                return TRUE;
            }
            break;
        }
        case WM_DRAWITEM: {
            auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item->CtlType == ODT_MENU) {
                const int saved = SaveDC(item->hDC);
                HBRUSH fill = CreateSolidBrush(item->itemState & ODS_SELECTED ? self->palette_.selection : self->palette_.panel);
                FillRect(item->hDC, &item->rcItem, fill); DeleteObject(fill);
                wchar_t caption[128]{};
                MENUITEMINFOW info{sizeof(info)}; info.fMask = MIIM_STRING;
                info.dwTypeData = caption; info.cch = static_cast<UINT>(std::size(caption));
                GetMenuItemInfoW(reinterpret_cast<HMENU>(item->hwndItem), item->itemID, FALSE, &info);
                SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, self->palette_.text);
                SelectObject(item->hDC, self->font_);
                RECT text = item->rcItem; text.left += self->Scale(28);
                DrawTextW(item->hDC, caption, -1, &text, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                if (item->itemState & ODS_CHECKED) {
                    HPEN pen = CreatePen(PS_SOLID, self->Scale(2), self->palette_.accent);
                    HGDIOBJ old = SelectObject(item->hDC, pen);
                    const int x = item->rcItem.left + self->Scale(9), y = (text.top + text.bottom) / 2;
                    MoveToEx(item->hDC, x, y, nullptr); LineTo(item->hDC, x + self->Scale(4), y + self->Scale(4));
                    LineTo(item->hDC, x + self->Scale(11), y - self->Scale(5));
                    SelectObject(item->hDC, old); DeleteObject(pen);
                }
                RestoreDC(item->hDC, saved);
                return TRUE;
            }
            self->DrawButton(*item);
            return TRUE;
        }
        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            if (HIWORD(wparam) == EN_CHANGE || HIWORD(wparam) == CBN_EDITCHANGE ||
                HIWORD(wparam) == CBN_SELCHANGE) {
                self->DraftChanged(reinterpret_cast<HWND>(lparam) == self->preview_, reinterpret_cast<HWND>(lparam)); return 0;
            }
            if (HIWORD(wparam) != BN_CLICKED) break;
            if (id == Back) { self->on_back_(); return 0; }
            if (id == SaveButton) { self->Save(); return 0; }
            if (id == LoadConfig) { self->LoadConfiguration(); return 0; }
            if (id == FetchModels) { self->StartModelFetch(); return 0; }
            if (id == Reveal) { self->revealed_ = !self->revealed_; SendMessageW(self->key_, EM_SETPASSWORDCHAR, self->revealed_ ? 0 : L'\x25cf', 0); InvalidateRect(self->key_, nullptr, TRUE); return 0; }
            HWND command_control = reinterpret_cast<HWND>(lparam);
            if (self->json_pending_) {
                std::string model_id;
                int row_action = 0;
                for (const auto& row : self->rows_) {
                    if (command_control == row.images || command_control == row.remove ||
                        command_control == row.reasoning || command_control == row.auto_compact) {
                        model_id = Trim(WideToUtf8(ReadWindowText(row.id)));
                        row_action = command_control == row.images ? 1 : command_control == row.remove ? 2
                            : command_control == row.reasoning ? 3 : 4;
                        break;
                    }
                }
                if (id == 0) command_control = nullptr;
                if (!self->ApplyPreview()) return 0;
                for (const auto& row : self->rows_) if (!model_id.empty() && row.model.id == model_id) {
                    command_control = row_action == 1 ? row.images : row_action == 2 ? row.remove
                        : row_action == 3 ? row.reasoning : row.auto_compact;
                    break;
                }
            }
            if (!self->json_valid_) return 0;
            self->status_ok_ = false; SetWindowTextW(self->status_, L"");
            self->CancelModelFetch();
            if (id == Responses || id == Chat || id == Anthropic) {
                self->draft_.protocol = id == Responses ? "responses" :
                    id == Chat ? "chat_completions" : "anthropic_messages";
            } else if (id == AddModelButton) self->AddModel({});
            else if (id == ClearModels) self->RemoveModels();
            else for (auto it = self->rows_.begin(); it != self->rows_.end(); ++it) {
                if (it->auto_compact == command_control) {
                    self->ShowAutoCompactMenu(*it);
                    break;
                }
                if (it->images == command_control) {
                    const auto model_id = Trim(WideToUtf8(ReadWindowText(it->id)));
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING | (it->model.capabilities.images ? MF_CHECKED : 0), 1, Text(L"Send original images"));
                    AppendMenuW(menu, MF_STRING | (!it->model.capabilities.images ? MF_CHECKED : 0), 2, Text(L"Do not send images"));
                    RECT rect{}; GetWindowRect(it->images, &rect);
                    const int choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_LEFTALIGN, rect.left, rect.bottom, 0, window, nullptr);
                    DestroyMenu(menu);
                    if (choice) for (auto& row : self->rows_) {
                        if (Trim(WideToUtf8(ReadWindowText(row.id))) == model_id) { self->SetImages(row, choice == 1); break; }
                    }
                    break;
                }
                if (it->reasoning == command_control) {
                    self->ShowReasoningMenu(*it);
                    break;
                }
                if (it->remove == command_control) {
                    self->CloseReasoningPopup();
                    for (HWND control : {it->remove, it->reasoning, it->auto_compact, it->context, it->id}) {
                        TOOLINFOW tool{sizeof(tool)};
                        tool.hwnd = self->body_; tool.uId = reinterpret_cast<UINT_PTR>(control);
                        SendMessageW(self->tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
                    }
                    for (HWND control : {it->id, it->context, it->auto_compact, it->images, it->reasoning, it->remove})
                        DestroyWindow(control);
                    self->rows_.erase(it); break;
                }
            }
            self->Layout(); self->RefreshPreview();
            for (HWND protocol : {self->responses_, self->chat_, self->anthropic_})
                InvalidateRect(protocol, nullptr, FALSE);
            return 0;
        }
        case WM_TIMER:
            if (wparam == 1) { KillTimer(window, 1); self->RefreshPreview(); }
            if (wparam == 2) { KillTimer(window, 2); if (self->json_pending_) self->ApplyPreview(); }
            if (wparam == 3) self->PollModelFetch();
            return 0;
        case WM_MOUSEWHEEL: self->Scroll(self->scroll_ - GET_WHEEL_DELTA_WPARAM(wparam) * self->Scale(48) / WHEEL_DELTA); return 0;
        case WM_LBUTTONDOWN: {
            const RECT thumb = self->ScrollThumb();
            if (window == self->body_ && GET_X_LPARAM(lparam) >= thumb.left && thumb.right > 0) {
                self->drag_y_ = GET_Y_LPARAM(lparam); self->drag_scroll_ = self->scroll_; SetCapture(window);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (GetCapture() == window && self->drag_y_ >= 0) {
                RECT rect{}; GetClientRect(window, &rect); const RECT thumb = self->ScrollThumb();
                self->Scroll(self->drag_scroll_ + MulDiv(GET_Y_LPARAM(lparam) - self->drag_y_,
                    self->content_height_ - rect.bottom, std::max(1L, rect.bottom - thumb.bottom + thumb.top)));
            }
            return 0;
        case WM_LBUTTONUP: if (GetCapture() == window) ReleaseCapture(); return 0;
        case WM_CAPTURECHANGED: self->drag_y_ = -1; return 0;
        case WM_NCDESTROY:
            if (window == self->window_) self->window_ = nullptr;
            if (window == self->body_) self->body_ = nullptr;
            break;
        case WM_PAINT:
        case WM_PRINTCLIENT: {
            PAINTSTRUCT paint{};
            HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
            RECT rect{}; GetClientRect(window, &rect); FillRect(dc, &rect, self->background_);
            if (window == self->body_) {
                const int saved = SaveDC(dc);
                HPEN pen = CreatePen(PS_SOLID, 1, self->palette_.border);
                SelectObject(dc, pen); SelectObject(dc, self->input_);
                for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
                    wchar_t type[16]{}; GetClassNameW(child, type, 16);
                    if (_wcsicmp(type, L"EDIT") != 0 || !(GetWindowLongPtrW(child, GWL_STYLE) & WS_VISIBLE)) continue;
                    GetWindowRect(child, &rect); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
                    InflateRect(&rect, self->Scale(8), self->Scale(7));
                    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, self->Scale(6), self->Scale(6));
                }
                RestoreDC(dc, saved); DeleteObject(pen);
                rect = self->ScrollThumb(); HBRUSH rail = CreateSolidBrush(self->palette_.subtle);
                FillRect(dc, &rect, rail); DeleteObject(rail);
            }
            if (message == WM_PAINT) EndPaint(window, &paint);
            return 0;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}  // namespace spark
