#include "gui/native/provider_editor.h"

#include "core/provider_models.h"
#include "gui/localization.h"
#include "gui/reasoning_summary.h"
#include "gui/native/popup_menu.h"
#include "gui/native/reasoning_popup.h"

#include <wx/artprov.h>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/checklst.h>
#include <wx/choice.h>
#include <wx/dcclient.h>
#include <wx/filedlg.h>
#include <wx/listbox.h>
#include <wx/menu.h>
#include <wx/scrolwin.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/wrapsizer.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>

namespace spark {
namespace {
constexpr size_t kMaxModels = 256;
constexpr size_t kMaxHeaders = 65536;
constexpr size_t kMaxPreview = 1024 * 1024;

wxString U(const std::string& text) { return wxString::FromUTF8(text.data(), text.size()); }
std::string S(const wxString& text) { return text.ToStdString(wxConvUTF8); }
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }

void Paint(wxWindow* window, bool dark) {
    const bool input = wxDynamicCast(window, wxTextCtrl) || wxDynamicCast(window, wxListBox) ||
                       wxDynamicCast(window, wxChoice) || wxDynamicCast(window, wxSpinCtrl);
    window->SetBackgroundColour(dark ? (input ? wxColour(48, 49, 52) : wxColour(28, 29, 31))
                                    : (input ? wxColour(255, 255, 255) : wxColour(250, 250, 250)));
    window->SetForegroundColour(dark ? wxColour(236, 237, 240) : wxColour(31, 32, 35));
    for (wxWindow* child : window->GetChildren()) Paint(child, dark);
}

NativeButton* Tool(wxWindow* parent, wxSizer* row, const wxArtID& art, const wxString& tip) {
    auto* button = new NativeButton(parent, {}, art);
    button->SetToolTip(tip);
    button->SetName(tip);
    row->Add(button, 0, wxRIGHT, parent->FromDIP(6));
    return button;
}

Result<int> ContextCount(std::string text) {
    text = LowerAscii(Trim(std::move(text)));
    double multiplier = 1;
    if (!text.empty() && (text.back() == 'k' || text.back() == 'm')) {
        multiplier = text.back() == 'k' ? 1000 : 1000000;
        text.pop_back();
        if (text.empty()) return Result<int>::Failure("Invalid context window.");
    }
    if (text.empty()) return Result<int>::Success(0);
    try {
        size_t end = 0;
        const double count = std::stod(text, &end) * multiplier;
        if (end != text.size() || !std::isfinite(count) || count < 0 ||
            count > std::numeric_limits<int>::max() || count != std::floor(count)) {
            return Result<int>::Failure("Use a token count, 200K, or 1M; 0 inherits.");
        }
        return Result<int>::Success(static_cast<int>(count));
    } catch (...) {
        return Result<int>::Failure("Use a token count, 200K, or 1M; 0 inherits.");
    }
}
}  // namespace

nlohmann::json NativeConfigDocument(const AppConfig& config) {
    auto document = ConfigToJson(config);
    const auto raw = config.raw.is_object() ? config.raw.value("providers", nlohmann::json::object())
                                           : nlohmann::json::object();
    auto overlay = [](nlohmann::json original, const nlohmann::json& typed,
                      std::initializer_list<const char*> managed) {
        if (!original.is_object()) original = nlohmann::json::object();
        for (const auto* key : managed) original.erase(key);
        original.update(typed);
        return original;
    };
    for (auto provider = document["providers"].begin(); provider != document["providers"].end(); ++provider) {
        const auto old = raw.is_object() ? raw.value(provider.key(), nlohmann::json::object())
                                        : nlohmann::json::object();
        auto models = provider->at("models");
        const auto old_models = old.is_object() ? old.value("models", nlohmann::json::array()) : nlohmann::json::array();
        for (auto& model : models) {
            nlohmann::json previous = nlohmann::json::object();
            if (old_models.is_array()) for (const auto& item : old_models) {
                if (item.is_object() && item.value("id", nlohmann::json{}) == model["id"]) { previous = item; break; }
            }
            const auto capabilities = overlay(previous.value("capabilities", nlohmann::json::object()),
                model.value("capabilities", nlohmann::json::object()),
                {"streaming", "tool_calls", "images", "reasoning", "reasoning_efforts",
                 "default_reasoning_effort", "context_window", "context_tokens", "auto_compact_threshold_percent"});
            model = overlay(previous, model, {"id", "display_name", "capabilities"});
            if (!capabilities.empty()) model["capabilities"] = capabilities;
        }
        *provider = overlay(old, *provider, {"protocol", "base_url", "api_key", "keys", "label", "headers", "test_model", "models"});
        (*provider)["models"] = std::move(models);
    }
    return document;
}

struct NativeProviderEditor::Fetch {
    CancellationToken cancellation;
    std::atomic<bool> done{false};
    unsigned revision = 0;
    Result<std::vector<std::string>> result;
};

NativeProviderEditor::NativeProviderEditor(wxWindow* parent, ProviderConfig provider,
                                           const ContextConfig& context, const std::string& theme,
                                           nlohmann::json raw_provider)
    : wxPanel(parent, wxID_ANY),
      provider_(std::move(provider)), raw_provider_(std::move(raw_provider)), context_(context), timer_(this) {
    auto* root = new wxBoxSizer(wxVERTICAL);
    header_panel_ = new wxPanel(this, wxID_ANY);
    header_panel_->SetName("spark.provider.editor.header");
    auto* header_layout = new wxBoxSizer(wxVERTICAL);
    header_row_ = new wxBoxSizer(wxHORIZONTAL);
    header_save_row_ = new wxBoxSizer(wxHORIZONTAL);
    back_button_ = new NativeButton(header_panel_, wxEmptyString, wxART_GO_BACK);
    back_button_->SetName("spark.provider.editor.back");
    back_button_->SetToolTip(T(L"Back to provider list"));
    header_row_->Add(back_button_, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    title_ = new wxStaticText(header_panel_, wxID_ANY,
        T(provider_.label.empty() ? L"Add provider" : L"Edit provider"),
        wxDefaultPosition, wxDefaultSize, wxST_ELLIPSIZE_END);
    title_->SetName("spark.provider.editor.title");
    title_->SetFont(wxFont(wxFontInfo(16).Bold()));
    header_row_->Add(title_, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(12));
    save_button_ = new NativeButton(header_panel_, T(L"Save provider"), wxART_FILE_SAVE);
    save_button_->SetName("spark.provider.editor.save");
    header_row_->Add(save_button_, 0, wxALIGN_CENTER_VERTICAL);
    header_save_row_->AddStretchSpacer(1);
    header_layout->Add(header_row_, 0, wxEXPAND);
    header_panel_->SetSizer(header_layout);
    root->Add(header_panel_, 0, wxEXPAND);
    status_ = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
                              wxST_ELLIPSIZE_END);
    status_->SetMinSize(FromDIP(wxSize(-1, 32)));
    root->Add(status_, 0, wxEXPAND | wxTOP, FromDIP(16));
    body_ = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxDefaultSize,
                                  wxVSCROLL | wxHSCROLL | wxTAB_TRAVERSAL);
    body_->SetScrollRate(FromDIP(12), FromDIP(12));
    content_ = new wxBoxSizer(wxVERTICAL);
    provider_fields_ = new wxFlexGridSizer(0, 2, FromDIP(22), FromDIP(16));
    provider_fields_->AddGrowableCol(0);
    provider_fields_->AddGrowableCol(1);
    auto field_group = [&](const wxString& label, wxWindow* control, const wxString& group_name) {
        auto* group = new wxBoxSizer(wxVERTICAL);
        auto* caption = new wxStaticText(body_, wxID_ANY, label);
        caption->SetName("spark.provider.editor.label." + group_name);
        caption->SetMinSize(FromDIP(wxSize(-1, 24)));
        group->Add(caption, 0, wxBOTTOM, FromDIP(4));
        control->SetName("spark.provider.editor." + group_name);
        control->SetMinSize(FromDIP(wxSize(-1, 20)));
        auto* field_slot = new wxBoxSizer(wxVERTICAL);
        field_slot->AddSpacer(FromDIP(8));
        field_slot->Add(control, 0, wxEXPAND);
        field_slot->AddSpacer(FromDIP(8));
        group->Add(field_slot, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(9));
        return group;
    };
    auto text = [&](const std::string& value, size_t limit = 2048, long style = 0) {
        if (value.size() > limit) oversized_ = true;
        auto* control = new wxTextCtrl(body_, wxID_ANY, U(TruncateUtf8(value, limit)),
                                       wxDefaultPosition, wxDefaultSize, style);
        if ((style & wxTE_MULTILINE) == 0)
            control->SetMaxLength(static_cast<unsigned long>(limit));
        return control;
    };
    name_ = text(provider_.label);
    url_ = text(provider_.base_url, 8192);
    auto* protocol_group = new wxBoxSizer(wxVERTICAL);
    auto* protocol_label = new wxStaticText(body_, wxID_ANY, T(L"Upstream protocol"));
    protocol_label->SetName("spark.provider.editor.label.protocol");
    protocol_label->SetMinSize(FromDIP(wxSize(-1, 24)));
    protocol_group->Add(protocol_label, 0, wxBOTTOM, FromDIP(4));
    protocol_grid_ = new wxGridSizer(0, 3, 0, FromDIP(8));
    const wxString protocol_captions[] = {
        "Chat Completions", "Responses API", "Anthropic Messages"};
    const wxString protocol_names[] = {
        "chat_completions", "responses", "anthropic_messages"};
    for (int index = 0; index < 3; ++index) {
        protocol_buttons_[index] = new NativeButton(body_, protocol_captions[index]);
        protocol_buttons_[index]->SetName(
            "spark.provider.editor.protocol." + protocol_names[index]);
        protocol_buttons_[index]->SetMinSize(FromDIP(wxSize(110, 36)));
        protocol_grid_->Add(protocol_buttons_[index], 1, wxEXPAND);
        protocol_buttons_[index]->Bind(wxEVT_BUTTON, [this, index](wxCommandEvent&) {
            SelectProtocol(index);
            Changed();
        });
    }
    protocol_group->Add(protocol_grid_, 0, wxEXPAND);
    const std::vector<std::string> protocols{"chat_completions", "responses", "anthropic_messages"};
    const auto protocol = std::find(protocols.begin(), protocols.end(), provider_.protocol);
    SelectProtocol(protocol == protocols.end() ? wxNOT_FOUND : static_cast<int>(protocol - protocols.begin()));
    provider_fields_->Add(field_group(T(L"Provider name"), name_, "name"), 1, wxEXPAND);
    provider_fields_->Add(protocol_group, 1, wxEXPAND);
    const auto credential = provider_.api_key_expression.empty() ? provider_.api_key : provider_.api_key_expression;
    key_ = text(credential, 16384, wxTE_PASSWORD);
    visible_key_ = text(credential, 16384);
    visible_key_->Hide();
    auto* key_row = new wxBoxSizer(wxHORIZONTAL);
    reveal_ = new wxCheckBox(body_, wxID_ANY, T(L"Show"));
    reveal_->SetName("spark.provider.editor.reveal-key");
    auto* key_group = new wxBoxSizer(wxVERTICAL);
    auto* key_label = new wxStaticText(body_, wxID_ANY, "API Key");
    key_label->SetName("spark.provider.editor.label.key");
    key_label->SetMinSize(FromDIP(wxSize(-1, 24)));
    key_group->Add(key_label, 0, wxBOTTOM, FromDIP(4));
    key_row->SetMinSize(FromDIP(wxSize(-1, 36)));
    key_->SetMinSize(FromDIP(wxSize(-1, 20)));
    visible_key_->SetMinSize(FromDIP(wxSize(-1, 20)));
    reveal_->SetMinSize(FromDIP(wxSize(36, 36)));
    key_row->Add(key_, 1, wxALIGN_CENTER_VERTICAL);
    key_row->Add(visible_key_, 1, wxALIGN_CENTER_VERTICAL);
    key_row->Add(reveal_, 0, wxLEFT | wxALIGN_CENTER_VERTICAL, FromDIP(8));
    key_group->Add(key_row, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(9));
    key_->SetToolTip("API key or ${ENV:NAME}");
    visible_key_->SetToolTip(key_->GetToolTipText());
    provider_fields_->Add(field_group("Base URL", url_, "url"), 1, wxEXPAND);
    provider_fields_->Add(key_group, 1, wxEXPAND);
    auto add_inset = [this](auto* section, int top, int bottom, int left, int right) {
        auto* row = new wxBoxSizer(wxHORIZONTAL);
        row->AddSpacer(FromDIP(left));
        row->Add(section, 1, wxEXPAND);
        row->AddSpacer(FromDIP(right));
        auto* item = content_->Add(row, 0, wxEXPAND | (top > 0 ? wxTOP : 0), FromDIP(top));
        if (bottom > 0) content_->AddSpacer(FromDIP(bottom));
        return item;
    };
    add_inset(provider_fields_, 8, 0, 2, 20);
    auto* model_label = new wxStaticText(body_, wxID_ANY, T(L"Model list"));
    model_label->SetName("spark.provider.editor.model-list-label");
    model_label->SetMinSize(FromDIP(wxSize(-1, 28)));
    add_inset(model_label, 34, 0, 2, 20);
    model_action_layout_ = new wxWrapSizer(wxHORIZONTAL);
    add_model_button_ = new NativeButton(body_, T(L"Add model"), wxART_PLUS);
    add_model_button_->SetName("spark.provider.editor.add-model");
    clear_models_button_ = new NativeButton(body_, T(L"Clear models"));
    clear_models_button_->SetName("spark.provider.editor.clear-models");
    fetch_button_ = new NativeButton(body_, T(L"Fetch from upstream"));
    fetch_button_->SetName("spark.provider.editor.fetch-models");
    const wxString model_action_labels[] = {
        add_model_button_->GetLabel(), fetch_button_->GetLabel(), clear_models_button_->GetLabel()};
    NativeButton* model_actions[] = {add_model_button_, fetch_button_, clear_models_button_};
    const int model_action_padding[] = {54, 24, 24};
    wxClientDC model_action_dc(body_);
    for (size_t index = 0; index < 3; ++index) {
        model_action_dc.SetFont(model_actions[index]->GetFont());
        const int width = model_action_dc.GetTextExtent(model_action_labels[index]).x +
                          FromDIP(model_action_padding[index]);
        model_actions[index]->SetMinSize(FromDIP(wxSize(width, 36)));
        model_action_layout_->Add(model_actions[index], 0, wxRIGHT, FromDIP(8));
    }
    add_inset(model_action_layout_, 6, 14, 2, 20);
    model_header_ = new wxPanel(body_);
    model_header_->SetName("spark.provider.editor.model.header");
    model_header_layout_ = new wxFlexGridSizer(1, 6, 0, FromDIP(8));
    model_header_layout_->AddGrowableCol(0);
    const wxString model_headers[] = {
        T(L"Model ID"), T(L"Context window"), T(L"Auto compact (%)"),
        T(L"Image handling"), T(L"Reasoning levels"), wxEmptyString};
    for (int index = 0; index < 6; ++index) {
        auto* label = new wxStaticText(model_header_, wxID_ANY, model_headers[index]);
        label->SetName(wxString::Format("spark.provider.editor.model.header.%d", index));
        model_header_layout_->Add(label, 0,
            index == 5 ? static_cast<int>(wxALIGN_CENTER) : static_cast<int>(wxEXPAND));
    }
    model_header_->SetSizer(model_header_layout_);
    model_header_->SetMinSize(FromDIP(wxSize(-1, 24)));
    model_header_item_ = add_inset(model_header_, 0, 0, 2, 20);
    model_rows_ = new wxBoxSizer(wxVERTICAL);
    model_rows_item_ = add_inset(model_rows_, 0, 0, 2, 20);
    test_model_ = text(provider_.test_model);
    test_model_->SetToolTip(T(L"Leave empty to use the first model."));
    add_inset(field_group(T(L"Test model"), test_model_, "test-model"), 12, 0, 2, 20);
    auto* preview_label = new wxStaticText(body_, wxID_ANY, T(L"config.json preview"));
    preview_label->SetName("spark.provider.editor.preview-label");
    preview_label->SetMinSize(FromDIP(wxSize(-1, 28)));
    add_inset(preview_label, 30, 0, 2, 20);
    auto* preview_actions = new wxBoxSizer(wxHORIZONTAL);
    auto* load = new NativeButton(body_, T(L"Load configuration"), wxART_FILE_OPEN);
    load->SetMinSize(FromDIP(wxSize(
        model_action_dc.GetTextExtent(T(L"Load configuration")).x + FromDIP(24), 36)));
    load->SetName("spark.provider.editor.load-configuration");
    load->SetToolTip(T(L"Load configuration"));
    preview_actions->Add(load, 0);
    add_inset(preview_actions, 6, 0, 2, 20);
    preview_ = text("", kMaxPreview, wxTE_MULTILINE);
    preview_->SetName("spark.provider.editor.preview");
    preview_->SetFont(wxFontInfo(10).Family(wxFONTFAMILY_TELETYPE));
    preview_->SetMinSize(FromDIP(wxSize(300, 260)));
    add_inset(preview_, 14, 16, 10, 30);
    body_->SetSizer(content_);
    root->Add(body_, 1, wxEXPAND);
    SetSizer(root);
    LayoutHeader();
    dark_theme_ = theme == "dark" || (theme == "system" && wxSystemSettings::GetAppearance().IsDark());
    Paint(this, dark_theme_);
    Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        LayoutHeader();
        LayoutProviderFields();
        event.Skip();
    });
    body_->Bind(wxEVT_SIZE, [this](wxSizeEvent& event) {
        LayoutModels();
        LayoutProviderFields();
        event.Skip();
    });

    reveal_->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent&) {
        const bool show = reveal_->GetValue();
        (show ? visible_key_ : key_)->ChangeValue((show ? key_ : visible_key_)->GetValue());
        key_->Show(!show);
        visible_key_->Show(show);
        body_->Layout();
    });
    body_->Bind(wxEVT_TEXT, [this](wxCommandEvent& event) {
        if (!syncing_) {
            if (event.GetEventObject() == preview_) {
                json_pending_ = true; form_pending_ = false; debounce_ = 3; ++revision_;
                if (fetch_) fetch_->cancellation.Cancel();
                event.Skip(); return;
            }
            Changed();
        }
        event.Skip();
    });
    body_->Bind(wxEVT_CHILD_FOCUS, [this](wxChildFocusEvent& event) {
        if (!syncing_ && event.GetWindow() != preview_ && json_pending_) ApplyPreview();
        event.Skip();
    });
    add_model_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (model_rows_data_.size() >= kMaxModels) {
            status_->SetLabel(T(L"Too many models. Keep at most 256 models.")); return;
        }
        Changed();
        AddModel({});
        LayoutModels();
        if (!model_rows_data_.empty()) model_rows_data_.back().id->SetFocus();
    });
    load->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { LoadConfiguration(); });
    fetch_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { StartFetch(); });
    clear_models_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        Changed();
        ClearModels();
        LayoutModels();
    });
    save_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Save(); });
    back_button_->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        if (fetch_) fetch_->cancellation.Cancel();
        const auto callback = on_back_;
        if (callback) callback();
    });
    Bind(wxEVT_TIMER, &NativeProviderEditor::Poll, this, timer_.GetId());
    oversized_ = oversized_ || provider_.models.size() > kMaxModels || provider_.headers.size() > 128 ||
                 nlohmann::json(provider_.headers).dump().size() > kMaxHeaders;
    for (const auto& model : provider_.models) {
        oversized_ = oversized_ || model.id.size() > 2048 ||
                     model.capabilities.reasoning_efforts.size() > 32;
        for (const auto& effort : model.capabilities.reasoning_efforts)
            oversized_ = oversized_ || effort.size() > 128;
    }
    if (oversized_) {
        body_->Disable(); save_button_->Disable();
        status_->SetLabel(T(L"Provider exceeds editor limits. Edit the config file."));
    } else {
        for (size_t index = 0; index < std::min(provider_.models.size(), kMaxModels); ++index)
            AddModel(provider_.models[index]);
        if (model_rows_data_.empty()) AddModel({});
        Preview();
    }
    LayoutModels();
    LayoutProviderFields();
    timer_.Start(100);
}

NativeProviderEditor::~NativeProviderEditor() {
    timer_.Stop();
    if (fetch_) fetch_->cancellation.Cancel();
    if (fetch_worker_.joinable()) fetch_worker_.join();
}

void NativeProviderEditor::SetHandlers(std::function<Result<void>(const ProviderConfig&)> save,
                                       std::function<void()> back) {
    on_save_ = std::move(save);
    on_back_ = std::move(back);
}

void NativeProviderEditor::LayoutHeader() {
    if (!header_panel_ || !header_row_ || !header_save_row_ || !save_button_) return;
    const int width = GetClientSize().x;
    const bool compact = width < FromDIP(580);
    wxClientDC save_dc(save_button_);
    save_dc.SetFont(save_button_->GetFont());
    const int save_width = std::max(FromDIP(126),
        save_dc.GetTextExtent(save_button_->GetLabel()).x + FromDIP(60));
    save_button_->SetMinSize(FromDIP(wxSize(save_width, 36)));
    if (compact == compact_header_) {
        header_panel_->SetMinSize(FromDIP(wxSize(-1, compact ? 84 : 36)));
        header_panel_->Layout();
        return;
    }
    compact_header_ = compact;
    if (compact) {
        header_row_->Detach(save_button_);
        header_save_row_->Add(save_button_, 0, wxALIGN_CENTER_VERTICAL);
        if (header_panel_->GetSizer()->GetItemCount() == 1)
            header_panel_->GetSizer()->Add(header_save_row_, 0, wxEXPAND | wxTOP, FromDIP(8));
    } else {
        if (header_panel_->GetSizer()->GetItemCount() > 1)
            header_panel_->GetSizer()->Detach(header_save_row_);
        header_save_row_->Detach(save_button_);
        header_row_->Add(save_button_, 0, wxALIGN_CENTER_VERTICAL);
    }
    header_panel_->SetMinSize(FromDIP(wxSize(-1, compact ? 84 : 36)));
    header_panel_->Layout();
    if (GetSizer()) GetSizer()->Layout();
}

void NativeProviderEditor::LayoutProviderFields() {
    if (!body_ || !provider_fields_ || !protocol_grid_) return;
    const int columns = GetClientSize().x < FromDIP(580) ? 1 : 2;
    if (provider_fields_->GetCols() != columns) {
        provider_fields_->RemoveGrowableCol(0);
        if (provider_fields_->GetCols() == 2) provider_fields_->RemoveGrowableCol(1);
        provider_fields_->SetCols(columns);
        provider_fields_->AddGrowableCol(0);
        if (columns == 2) provider_fields_->AddGrowableCol(1);
    }
    const int horizontal_padding = FromDIP(22);
    const int gap = FromDIP(16);
    const int available = std::max(1, body_->GetClientSize().x - horizontal_padding);
    const int protocol_width = columns == 1 ? available : std::max(1, (available - gap) / 2);
    wxClientDC dc(body_);
    int minimum = FromDIP(110);
    for (const auto* button : protocol_buttons_) {
        dc.SetFont(button->GetFont());
        minimum = std::max(minimum, dc.GetTextExtent(button->GetLabel()).x + FromDIP(36));
    }
    const int protocol_columns = std::clamp(
        (protocol_width + FromDIP(8)) / (minimum + FromDIP(8)), 1, 3);
    if (protocol_grid_->GetCols() != protocol_columns) protocol_grid_->SetCols(protocol_columns);
    body_->Layout();
}

void NativeProviderEditor::SelectProtocol(int index) {
    selected_protocol_ = index >= 0 && index < 3 ? index : wxNOT_FOUND;
    for (int button = 0; button < 3; ++button)
        if (protocol_buttons_[button])
            protocol_buttons_[button]->SetSelected(button == selected_protocol_);
}

void NativeProviderEditor::Changed() {
    if (syncing_) return;
    if (json_pending_ && !ApplyPreview()) return;
    ++revision_;
    if (fetch_) fetch_->cancellation.Cancel();
    form_pending_ = true; debounce_ = 2;
    status_->SetLabel(wxEmptyString);
}

size_t NativeProviderEditor::FindModelRow(wxWindow* panel) const {
    for (size_t index = 0; index < model_rows_data_.size(); ++index)
        if (model_rows_data_[index].panel == panel) return index;
    return model_rows_data_.size();
}

void NativeProviderEditor::AddModel(ModelConfig model) {
    ModelRow row;
    row.model = std::move(model);
    row.original_id = row.model.id;
    if (row.model.display_name.empty()) row.model.display_name = row.model.id;
    row.auto_compact_options = {50, 60, 70, 75, 80, 85, 90, 95, 100};
    if (row.model.capabilities.auto_compact_threshold_percent > 0 &&
        std::find(row.auto_compact_options.begin(), row.auto_compact_options.end(),
                  row.model.capabilities.auto_compact_threshold_percent) == row.auto_compact_options.end()) {
        row.auto_compact_options.push_back(row.model.capabilities.auto_compact_threshold_percent);
        std::sort(row.auto_compact_options.begin(), row.auto_compact_options.end());
    }

    row.panel = new wxPanel(body_);
    row.id = new wxTextCtrl(row.panel, wxID_ANY, U(row.model.id));
    row.context = new wxTextCtrl(row.panel, wxID_ANY,
        row.model.capabilities.context_window > 0
            ? wxString::Format("%d", row.model.capabilities.context_window) : wxString{});
    row.context->SetToolTip(T(L"Use 0 to inherit the default context window."));
    row.context->SetHint(wxString::Format("%d", context_.fallback_context_window));
    row.id->SetToolTip(T(L"Model ID"));
    const wchar_t* captions[] = {L"Model ID", L"Context window", L"Auto compact (%)"};
    for (size_t field = 0; field < 3; ++field) {
        row.labels[field] = new wxStaticText(row.panel, wxID_ANY, T(captions[field]));
        row.labels[field]->Hide();
    }
    row.auto_compact = new NativeChoice(row.panel);
    row.auto_compact->Append(i18n::Format(L"Default ({value}%)",
        {{L"value", std::to_wstring(context_.auto_compact_threshold_percent)}}));
    row.auto_compact->AppendSeparator();
    for (const int percent : row.auto_compact_options)
        row.auto_compact->Append(wxString::Format("%d%%", percent));
    const int compact_index = row.model.capabilities.auto_compact_threshold_percent == 0
        ? 0
        : 1 + static_cast<int>(std::find(row.auto_compact_options.begin(), row.auto_compact_options.end(),
                                          row.model.capabilities.auto_compact_threshold_percent) -
                                row.auto_compact_options.begin());
    row.auto_compact->SetSelection(compact_index);
    row.auto_compact->SetMinSize(FromDIP(wxSize(110, 36)));
    row.auto_compact->SetToolTip(T(L"Auto compact (%)"));
    row.images = new NativeChoice(row.panel);
    row.images->Append(T(L"Send original images"));
    row.images->Append(T(L"Do not send images"));
    row.images->SetSelection(row.model.capabilities.images ? 0 : 1);
    row.images->SetMinSize(FromDIP(wxSize(110, 36)));
    row.images->SetToolTip(T(L"Image handling"));
    row.reasoning = new NativeButton(row.panel, T(L"Reasoning"));
    row.reasoning->SetMinSize(FromDIP(wxSize(150, 36)));
    row.reasoning->SetDisclosure(NativeDisclosure::Collapsed);
    row.reasoning->SetToolTip(T(L"Configure reasoning levels and default"));
    row.remove = new NativeButton(row.panel, {}, wxART_DELETE);
    row.remove->SetMinSize(FromDIP(wxSize(32, 36)));
    row.remove->SetToolTip(T(L"Remove model"));
    row.id->SetMinSize(FromDIP(wxSize(-1, 20)));
    row.context->SetMinSize(FromDIP(wxSize(-1, 20)));
    Paint(row.panel, dark_theme_);

    wxWindow* fields[] = {row.id, row.context, row.auto_compact, row.images, row.reasoning, row.remove};
    for (size_t column = 0; column < 6; ++column)
        row.minimum_widths[column] = fields[column]->GetMinSize().x;
    model_rows_data_.push_back(std::move(row));
    const size_t index = model_rows_data_.size() - 1;
    auto* panel = model_rows_data_[index].panel;
    panel->SetName(wxString::Format("spark.provider.editor.model.%zu", index));
    model_rows_data_[index].id->SetName(wxString::Format("spark.provider.editor.model.%zu.id", index));
    model_rows_data_[index].context->SetName(wxString::Format("spark.provider.editor.model.%zu.context", index));
    model_rows_data_[index].auto_compact->SetName(
        wxString::Format("spark.provider.editor.model.%zu.auto-compact", index));
    model_rows_data_[index].images->SetName(wxString::Format("spark.provider.editor.model.%zu.images", index));
    model_rows_data_[index].reasoning->SetName(
        wxString::Format("spark.provider.editor.model.%zu.reasoning", index));
    model_rows_data_[index].remove->SetName(wxString::Format("spark.provider.editor.model.%zu.remove", index));
    const char* label_names[] = {"id", "context", "auto-compact"};
    for (size_t field = 0; field < 3; ++field)
        model_rows_data_[index].labels[field]->SetName(
            wxString::Format("spark.provider.editor.model.%zu.label.", index) + label_names[field]);
    model_rows_data_[index].sizer_item =
        model_rows_->Add(panel, 0, wxEXPAND | wxBOTTOM, FromDIP(14));

    model_rows_data_[index].id->Bind(wxEVT_TEXT, [this, panel](wxCommandEvent&) {
        const size_t row_index = FindModelRow(panel);
        if (row_index < model_rows_data_.size()) UpdateModelRow(row_index);
        Changed();
    });
    model_rows_data_[index].context->Bind(wxEVT_TEXT, [this](wxCommandEvent&) { Changed(); });
    model_rows_data_[index].auto_compact->Bind(wxEVT_CHOICE, [this, panel](wxCommandEvent&) {
        const size_t row_index = FindModelRow(panel);
        if (row_index < model_rows_data_.size()) { UpdateModelRow(row_index); Changed(); }
    });
    model_rows_data_[index].images->Bind(wxEVT_CHOICE, [this, panel](wxCommandEvent& event) {
        const size_t row_index = FindModelRow(panel);
        if (row_index >= model_rows_data_.size()) return;
        model_rows_data_[row_index].model.capabilities.images = event.GetInt() == 0;
        Changed();
    });
    model_rows_data_[index].reasoning->Bind(wxEVT_BUTTON, [this, panel](wxCommandEvent&) {
        const size_t row_index = FindModelRow(panel);
        if (row_index < model_rows_data_.size()) ShowReasoningMenu(row_index);
    });
    model_rows_data_[index].remove->Bind(wxEVT_BUTTON, [this, panel](wxCommandEvent&) {
        const size_t row_index = FindModelRow(panel);
        if (row_index < model_rows_data_.size()) RemoveModel(row_index);
    });
    UpdateModelRow(index);
}

void NativeProviderEditor::RemoveModel(size_t index) {
    if (index >= model_rows_data_.size()) return;
    auto* panel = model_rows_data_[index].panel;
    model_rows_->Detach(panel);
    panel->Destroy();
    model_rows_data_.erase(model_rows_data_.begin() + static_cast<std::ptrdiff_t>(index));
    for (size_t current = 0; current < model_rows_data_.size(); ++current) {
        auto& row = model_rows_data_[current];
        row.panel->SetName(wxString::Format("spark.provider.editor.model.%zu", current));
        row.id->SetName(wxString::Format("spark.provider.editor.model.%zu.id", current));
        row.context->SetName(wxString::Format("spark.provider.editor.model.%zu.context", current));
        row.auto_compact->SetName(wxString::Format("spark.provider.editor.model.%zu.auto-compact", current));
        row.images->SetName(wxString::Format("spark.provider.editor.model.%zu.images", current));
        row.reasoning->SetName(wxString::Format("spark.provider.editor.model.%zu.reasoning", current));
        row.remove->SetName(wxString::Format("spark.provider.editor.model.%zu.remove", current));
        const char* label_names[] = {"id", "context", "auto-compact"};
        for (size_t field = 0; field < 3; ++field)
            row.labels[field]->SetName(
                wxString::Format("spark.provider.editor.model.%zu.label.", current) + label_names[field]);
    }
    LayoutModels();
    Changed();
}

void NativeProviderEditor::ClearModels() {
    for (auto& row : model_rows_data_)
        if (row.panel) row.panel->Destroy();
    model_rows_->Clear(false);
    model_rows_data_.clear();
}

void NativeProviderEditor::UpdateModelRow(size_t index) {
    if (index >= model_rows_data_.size()) return;
    auto& row = model_rows_data_[index];
    const bool compact = compact_models_;
    for (auto* label : row.labels) label->Show(compact);
    if (!row.layout_initialized || row.compact_layout != compact) {
        // SetSizer deletes the previous sizer; do not delete it again.
        row.panel->SetSizer(nullptr);
        row.layout = nullptr;
        if (compact) {
            auto* vertical = new wxBoxSizer(wxVERTICAL);
            const auto labeled = [this, &row](wxWindow* control, size_t field, bool inset) {
                auto* slot = new wxBoxSizer(wxVERTICAL);
                slot->Add(row.labels[field], 0, wxBOTTOM, FromDIP(4));
                auto* input = new wxBoxSizer(wxHORIZONTAL);
                input->SetMinSize(0, FromDIP(36));
                if (inset) input->AddSpacer(FromDIP(9));
                input->Add(control, 1, wxALIGN_CENTER_VERTICAL);
                if (inset) input->AddSpacer(FromDIP(9));
                slot->Add(input, 0, wxEXPAND);
                return slot;
            };
            auto* top = new wxBoxSizer(wxHORIZONTAL);
            top->Add(labeled(row.id, 0, true), 1, wxEXPAND);
            top->Add(row.remove, 0, wxALIGN_BOTTOM | wxLEFT, FromDIP(10));
            vertical->Add(top, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            auto* second = new wxFlexGridSizer(1, 2, 0, FromDIP(8));
            second->AddGrowableCol(0);
            second->AddGrowableCol(1);
            second->Add(labeled(row.context, 1, true), 1, wxEXPAND);
            second->Add(labeled(row.auto_compact, 2, false), 1, wxEXPAND);
            vertical->Add(second, 0, wxEXPAND | wxBOTTOM, FromDIP(6));
            auto* third = new wxFlexGridSizer(1, 2, 0, FromDIP(8));
            third->AddGrowableCol(0);
            third->AddGrowableCol(1);
            third->Add(row.images, 1, wxEXPAND);
            third->Add(row.reasoning, 1, wxEXPAND);
            vertical->Add(third, 0, wxEXPAND);
            row.panel->SetSizer(vertical);
            row.layout = vertical;
        } else {
            auto* grid = new wxFlexGridSizer(1, 6, 0, FromDIP(8));
            grid->AddGrowableCol(0);
            auto centered_field = [this](wxWindow* control) {
                auto* slot = new wxBoxSizer(wxVERTICAL);
                slot->AddStretchSpacer(1);
                auto* inset = new wxBoxSizer(wxHORIZONTAL);
                inset->AddSpacer(FromDIP(9));
                inset->Add(control, 1, wxALIGN_CENTER_VERTICAL);
                inset->AddSpacer(FromDIP(9));
                slot->Add(inset, 0, wxEXPAND);
                slot->AddStretchSpacer(1);
                return slot;
            };
            grid->Add(centered_field(row.id), 1, wxEXPAND);
            grid->Add(centered_field(row.context), 0, wxEXPAND);
            grid->Add(row.auto_compact, 0, wxEXPAND);
            grid->Add(row.images, 0, wxEXPAND);
            grid->Add(row.reasoning, 0, wxEXPAND);
            grid->Add(row.remove, 0, wxEXPAND);
            row.panel->SetSizer(grid);
            row.layout = grid;
        }
        row.compact_layout = compact;
        row.layout_initialized = true;
    }
    ModelConfig current = row.model;
    current.id = Trim(S(row.id->GetValue()));
    wxString reasoning(ReasoningSummary(current));
    if (compact) reasoning = T(L"Reasoning") + ": " + reasoning;
    row.reasoning->SetLabel(reasoning);
    const wxString image_options[] = {
        T(L"Send original images"), T(L"Do not send images")};
    for (unsigned option = 0; option < 2; ++option) {
        row.images->SetString(option, image_options[option]);
        row.images->SetPopupLabel(option, image_options[option]);
    }
    if (compact && row.images->GetSelection() >= 0) {
        const auto selected = static_cast<unsigned>(row.images->GetSelection());
        row.images->SetString(selected, T(L"Image") + ": " + image_options[selected]);
    }
    row.panel->Layout();
}

void NativeProviderEditor::LayoutModels() {
    if (!body_ || !model_rows_ || !model_header_) return;
    struct Field { wxWindow* window; int minimum_width; };
    std::vector<Field> columns[6];
    for (size_t column = 0; column < 6; ++column)
        columns[column].push_back({model_header_layout_->GetItem(column)->GetWindow(), -1});
    for (auto& row : model_rows_data_) {
        wxWindow* fields[] = {row.id, row.context, row.auto_compact, row.images, row.reasoning, row.remove};
        for (size_t column = 0; column < 6; ++column)
            columns[column].push_back({fields[column], row.minimum_widths[column]});
    }
    int widths[6]{};
    int minimum_width = FromDIP(32) + 5 * FromDIP(8);
    for (size_t column = 0; column < 6; ++column) {
        for (const auto& field : columns[column]) {
            auto* control = field.window;
            auto minimum = control->GetMinSize();
            // Discard widths assigned on the last wide pass before measuring.
            minimum.x = field.minimum_width;
            control->SetMinSize(minimum);
            control->InvalidateBestSize();
            widths[column] = std::max(widths[column], control->GetBestSize().x);
        }
        minimum_width += widths[column];
    }
    compact_models_ = body_->GetClientSize().x < std::max(FromDIP(960), minimum_width);
    if (!compact_models_) {
        const int gap = FromDIP(8);
        const int fixed_widths[] = {
            0,
            FromDIP(100),
            0,
            FromDIP(136),
            FromDIP(184),
            FromDIP(32),
        };
        wxClientDC dc(body_);
        if (!model_rows_data_.empty())
            dc.SetFont(model_rows_data_.front().auto_compact->GetFont());
        const auto default_label = i18n::Format(L"Default ({value}%)",
            {{L"value", std::to_wstring(context_.auto_compact_threshold_percent)}});
        const wxString default_label_text(default_label.c_str());
        const int auto_compact_width = std::max(
            FromDIP(110), dc.GetTextExtent(default_label_text).x + FromDIP(48));
        const int available = std::max(1, body_->GetClientSize().x - FromDIP(22));
        const int fixed_total = fixed_widths[1] + auto_compact_width + fixed_widths[3] +
            fixed_widths[4] + fixed_widths[5] + gap * 5;
        const int wide_widths[] = {
            std::max(FromDIP(120), available - fixed_total),
            fixed_widths[1],
            auto_compact_width,
            fixed_widths[3],
            fixed_widths[4],
            fixed_widths[5],
        };
        for (size_t column = 0; column < 6; ++column) {
            for (size_t row_index = 0; row_index < columns[column].size(); ++row_index) {
                const auto& field = columns[column][row_index];
                auto* control = field.window;
                auto minimum = control->GetMinSize();
                const int field_inset = row_index > 0 && column < 2 ? FromDIP(18) : 0;
                minimum.x = wide_widths[column] - field_inset;
                control->SetMinSize(minimum);
            }
        }
    }
    model_header_->Show(!compact_models_);
    if (model_header_item_) {
        model_header_item_->SetFlag(wxEXPAND);
        model_header_item_->SetBorder(0);
    }
    if (model_rows_item_) {
        model_rows_item_->SetFlag(wxEXPAND | (compact_models_ ? 0 : wxTOP));
        model_rows_item_->SetBorder(FromDIP(compact_models_ ? 0 : 6));
    }
    for (size_t index = 0; index < model_rows_data_.size(); ++index) {
        auto& row = model_rows_data_[index];
        if (row.sizer_item) {
            row.sizer_item->SetFlag(wxEXPAND | wxBOTTOM);
            row.sizer_item->SetBorder(FromDIP(compact_models_ ? 6 : 14));
        }
        UpdateModelRow(index);
    }
    model_rows_->Layout();
    if (content_) content_->Layout();
    body_->FitInside();
    body_->Layout();
}

void NativeProviderEditor::ShowReasoningMenu(size_t index) {
    if (index >= model_rows_data_.size()) return;
    auto& row = model_rows_data_[index];
    ModelConfig current = row.model;
    current.id = Trim(S(row.id->GetValue()));
    const auto selected = current.capabilities.reasoning
        ? EffectiveReasoningEfforts(current) : std::vector<std::string>{};
    ModelConfig built_in = current;
    built_in.capabilities.reasoning = true;
    built_in.capabilities.reasoning_efforts.clear();
    auto available = EffectiveReasoningEfforts(built_in);
    const bool has_built_in = !available.empty();
    if (available.empty()) available = selected;
    const wxPoint screen = row.reasoning->ClientToScreen(wxPoint(0, row.reasoning->GetSize().y));
    NativeReasoningPopupState state;
    state.reasoning = current.capabilities.reasoning;
    state.efforts = std::move(available);
    state.selected = selected;
    state.default_effort = row.model.capabilities.default_reasoning_effort;
    ShowNativeReasoningPopup(row.reasoning, screen, std::move(state),
        [this, index, has_built_in](const NativeReasoningPopupState& next) {
            if (index >= model_rows_data_.size()) return;
            auto& model = model_rows_data_[index].model;
            model.capabilities.reasoning = next.reasoning;
            model.capabilities.reasoning_efforts =
                has_built_in && (next.built_in_defaults || next.selected == next.efforts)
                    ? std::vector<std::string>{} : next.selected;
            model.capabilities.default_reasoning_effort = next.default_effort;
            if (std::find(next.selected.begin(), next.selected.end(),
                          model.capabilities.default_reasoning_effort) ==
                next.selected.end())
                model.capabilities.default_reasoning_effort.clear();
            UpdateModelRow(index);
            Changed();
        });
}

bool NativeProviderEditor::StoreModels(bool validate) {
    std::set<std::string> ids;
    std::vector<ModelConfig> models;
    std::vector<std::pair<std::string, std::string>> id_changes;
    models.reserve(model_rows_data_.size());
    for (auto& row : model_rows_data_) {
        const auto id = Trim(S(row.id->GetValue()));
        const auto context = ContextCount(S(row.context->GetValue()));
        if (!context.ok) {
            if (validate) status_->SetLabel(U(context.error));
            return false;
        }
        if (!validate && id.empty()) continue;
        if (validate && (id.empty() || id.size() > 2048 || !ids.insert(id).second)) {
            status_->SetLabel(T(L"Model IDs must be nonempty and unique."));
            return false;
        }
        if (!validate && !id.empty()) ids.insert(id);
        if (validate && !row.original_id.empty() && row.original_id != id)
            id_changes.emplace_back(row.original_id, id);
        row.model.id = row.model.display_name = id;
        row.model.capabilities.context_window = context.value;
        const int compact_selection = row.auto_compact->GetSelection();
        row.model.capabilities.auto_compact_threshold_percent =
            compact_selection <= 0 ? 0 : row.auto_compact_options[static_cast<size_t>(compact_selection - 1)];
        row.model.capabilities.images = row.images->GetSelection() == 0;
        models.push_back(row.model);
    }
    if (validate && raw_provider_.is_object() &&
        raw_provider_.value("models", nlohmann::json::array()).is_array()) {
        const auto original_models = raw_provider_["models"];
        std::set<size_t> updated_entries;
        for (const auto& [old_id, new_id] : id_changes)
            for (size_t index = 0; index < original_models.size(); ++index)
                if (updated_entries.find(index) == updated_entries.end() &&
                    original_models[index].is_object() &&
                    original_models[index].value("id", nlohmann::json{}) == old_id) {
                    raw_provider_["models"][index]["id"] = new_id;
                    updated_entries.insert(index);
                    break;
                }
        for (auto& row : model_rows_data_)
            row.original_id = Trim(S(row.id->GetValue()));
    }
    provider_.models = std::move(models);
    return true;
}

Result<ProviderConfig> NativeProviderEditor::ReadProvider(bool validate) {
    if (oversized_) return Result<ProviderConfig>::Failure("Provider exceeds editor limits.");
    if (json_pending_ && !ApplyPreview()) return Result<ProviderConfig>::Failure("Fix the provider JSON before continuing.");
    if (!StoreModels(validate)) return Result<ProviderConfig>::Failure("Check the model list.");
    ProviderConfig result = provider_;
    result.label = Trim(S(name_->GetValue()));
    result.base_url = Trim(S(url_->GetValue()));
    result.api_key_expression = Trim(S((reveal_->GetValue() ? visible_key_ : key_)->GetValue()));
    result.api_key.clear();
    result.test_model = Trim(S(test_model_->GetValue()));
    const char* protocols[] = {"chat_completions", "responses", "anthropic_messages"};
    const int protocol = selected_protocol_;
    if (protocol < 0 || protocol > 2) return Result<ProviderConfig>::Failure("Select a supported protocol.");
    result.protocol = protocols[protocol];
    for (auto& model : result.models) model.display_name = model.id;
    return Result<ProviderConfig>::Success(std::move(result));
}

void NativeProviderEditor::Preview() {
    const auto draft = ReadProvider(false);
    if (!draft.ok) { status_->SetLabel(U(draft.error)); return; }
    AppConfig wrapper;
    wrapper.providers = {draft.value};
    wrapper.raw = {{"providers", {{draft.value.id, raw_provider_}}}};
    auto document = nlohmann::json{{"providers", NativeConfigDocument(wrapper)["providers"]}};
    const auto json = document.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    preview_->ChangeValue(json.size() <= kMaxPreview ? U(json) : T(L"Preview exceeds 1 MiB."));
    form_pending_ = false; debounce_ = 0;
}

bool NativeProviderEditor::ApplyPreview() {
    try {
        const auto text = S(preview_->GetValue());
        if (text.size() > kMaxPreview) throw std::invalid_argument("size");
        auto document = nlohmann::json::parse(text, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 32) throw std::invalid_argument("depth");
            return true;
        }, true, true);
        if (document.is_object() && document.contains("providers")) {
            const auto providers = document.at("providers");
            if (!providers.is_object() || providers.empty()) throw std::invalid_argument("providers");
            document = providers.contains(provider_.id) ? providers.at(provider_.id) :
                       providers.size() == 1 ? providers.begin().value() : nlohmann::json{};
        }
        if (!document.is_object() || !document.contains("base_url") || !document.at("base_url").is_string() ||
            !document.contains("models") || !document.at("models").is_array() || document.at("models").size() > kMaxModels)
            throw std::invalid_argument("provider");
        for (const auto* key : {"label", "protocol", "api_key", "test_model"})
            if (document.contains(key) && (!document.at(key).is_string() ||
                document.at(key).get_ref<const std::string&>().size() > (std::string(key) == "api_key" ? 16384u : 2048u)))
                throw std::invalid_argument(key);
        if (document.at("base_url").get_ref<const std::string&>().size() > 8192) throw std::invalid_argument("url");
        if (document.contains("headers")) {
            const auto& headers = document.at("headers");
            if (!headers.is_object() || headers.size() > 128 || headers.dump().size() > kMaxHeaders)
                throw std::invalid_argument("headers");
            for (const auto& value : headers) if (!value.is_string()) throw std::invalid_argument("header");
        }
        std::set<std::string> ids;
        for (const auto& model : document.at("models")) {
            if (!model.is_object() || !model.contains("id") || !model.at("id").is_string())
                throw std::invalid_argument("model");
            const auto id = model.at("id").get<std::string>();
            if (id.empty() || Trim(id) != id || id.size() > 2048 || !ids.insert(id).second)
                throw std::invalid_argument("id");
            if (!model.contains("capabilities")) continue;
            const auto& caps = model.at("capabilities");
            if (!caps.is_object()) throw std::invalid_argument("capabilities");
            for (const auto* key : {"streaming", "tool_calls", "images", "reasoning"})
                if (caps.contains(key) && !caps.at(key).is_boolean()) throw std::invalid_argument(key);
            for (const auto* key : {"context_window", "context_tokens", "auto_compact_threshold_percent"}) {
                if (!caps.contains(key)) continue;
                const auto& count = caps.at(key);
                if (!count.is_number_integer() || count.get<double>() < 0 ||
                    count.get<double>() > (std::string(key) == "auto_compact_threshold_percent" ? 100 : std::numeric_limits<int>::max()))
                    throw std::invalid_argument(key);
            }
            if (caps.contains("reasoning_efforts")) {
                const auto& efforts = caps.at("reasoning_efforts");
                if (!efforts.is_array() || efforts.size() > 32) throw std::invalid_argument("efforts");
                for (const auto& effort : efforts)
                    if (!effort.is_string() || effort.get_ref<const std::string&>().size() > 128)
                        throw std::invalid_argument("effort");
            }
            if (caps.contains("default_reasoning_effort") && (!caps.at("default_reasoning_effort").is_string() ||
                caps.at("default_reasoning_effort").get_ref<const std::string&>().size() > 128))
                throw std::invalid_argument("default");
        }
        auto parsed = ConfigFromJson({{"providers", {{provider_.id, document}}}}, {});
        const std::vector<std::string> protocols{"chat_completions", "responses", "anthropic_messages"};
        const auto protocol = std::find(protocols.begin(), protocols.end(), parsed.providers.front().protocol);
        if (protocol == protocols.end()) throw std::invalid_argument("protocol");
        if (fetch_) fetch_->cancellation.Cancel();
        syncing_ = true;
        provider_ = std::move(parsed.providers.front()); raw_provider_ = std::move(document);
        name_->ChangeValue(U(provider_.label)); url_->ChangeValue(U(provider_.base_url));
        key_->ChangeValue(U(provider_.api_key_expression)); visible_key_->ChangeValue(key_->GetValue());
        test_model_->ChangeValue(U(provider_.test_model));
        SelectProtocol(static_cast<int>(protocol - protocols.begin()));
        ClearModels();
        for (const auto& model : provider_.models) AddModel(model);
        if (model_rows_data_.empty()) AddModel({});
        LayoutModels();
        json_pending_ = form_pending_ = false; debounce_ = 0;
        status_->SetLabel(wxEmptyString);
        syncing_ = false;
        return true;
    } catch (...) {
        syncing_ = false;
        status_->SetLabel(T(L"Invalid provider JSON. Check the selected provider and model list."));
        return false;
    }
}

void NativeProviderEditor::LoadConfiguration() {
    wxFileDialog dialog(this, T(L"Load configuration"), wxEmptyString, wxEmptyString,
                        "JSON (*.json)|*.json", wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dialog.ShowModal() != wxID_OK) return;
    spark::fs_ifstream file(NativePath(S(dialog.GetPath())), std::ios::binary);
    std::string bytes(kMaxPreview + 1, '\0');
    file.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<size_t>(file.gcount()));
    if (file.bad() || bytes.empty() || bytes.size() > kMaxPreview) {
        status_->SetLabel(T(L"Cannot load configuration. Select a JSON file smaller than 1 MB.")); return;
    }
    const auto previous = preview_->GetValue();
    const bool pending = json_pending_;
    preview_->ChangeValue(U(bytes)); json_pending_ = true;
    if (ApplyPreview()) { ++revision_; Preview(); }
    else { preview_->ChangeValue(previous); json_pending_ = pending; }
}

void NativeProviderEditor::StartFetch() {
    if (fetch_) {
        fetch_->cancellation.Cancel();
        status_->SetLabel(T(L"Cancelling..."));
        return;
    }
    static std::weak_ptr<Fetch> inflight;
    if (const auto pending = inflight.lock(); pending && !pending->done.load(std::memory_order_acquire)) {
        status_->SetLabel(T(L"Previous model request is still cancelling.")); return;
    }
    const auto provider = ReadProvider();
    if (!provider.ok) { status_->SetLabel(U(provider.error)); return; }
    if (provider.value.base_url.rfind("https://", 0) != 0 && provider.value.base_url.rfind("http://", 0) != 0) {
        status_->SetLabel(T(L"Enter a name and an HTTP or HTTPS Base URL.")); return;
    }
    auto state = std::make_shared<Fetch>();
    state->revision = revision_;
    try {
        // Workers own snapshots only. wx controls are read and updated by the timer.
        fetch_worker_ = std::thread([state, snapshot = provider.value] {
            try { state->result = FetchProviderModelIds(snapshot, &state->cancellation); }
            catch (...) { state->result = Result<std::vector<std::string>>::Failure("Model request failed."); }
            state->done.store(true, std::memory_order_release);
        });
    } catch (...) {
        status_->SetLabel(T(L"Could not start model request.")); return;
    }
    inflight = state;
    fetch_ = std::move(state);
    fetch_button_->SetLabel(T(L"Cancel"));
    status_->SetLabel(T(L"Fetching models..."));
    timer_.Start(100);
}

void NativeProviderEditor::Poll(wxTimerEvent&) {
    if (debounce_ > 0 && --debounce_ == 0) {
        if (json_pending_) ApplyPreview();
        else if (form_pending_) Preview();
    }
    if (!fetch_ || !fetch_->done.load(std::memory_order_acquire)) return;
    if (fetch_worker_.joinable()) fetch_worker_.join();
    const auto state = std::move(fetch_);
    fetch_button_->SetLabel(T(L"Fetch from upstream"));
    if (state->cancellation.IsCancelled() || state->revision != revision_) {
        status_->SetLabel(T(L"Model request cancelled.")); return;
    }
    if (!state->result.ok) { status_->SetLabel(U(state->result.error)); return; }
    const auto current = ReadProvider();
    if (!current.ok) { status_->SetLabel(U(current.error)); return; }
    std::set<std::string> ids;
    for (const auto& model : current.value.models) ids.insert(model.id);
    std::vector<std::string> added;
    for (const auto& id : state->result.value)
        if (ids.insert(id).second) added.push_back(id);
    if (current.value.models.size() + added.size() > kMaxModels) {
        status_->SetLabel(T(L"Too many models. Keep at most 256 models.")); return;
    }
    provider_ = current.value;
    for (const auto& id : added) {
        ModelConfig model;
        model.id = model.display_name = id;
        provider_.models.push_back(std::move(model));
    }
    syncing_ = true;
    ClearModels();
    for (const auto& model : provider_.models) AddModel(model);
    if (model_rows_data_.empty()) AddModel({});
    syncing_ = false;
    LayoutModels();
    Preview();
    status_->SetLabel(wxString::Format("%zu models added.", added.size()));
}

void NativeProviderEditor::Save() {
    const auto result = ReadProvider();
    if (!result.ok) { status_->SetLabel(U(result.error)); return; }
    if (result.value.label.empty() || result.value.models.empty() || result.value.models.size() > kMaxModels) {
        status_->SetLabel(T(L"Enter a name and keep between 1 and 256 models.")); return;
    }
    if (result.value.base_url.rfind("https://", 0) != 0 && result.value.base_url.rfind("http://", 0) != 0) {
        status_->SetLabel(T(L"Enter a name and an HTTP or HTTPS Base URL.")); return;
    }
    std::set<std::string> ids;
    for (const auto& model : result.value.models) {
        if (model.id.empty() || model.id.size() > 2048 || !ids.insert(model.id).second ||
            model.capabilities.context_window < 0 || model.capabilities.auto_compact_threshold_percent < 0 ||
            model.capabilities.auto_compact_threshold_percent > 100) {
            status_->SetLabel(T(L"Check model IDs, context windows, and compaction percentages.")); return;
        }
    }
    provider_ = result.value;
    if (provider_.test_model.empty()) provider_.test_model = provider_.models.front().id;
    AppConfig wrapper; wrapper.providers = {provider_};
    wrapper.raw = {{"providers", {{provider_.id, raw_provider_}}}};
    raw_provider_ = NativeConfigDocument(wrapper)["providers"][provider_.id];
    if (raw_provider_.dump().size() > kMaxPreview) { status_->SetLabel(T(L"Preview exceeds 1 MiB.")); return; }
    if (!on_save_) { status_->SetLabel(T(L"Provider save is unavailable.")); return; }
    const auto saved = on_save_(provider_);
    if (!saved.ok) {
        status_->SetLabel(wxString(i18n::Format(L"Save failed: {error}",
            {{L"error", Utf8ToWide(saved.error)}})));
        return;
    }
    const auto callback = on_back_;
    if (callback) callback();
}

}  // namespace spark
