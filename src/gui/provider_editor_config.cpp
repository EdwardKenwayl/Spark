#include "gui/provider_editor.h"
#include "gui/localization.h"
#include "core/provider_models.h"
#include "platform/window_text.h"

#include <commctrl.h>
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <set>
#include <thread>

namespace spark {
namespace {
const wchar_t* Text(std::wstring_view key) { return i18n::Text(key); }
constexpr size_t kMaxConfigBytes = 1024 * 1024;

std::wstring EditJson(const nlohmann::json& json) {
    std::wstring result;
    for (wchar_t ch : Utf8ToWide(json.dump(2))) {
        if (ch == L'\n') result += L'\r';
        result += ch;
    }
    return result;
}
}  // namespace

Result<ProviderConfig> ProviderEditor::ReadDraft(bool validate) const {
    ProviderConfig result = draft_;
    result.label = Trim(WideToUtf8(ReadWindowText(name_)));
    result.base_url = Trim(WideToUtf8(ReadWindowText(url_)));
    result.api_key_expression = Trim(WideToUtf8(ReadWindowText(key_)));
    result.api_key.clear();
    result.test_model = Trim(WideToUtf8(ReadWindowText(test_model_)));
    result.models.clear();
    auto error = [](const wchar_t* text) { return Result<ProviderConfig>::Failure(WideToUtf8(text)); };
    if (validate && (result.label.empty() ||
        (result.base_url.rfind("https://", 0) != 0 && result.base_url.rfind("http://", 0) != 0)))
        return error(Text(L"Enter a name and an HTTP or HTTPS Base URL."));
    std::set<std::string> ids;
    for (const auto& row : rows_) {
        ModelConfig model = row.model;
        model.id = Trim(WideToUtf8(ReadWindowText(row.id)));
        if (!validate && model.id.empty()) continue;
        if (model.id.empty() || !ids.insert(model.id).second)
            return error(Text(L"Model IDs must be nonempty and unique."));
        if (model.display_name.empty() || model.display_name == row.model.id) model.display_name = model.id;
        std::string context = LowerAscii(Trim(WideToUtf8(ReadWindowText(row.context))));
        double multiplier = 1;
        if (!context.empty() && (context.back() == 'k' || context.back() == 'm')) {
            multiplier = context.back() == 'k' ? 1000 : 1000000; context.pop_back();
            if (context.empty()) return error(Text(L"Invalid context window. Use a token count, 200K, or 1M."));
        }
        try {
            size_t end = 0;
            const double value = context.empty() ? 0 : std::stod(context, &end) * multiplier;
            if (end != context.size() || !std::isfinite(value) || value < 0 || value > INT_MAX || value != std::floor(value))
                throw std::invalid_argument("context");
            model.capabilities.context_window = static_cast<int>(value);
        } catch (...) { return error(Text(L"Invalid context window. Use a token count, 200K, or 1M.")); }
        model.capabilities.auto_compact_threshold_percent = row.auto_compact_percent;
        result.models.push_back(std::move(model));
    }
    if (validate && result.models.empty()) return error(Text(L"Add at least one model."));
    if (validate && result.test_model.empty()) result.test_model = result.models.front().id;
    return Result<ProviderConfig>::Success(std::move(result));
}

void ProviderEditor::RemoveModels() {
    CloseReasoningPopup();
    const bool was_syncing = syncing_; syncing_ = true;
    for (const auto& row : rows_) {
        for (HWND control : {row.remove, row.reasoning, row.auto_compact, row.context, row.id}) {
            TOOLINFOW tool{sizeof(tool)}; tool.hwnd = body_; tool.uId = reinterpret_cast<UINT_PTR>(control);
            SendMessageW(tooltip_, TTM_DELTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        }
        for (HWND control : {row.id, row.context, row.auto_compact, row.images, row.reasoning, row.remove})
            DestroyWindow(control);
    }
    rows_.clear(); syncing_ = was_syncing;
}

void ProviderEditor::DraftChanged(bool json, HWND control) {
    if (syncing_ || !window_ || !preview_) return;
    if (!json && json_pending_) {
        const auto text = ReadWindowText(control);
        const int id = GetDlgCtrlID(control);
        std::string model_id;
        int row_field = 0;
        for (const auto& row : rows_) {
            if (row.id == control || row.context == control) {
                model_id = row.model.id;
                row_field = row.context == control ? 1 : 0;
                break;
            }
        }
        const bool focused = GetFocus() == control;
        DWORD start = 0, end = 0;
        SendMessageW(control, EM_GETSEL, reinterpret_cast<WPARAM>(&start), reinterpret_cast<LPARAM>(&end));
        if (!ApplyPreview()) return;
        HWND target = id == Name ? name_ : id == Url ? url_ : id == Key ? key_ : id == TestModel ? test_model_ : nullptr;
        for (const auto& row : rows_) if (!model_id.empty() && row.model.id == model_id) {
            target = row_field == 1 ? row.context : row.id;
            break;
        }
        syncing_ = true;
        if (target) {
            SetWindowTextW(target, text.c_str());
            if (focused) {
                SetFocus(target);
                SendMessageW(target, EM_SETSEL, start, end);
            }
        }
        syncing_ = false;
    }
    status_ok_ = false;
    CancelModelFetch();
    KillTimer(window_, 1); KillTimer(window_, 2);
    SetWindowTextW(status_, L"");
    json_pending_ = json;
    if (!json) json_valid_ = true;
    SetTimer(window_, json ? 2 : 1, json ? 350 : 180, nullptr);
}

void ProviderEditor::RefreshPreview() {
    KillTimer(window_, 1); KillTimer(window_, 2);
    if (!preview_) return;
    const auto provider = ReadDraft(false);
    if (!provider.ok) { SetWindowTextW(status_, Utf8ToWide(provider.error).c_str()); return; }
    AppConfig config; config.providers = {provider.value};
    auto json = nlohmann::json{{"providers", ConfigToJson(config)["providers"]}};
    json["providers"][provider.value.id]["label"] = provider.value.label;
    syncing_ = true;
    SetWindowTextW(preview_, EditJson(json).c_str());
    syncing_ = false; json_pending_ = false; json_valid_ = true;
}

bool ProviderEditor::ApplyPreview() {
    status_ok_ = false;
    KillTimer(window_, 2);
    json_pending_ = false;
    try {
        const auto text = WideToUtf8(ReadWindowText(preview_));
        if (text.size() > kMaxConfigBytes) throw std::invalid_argument("size");
        auto json = nlohmann::json::parse(text, [](int depth, nlohmann::json::parse_event_t, nlohmann::json&) {
            if (depth > 32) throw std::invalid_argument("depth");
            return true;
        }, true, true);
        if (!json.is_object()) throw std::invalid_argument("object");
        if (json.contains("providers")) {
            const auto& providers = json.at("providers");
            if (!providers.is_object() || providers.empty()) throw std::invalid_argument("providers");
            json = providers.contains(draft_.id) ? nlohmann::json(providers.at(draft_.id))
                : providers.size() == 1 ? nlohmann::json(providers.begin().value()) : nlohmann::json();
        }
        if (!json.is_object() || !json.contains("base_url") || !json.at("base_url").is_string() ||
            !json.contains("models") || !json.at("models").is_array() || json.at("models").size() > 256)
            throw std::invalid_argument("provider");
        for (const char* key : {"label", "protocol", "api_key", "test_model"})
            if (json.contains(key) && !json.at(key).is_string()) throw std::invalid_argument(key);
        if (json.contains("headers")) {
            if (!json.at("headers").is_object()) throw std::invalid_argument("headers");
            for (const auto& value : json.at("headers")) if (!value.is_string()) throw std::invalid_argument("header");
        }
        std::set<std::string> ids;
        for (const auto& model : json.at("models")) {
            if (!model.is_object() || !model.contains("id") || !model.at("id").is_string())
                throw std::invalid_argument("model");
            const auto id = model.at("id").get<std::string>();
            if (id.empty() || id != Trim(id) || id.size() > 2048 || !ids.insert(id).second)
                throw std::invalid_argument("id");
            if (model.contains("capabilities") && !model.at("capabilities").is_object())
                throw std::invalid_argument("capabilities");
            if (model.contains("capabilities")) {
                const auto& capabilities = model.at("capabilities");
                for (const char* key : {"streaming", "tool_calls", "images", "reasoning"})
                    if (capabilities.contains(key) && !capabilities.at(key).is_boolean())
                        throw std::invalid_argument(key);
                if (capabilities.contains("context_window")) {
                    const auto& count = capabilities.at("context_window");
                    if (!count.is_number_integer() || count.get<double>() < 0 || count.get<double>() > INT_MAX)
                        throw std::invalid_argument("context_window");
                }
                if (capabilities.contains("auto_compact_threshold_percent")) {
                    const auto& percent = capabilities.at("auto_compact_threshold_percent");
                    if (!percent.is_number_integer() || percent.get<int>() < 0 || percent.get<int>() > 100)
                        throw std::invalid_argument("auto_compact_threshold_percent");
                }
                if (capabilities.contains("reasoning_efforts")) {
                    if (!capabilities.at("reasoning_efforts").is_array()) throw std::invalid_argument("efforts");
                    for (const auto& effort : capabilities.at("reasoning_efforts"))
                        if (!effort.is_string()) throw std::invalid_argument("effort");
                }
                if (capabilities.contains("default_reasoning_effort") &&
                    !capabilities.at("default_reasoning_effort").is_string())
                    throw std::invalid_argument("default_reasoning_effort");
            }
        }
        auto config = ConfigFromJson({{"providers", {{draft_.id, json}}}}, {});
        if (config.providers.size() != 1) throw std::invalid_argument("provider");
        CancelModelFetch();
        syncing_ = true;
        RemoveModels();
        draft_ = std::move(config.providers.front());
        SetWindowTextW(name_, Utf8ToWide(draft_.label).c_str());
        SetWindowTextW(url_, Utf8ToWide(draft_.base_url).c_str());
        SetWindowTextW(key_, Utf8ToWide(draft_.api_key_expression).c_str());
        SetWindowTextW(test_model_, Utf8ToWide(draft_.test_model).c_str());
        for (const auto& model : draft_.models) AddModel(model);
        syncing_ = false; json_valid_ = true;
        SetWindowTextW(status_, L"");
        Layout();
        for (HWND protocol : {responses_, chat_, anthropic_}) InvalidateRect(protocol, nullptr, FALSE);
        return true;
    } catch (...) {
        syncing_ = false; json_valid_ = false;
        SetWindowTextW(status_, Text(L"Invalid provider JSON. Check the selected provider and model list."));
        return false;
    }
}

void ProviderEditor::LoadConfiguration(const std::wstring& path) {
    status_ok_ = false;
    std::wstring selected = path;
    if (selected.empty()) {
        wchar_t file[32768]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = window_; dialog.lpstrFile = file; dialog.nMaxFile = static_cast<DWORD>(std::size(file));
        dialog.lpstrFilter = L"JSON (*.json)\0*.json\0\0";
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return;
        selected = file;
    }
    std::error_code error;
    const auto size = spark::fs::file_size(selected, error);
    const auto contents = !error && size <= kMaxConfigBytes ? ReadFileUtf8(WideToUtf8(selected)) : std::nullopt;
    if (!contents) {
        SetWindowTextW(status_, Text(L"Cannot load configuration. Select a JSON file smaller than 1 MB.")); return;
    }
    const auto previous = ReadWindowText(preview_);
    const bool valid = json_valid_, pending = json_pending_;
    syncing_ = true; SetWindowTextW(preview_, Utf8ToWide(*contents).c_str()); syncing_ = false;
    if (ApplyPreview()) RefreshPreview();
    else {
        syncing_ = true; SetWindowTextW(preview_, previous.c_str()); syncing_ = false;
        json_valid_ = valid; json_pending_ = pending;
        if (pending) SetTimer(window_, 2, 350, nullptr);
    }
}

struct ProviderEditor::ModelFetch {
    CancellationToken cancellation;
    std::atomic<bool> done{false};
    Result<std::vector<std::string>> result;
};

void ProviderEditor::CancelModelFetch() {
    if (!fetch_) return;
    fetch_->cancellation.Cancel(); fetch_.reset();
    if (window_) KillTimer(window_, 3);
    if (fetch_models_ && IsWindow(fetch_models_)) {
        EnableWindow(fetch_models_, TRUE); SetWindowTextW(fetch_models_, Text(L"Fetch from upstream"));
    }
}

void ProviderEditor::StartModelFetch() {
    status_ok_ = false;
    if (fetch_ || (json_pending_ && !ApplyPreview())) return;
    if (!json_valid_) return;
    const auto provider = ReadDraft(false);
    if (!provider.ok) { SetWindowTextW(status_, Utf8ToWide(provider.error).c_str()); return; }
    if (provider.value.base_url.rfind("https://", 0) != 0 && provider.value.base_url.rfind("http://", 0) != 0) {
        SetWindowTextW(status_, Text(L"Enter a name and an HTTP or HTTPS Base URL.")); return;
    }
    fetch_ = std::make_shared<ModelFetch>();
    try {
        std::thread([state = fetch_, provider = provider.value] {
            try { state->result = FetchProviderModelIds(provider, &state->cancellation); }
            catch (...) { state->result = Result<std::vector<std::string>>::Failure("Model request failed."); }
            state->done.store(true, std::memory_order_release);
        }).detach();
    } catch (...) {
        fetch_.reset();
        SetWindowTextW(status_, i18n::Format(L"Connection failed: {error}",
            {{L"error", Utf8ToWide(FormatWin32Error(ERROR_NOT_ENOUGH_MEMORY))}}).c_str()); return;
    }
    EnableWindow(fetch_models_, FALSE);
    SetWindowTextW(fetch_models_, Text(L"Fetching models...")); Layout();
    SetTimer(window_, 3, 50, nullptr);
}

void ProviderEditor::PollModelFetch() {
    if (!fetch_ || !fetch_->done.load(std::memory_order_acquire)) return;
    const auto result = fetch_->result;
    CancelModelFetch();
    if (!result.ok) {
        SetWindowTextW(status_, i18n::Format(L"Connection failed: {error}", {{L"error", Utf8ToWide(result.error)}}).c_str());
        return;
    }
    const auto provider = ReadDraft(false);
    if (!provider.ok) return;
    std::set<std::string> ids;
    auto models = provider.value.models;
    for (const auto& model : models) ids.insert(model.id);
    for (const auto& id : result.value) if (ids.insert(id).second) {
        if (models.size() >= 256) {
            SetWindowTextW(status_, Text(L"Too many models. Keep at most 256 models.")); return;
        }
        ModelConfig model; model.id = model.display_name = id; models.push_back(std::move(model));
    }
    syncing_ = true; RemoveModels();
    for (auto& model : models) AddModel(std::move(model));
    syncing_ = false; RefreshPreview(); Layout();
    status_ok_ = true;
    SetWindowTextW(status_, i18n::Format(L"Fetched {count} models.", {{L"count", std::to_wstring(result.value.size())}}).c_str());
}
}  // namespace spark
