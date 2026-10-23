#pragma once

#include "core/config.h"
#include "gui/native/controls.h"

#include <wx/panel.h>
#include <wx/timer.h>

#include <functional>
#include <memory>
#include <thread>
#include <vector>

class wxButton;
class wxCheckBox;
class wxFlexGridSizer;
class wxGridSizer;
class wxBoxSizer;
class wxPanel;
class wxScrolledWindow;
class wxSizer;
class wxSizerItem;
class wxStaticText;
class wxTextCtrl;
class wxWrapSizer;

namespace spark {

// Preserve provider/model extensions while applying the typed config's managed fields.
nlohmann::json NativeConfigDocument(const AppConfig& config);

class NativeProviderEditor : public wxPanel {
public:
    NativeProviderEditor(wxWindow* parent, ProviderConfig provider,
                         const ContextConfig& context, const std::string& theme,
                         nlohmann::json raw_provider = nlohmann::json::object());
    ~NativeProviderEditor() override;
    void SetHandlers(std::function<Result<void>(const ProviderConfig&)> save,
                     std::function<void()> back);
    const ProviderConfig& GetProvider() const { return provider_; }
    const nlohmann::json& GetProviderJson() const { return raw_provider_; }

private:
    struct Fetch;
    struct ModelRow {
        ModelConfig model;
        std::string original_id;
        std::vector<int> auto_compact_options;
        int minimum_widths[6]{-1, -1, -1, -1, -1, -1};
        wxPanel* panel = nullptr;
        wxSizer* layout = nullptr;
        wxSizerItem* sizer_item = nullptr;
        wxTextCtrl* id = nullptr;
        wxTextCtrl* context = nullptr;
        wxStaticText* labels[3]{};
        NativeChoice* auto_compact = nullptr;
        NativeChoice* images = nullptr;
        NativeButton* reasoning = nullptr;
        NativeButton* remove = nullptr;
        bool compact_layout = false;
        bool layout_initialized = false;
    };
    bool StoreModels(bool validate);
    void AddModel(ModelConfig model);
    void RemoveModel(size_t index);
    void ClearModels();
    void LayoutModels();
    void UpdateModelRow(size_t index);
    void ShowReasoningMenu(size_t index);
    size_t FindModelRow(wxWindow* panel) const;
    void LayoutHeader();
    void LayoutProviderFields();
    void SelectProtocol(int index);
    Result<ProviderConfig> ReadProvider(bool validate = true);
    void Preview();
    bool ApplyPreview();
    void LoadConfiguration();
    void StartFetch();
    void Poll(wxTimerEvent&);
    void Changed();
    void Save();

    ProviderConfig provider_;
    nlohmann::json raw_provider_;
    ContextConfig context_;
    bool syncing_ = false;
    bool oversized_ = false;
    bool dark_theme_ = false;
    bool json_pending_ = false, form_pending_ = false;
    int debounce_ = 0;
    unsigned revision_ = 0;
    wxTimer timer_;
    std::shared_ptr<Fetch> fetch_;
    std::thread fetch_worker_;
    wxScrolledWindow* body_ = nullptr;
    wxPanel* header_panel_ = nullptr;
    wxBoxSizer* header_row_ = nullptr;
    wxBoxSizer* header_save_row_ = nullptr;
    bool compact_header_ = false;
    wxFlexGridSizer* provider_fields_ = nullptr;
    wxBoxSizer* content_ = nullptr;
    wxBoxSizer* model_rows_ = nullptr;
    wxSizerItem* model_header_item_ = nullptr;
    wxSizerItem* model_rows_item_ = nullptr;
    wxPanel* model_header_ = nullptr;
    wxFlexGridSizer* model_header_layout_ = nullptr;
    wxWrapSizer* model_action_layout_ = nullptr;
    wxGridSizer* protocol_grid_ = nullptr;
    std::vector<ModelRow> model_rows_data_;
    bool compact_models_ = false;
    int selected_protocol_ = wxNOT_FOUND;
    NativeButton* back_button_ = nullptr;
    NativeButton* protocol_buttons_[3]{};
    wxTextCtrl *name_ = nullptr, *url_ = nullptr, *key_ = nullptr, *test_model_ = nullptr;
    wxTextCtrl *preview_ = nullptr;
    wxTextCtrl *visible_key_ = nullptr;
    wxCheckBox* reveal_ = nullptr;
    wxStaticText *title_ = nullptr, *status_ = nullptr, *effective_ = nullptr;
    NativeButton *add_model_button_ = nullptr, *fetch_button_ = nullptr, *clear_models_button_ = nullptr;
    NativeButton* save_button_ = nullptr;
    std::function<Result<void>(const ProviderConfig&)> on_save_;
    std::function<void()> on_back_;
};

}  // namespace spark
