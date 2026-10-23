#pragma once

#include "core/config.h"
#include "gui/theme.h"

#include <functional>
#include <memory>

namespace spark {

class ProviderEditor {
public:
    ~ProviderEditor() { Close(); }
    void Open(HWND parent, const std::string& theme, HFONT font, HFONT heading, ProviderConfig provider,
              const ContextConfig& context,
              std::function<Result<void>(ProviderConfig)> save, std::function<void()> back);
    void Close();
    void Resize(int x, int y, int width, int height);
    HWND Window() const { return window_; }

private:
    friend struct SettingsProviderTest;
    enum Id { Back = 510, SaveButton = 511, Name, Url, Key, Reveal, Responses, Chat, AddModelButton,
              TestModel = 520, Status, Preview, LoadConfig, FetchModels, ClearModels, Anthropic };
    struct ModelRow {
        ModelConfig model;
        std::vector<int> auto_compact_options;
        int auto_compact_percent = 0;
        HWND id = nullptr, context = nullptr, auto_compact = nullptr, images = nullptr;
        HWND reasoning = nullptr, remove = nullptr;
    };
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    static LRESULT CALLBACK ControlProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    HWND Control(HWND parent, const wchar_t* type, const wchar_t* text, int id, DWORD style);
    HWND Field(const wchar_t* text, int id, bool password = false);
    void Tip(HWND control, const wchar_t* text);
    void AddModel(ModelConfig model);
    void SetAutoCompactButton(ModelRow& row);
    void ShowAutoCompactMenu(ModelRow& row);
    void SetImages(ModelRow& row, bool enabled);
    void SetReasoningButton(ModelRow& row, bool include_label = false);
    void ShowReasoningMenu(ModelRow& row);
    void CloseReasoningPopup();
    void LayoutReasoningPopup();
    void HandleReasoningPopupClick(POINT point);
    void UpdateReasoningPopup();
    void DrawReasoningPopup(HDC dc);
    void RemoveModels();
    Result<ProviderConfig> ReadDraft(bool validate) const;
    void DraftChanged(bool json, HWND control = nullptr);
    void RefreshPreview();
    bool ApplyPreview();
    void LoadConfiguration(const std::wstring& path = {});
    void CancelModelFetch();
    void StartModelFetch();
    void PollModelFetch();
    void Layout();
    void Scroll(int position);
    RECT ScrollThumb() const;
    void DrawButton(const DRAWITEMSTRUCT& item);
    void Save();
    int Scale(int value) const;

    HWND window_ = nullptr, body_ = nullptr, back_ = nullptr, title_ = nullptr, save_ = nullptr, status_ = nullptr;
    HWND name_ = nullptr, url_ = nullptr, key_ = nullptr, reveal_ = nullptr, responses_ = nullptr, chat_ = nullptr;
    HWND anthropic_ = nullptr, add_model_ = nullptr, test_model_ = nullptr;
    HWND preview_ = nullptr, preview_label_ = nullptr, load_config_ = nullptr;
    HWND fetch_models_ = nullptr, clear_models_ = nullptr;
    HWND tooltip_ = nullptr, reasoning_popup_ = nullptr;
    std::vector<HWND> labels_;
    std::vector<ModelRow> rows_;
    std::vector<RECT> reasoning_popup_effort_rects_, reasoning_popup_chip_rects_;
    std::vector<std::string> reasoning_popup_efforts_;
    ProviderConfig draft_;
    ContextConfig context_;
    theme::Palette palette_;
    HBRUSH background_ = nullptr, input_ = nullptr;
    HFONT font_ = nullptr, heading_ = nullptr, code_font_ = nullptr;
    RECT reasoning_popup_reset_rect_{}, reasoning_popup_done_rect_{};
    int scroll_ = 0, content_height_ = 0, drag_y_ = -1, drag_scroll_ = 0, dpi_ = 96;
    int reasoning_popup_row_ = -1, reasoning_popup_hover_effort_ = -1, reasoning_popup_hover_chip_ = -1;
    bool reasoning_popup_hover_reset_ = false, reasoning_popup_hover_done_ = false;
    bool revealed_ = false;
    bool syncing_ = false, json_pending_ = false, json_valid_ = true, status_ok_ = false;
    struct ModelFetch;
    std::shared_ptr<ModelFetch> fetch_;
    std::function<Result<void>(ProviderConfig)> on_save_;
    std::function<void()> on_back_;
};

}  // namespace spark
