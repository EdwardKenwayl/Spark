#include "gui/settings_window.h"
#include "gui/localization.h"
#include "core/provider_probe.h"
#include "core/session.h"

#include <commctrl.h>
#include <algorithm>
#include <atomic>
#include <thread>

namespace spark {
namespace {
constexpr UINT_PTR kProbeTimer = 2;
}

Result<void> SettingsWindow::SaveProviders(AppConfig updated) {
    updated = ConfigFromJson(ConfigToJson(updated), updated.config_path);
    const auto errors = ValidateConfig(updated, false);
    if (!errors.empty()) return Result<void>::Failure(errors.front());
    const auto saved = SaveConfigAtomically(updated);
    if (!saved.ok) return saved;
    CancelProviderTest();
    config_ = std::move(updated);
    RefreshProviders();
    RefreshModelStatus();
    Layout();
    if (on_saved_) on_saved_(config_);
    return Result<void>::Success();
}

Result<void> SettingsWindow::DuplicateProvider(size_t index) {
    if (index >= config_.providers.size()) return Result<void>::Failure("Provider no longer exists.");
    AppConfig updated = config_;
    ProviderConfig copy = updated.providers[index];
    copy.id = "provider-" + NewSessionId();
    const auto name = Utf8ToWide(copy.label.empty() ? config_.providers[index].id : copy.label);
    const auto base = i18n::Format(L"{name} (copy)", {{L"name", name}});
    copy.label = WideToUtf8(base);
    for (int suffix = 2; std::any_of(updated.providers.begin(), updated.providers.end(),
             [&](const ProviderConfig& entry) { return entry.label == copy.label; }); ++suffix)
        copy.label = WideToUtf8(base + L" " + std::to_wstring(suffix));
    updated.providers.insert(updated.providers.begin() + index + 1, std::move(copy));
    return SaveProviders(std::move(updated));
}

Result<void> SettingsWindow::DeleteProvider(size_t index) {
    if (index >= config_.providers.size()) return Result<void>::Failure("Provider no longer exists.");
    if (config_.providers.size() == 1)
        return Result<void>::Failure(WideToUtf8(Text(L"Keep at least one provider. Add another provider before deleting this one.")));
    AppConfig updated = config_;
    const bool active = updated.active.provider == updated.providers[index].id;
    updated.providers.erase(updated.providers.begin() + index);
    if (active) {
        const auto& next = updated.providers.front();
        updated.active = {next.id, next.models.empty() ? "" : next.models.front().id, ""};
    }
    return SaveProviders(std::move(updated));
}

Result<void> SettingsWindow::MoveProvider(const std::string& id, size_t destination) {
    AppConfig updated = config_;
    const auto found = std::find_if(updated.providers.begin(), updated.providers.end(),
        [&](const ProviderConfig& entry) { return entry.id == id; });
    if (found == updated.providers.end() || destination >= updated.providers.size())
        return Result<void>::Failure("Provider no longer exists.");
    if (static_cast<size_t>(found - updated.providers.begin()) == destination) return Result<void>::Success();
    ProviderConfig provider = std::move(*found);
    updated.providers.erase(found);
    updated.providers.insert(updated.providers.begin() + destination, std::move(provider));
    return SaveProviders(std::move(updated));
}

void SettingsWindow::ActivateProviderAction(size_t index, int action) {
    if (index >= config_.providers.size()) return;
    if (action == 1) { StartProviderTest(index); return; }
    if (action == 2) { EditProvider(index); return; }
    if (action != 3 && action != 4) return;
    const auto id = config_.providers[index].id;
    if (action == 4) {
        const auto& provider = config_.providers[index];
        const auto message = i18n::Format(L"Delete provider \"{name}\"? Existing chats will be kept.",
            {{L"name", Utf8ToWide(provider.label.empty() ? id : provider.label)}});
        if (MessageBoxW(window_, message.c_str(), Text(L"Delete provider"),
                        MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) return;
        // A modal dialog pumps messages; locate the provider again before mutating.
        const auto found = std::find_if(config_.providers.begin(), config_.providers.end(),
            [&](const ProviderConfig& entry) { return entry.id == id; });
        if (found == config_.providers.end()) return;
        index = static_cast<size_t>(found - config_.providers.begin());
    }
    const auto result = action == 3 ? DuplicateProvider(index) : DeleteProvider(index);
    SetStatus(result.ok ? Text(L"Settings saved and applied.")
        : i18n::Format(L"Save failed: {error}", {{L"error", Utf8ToWide(result.error)}}));
}

void SettingsWindow::CancelProviderDrag() {
    const bool had_drag = provider_dragging_;
    provider_pressed_id_.clear();
    provider_pressed_action_ = -1;
    provider_dragging_ = false;
    provider_drop_ = -1;
    if (provider_list_) {
        KillTimer(provider_list_, 1);
        if (GetCapture() == provider_list_) ReleaseCapture();
        if (had_drag) InvalidateRect(provider_list_, nullptr, FALSE);
    }
}

void SettingsWindow::UpdateProviderDrag(POINT point) {
    if (provider_pressed_action_ != 0 || provider_pressed_id_.empty()) return;
    if (!provider_dragging_ &&
        std::abs(point.x - provider_pressed_point_.x) < GetSystemMetrics(SM_CXDRAG) &&
        std::abs(point.y - provider_pressed_point_.y) < GetSystemMetrics(SM_CYDRAG)) return;
    provider_dragging_ = true;
    RECT client{}; GetClientRect(provider_list_, &client);
    int drop = -1;
    if (point.x >= client.left && point.x < client.right && point.y >= client.top && point.y < client.bottom) {
        drop = static_cast<int>(config_.providers.size());
        for (int row = 0; row < static_cast<int>(config_.providers.size()); ++row) {
            RECT rect{}; SendMessageW(provider_list_, LB_GETITEMRECT, row, reinterpret_cast<LPARAM>(&rect));
            if (point.y < (rect.top + rect.bottom) / 2) { drop = row; break; }
        }
    }
    if (drop != provider_drop_) {
        provider_drop_ = drop;
        InvalidateRect(provider_list_, nullptr, FALSE);
    }
    SendMessageW(provider_tooltip_, TTM_POP, 0, 0);
    SetCursor(LoadCursorW(nullptr, drop < 0 ? IDC_NO : IDC_SIZEALL));
}

struct SettingsWindow::ProviderTest {
    CancellationToken cancellation;
    std::atomic<bool> done{false};
    std::string provider_id;
    std::wstring name;
    std::wstring model;
    Result<ProviderProbeResult> result;
};

void SettingsWindow::CancelProviderTest() {
    if (provider_test_) {
        provider_test_->cancellation.Cancel();
        if (provider_status_ && IsWindow(provider_status_)) SetWindowTextW(provider_status_, L"");
    }
    provider_test_.reset();
    if (provider_list_) KillTimer(provider_list_, kProbeTimer);
}

void SettingsWindow::StartProviderTest(size_t index) {
    if (index >= config_.providers.size()) return;
    if (provider_test_ && provider_test_->provider_id == config_.providers[index].id) return;
    CancelProviderTest();
    const auto provider = config_.providers[index];
    auto state = std::make_shared<ProviderTest>();
    state->provider_id = provider.id;
    state->name = Utf8ToWide(provider.label.empty() ? provider.id : provider.label);
    state->model = Utf8ToWide(TruncateUtf8(SanitizeDisplayText(
        RedactSecrets(ProviderProbeModelId(provider), CollectConfiguredSecrets(config_))), 512));
    try {
        std::thread([state, provider] {
            try { state->result = ProbeProvider(provider, &state->cancellation); }
            catch (...) { state->result = Result<ProviderProbeResult>::Failure("Provider test failed."); }
            state->done.store(true, std::memory_order_release);
        }).detach();
    } catch (...) {
        SetStatus(Text(L"Could not start the provider test."));
        return;
    }
    provider_test_ = std::move(state);
    SetStatus(i18n::Format(L"Sending hi to {name} using {model}...",
        {{L"name", provider_test_->name}, {L"model", provider_test_->model}}));
    SetTimer(provider_list_, kProbeTimer, 50, nullptr);
}

void SettingsWindow::PollProviderTest() {
    if (!provider_test_ || !provider_test_->done.load(std::memory_order_acquire)) return;
    const auto state = provider_test_;
    CancelProviderTest();
    const auto& result = state->result;
    const std::wstring status = result.value.http_status > 0 ? L"HTTP " + std::to_wstring(result.value.http_status)
                                                            : Text(L"No HTTP response");
    const std::wstring reply = result.ok ? Utf8ToWide(result.value.text)
        : i18n::Format(L"hi test failed: {error}", {{L"error", Utf8ToWide(result.error)}});
    SetStatus(i18n::Format(L"{name} / {model}: {status}, {time} ms\n{reply}",
        {{L"name", state->name},
         {L"model", result.value.model_id.empty() ? state->model : Utf8ToWide(result.value.model_id)},
         {L"status", status}, {L"time", std::to_wstring(result.value.elapsed_ms)}, {L"reply", reply}}));
}
}  // namespace spark
