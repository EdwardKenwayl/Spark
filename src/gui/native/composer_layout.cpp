#include "gui/native/app.h"
#include "gui/localization.h"
#include "gui/native/popup_menu.h"
#include "gui/native/model_picker.h"

#include <wx/menu.h>
#include <wx/weakref.h>

#include <algorithm>

namespace spark {
namespace {
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
wxString Effort(const std::string& value) {
    if (value.empty()) return T(L"Default");
    if (value == "none") return T(L"None");
    if (value == "minimal") return T(L"Minimal");
    if (value == "low") return T(L"Low");
    if (value == "medium") return T(L"Medium");
    if (value == "high") return T(L"High");
    if (value == "xhigh") return T(L"Extra high");
    if (value == "ultra") return T(L"Ultra");
    if (value == "max") return T(L"Maximum");
    return wxString::FromUTF8(value);
}
}

void NativeFrame::LayoutComposer() {
    if (!send_ || !attachment_panel_) return;
    const int width = std::min(FromDIP(900), std::max(1, chat_page_->GetClientSize().x - FromDIP(40)));
    const int image = std::clamp(GetClientSize().y / 4, FromDIP(80), FromDIP(160));
    const int attachment_height = attachments_.empty() ? 0 : image + FromDIP(16);
    const int height = FromDIP(138) + attachment_height;
    const wxSize minimum(width, height);
    if (input_panel_->GetMinSize() != minimum) {
        input_panel_->SetMinSize(minimum);
        composer_host_->SetMinSize(wxSize(-1, height));
        chat_page_->Layout();
    }
    composer_host_->Layout();
    const auto place = [&](wxWindow* child, int x, int y, int w, int h) {
        const wxRect bounds(x, y, std::max(1, w), std::max(1, h));
        if (child->GetRect() != bounds) child->SetSize(bounds);
    };
    const int row = height - FromDIP(40), input_top = FromDIP(10) + attachment_height;
    place(attachment_panel_, FromDIP(12), FromDIP(8), width - FromDIP(24), image);
    attachment_panel_->Show(!attachments_.empty());
    place(composer_, FromDIP(12), input_top, width - FromDIP(24),
          std::max(FromDIP(60), row - input_top - FromDIP(16)));
    place(composer_hint_, FromDIP(12), input_top, width - FromDIP(28),
          std::max(FromDIP(20), composer_->GetCharHeight()));
    composer_hint_->Raise();
    place(attach_, FromDIP(8), row + FromDIP(2), FromDIP(30), FromDIP(30));
    const int permission_width = std::min(std::max(FromDIP(120),
        permissions_->GetTextExtent(permissions_->GetStringSelection()).x + FromDIP(46)),
        std::max(FromDIP(64), width * 40 / 100));
    place(permissions_, FromDIP(44), row + FromDIP(4), permission_width, FromDIP(26));
    place(send_, width - FromDIP(42), row, FromDIP(32), FromDIP(32));
    const int model_left = std::max(FromDIP(44) + permission_width + FromDIP(8), width - FromDIP(258));
    place(model_, model_left, row + FromDIP(3), width - FromDIP(46) - model_left, FromDIP(26));
}

void NativeFrame::RefreshModels() {
    const auto* model = SelectedModel();
    const wxString name = model ? wxString::FromUTF8(model->id) : T(L"No model configured");
    const std::string effort = model && config_.active.reasoning_effort.empty()
        ? EffectiveDefaultReasoningEffort(*model) : config_.active.reasoning_effort;
    model_->SetLabel(name);
    model_->SetBadge(effort.empty() ? wxString{} : Effort(effort));
    model_->SetToolTip(name + (effort.empty() ? wxString{} : wxString::FromUTF8(" \xc2\xb7 ") + Effort(effort)));
    model_->Enable(model != nullptr || std::any_of(config_.providers.begin(), config_.providers.end(),
        [](const auto& provider) { return !provider.models.empty(); }));
    LayoutComposer();
}

void NativeFrame::ModelMenu() {
    wxMenu menu;
    std::vector<std::pair<std::string, std::string>> targets;
    std::vector<NativeModelPickerRow> picker_rows;
    int checked_model = 0;
    int header = 25000;
    for (const auto& provider : config_.providers) {
        if (targets.size() >= 4096) break;
        if (provider.models.empty()) continue;
        menu.Append(header, wxString::FromUTF8(provider.label.empty() ? provider.id : provider.label));
        picker_rows.push_back({header, wxString::FromUTF8(provider.label.empty() ? provider.id : provider.label),
                               NativeModelPickerKind::Header});
        menu.Enable(header++, false);
        for (const auto& model : provider.models) {
            if (targets.size() >= 4096) break;
            const int id = 100 + static_cast<int>(targets.size());
            menu.AppendCheckItem(id, wxString::FromUTF8(model.id))->Check(
                provider.id == config_.active.provider && model.id == config_.active.model);
            picker_rows.push_back({id, wxString::FromUTF8(model.id), NativeModelPickerKind::Model});
            if (provider.id == config_.active.provider && model.id == config_.active.model) checked_model = id;
            targets.emplace_back(provider.id, model.id);
        }
    }
    if (const auto* model = SelectedModel(); model && !EffectiveReasoningEfforts(*model).empty()) {
        menu.AppendSeparator();
        const auto effort = config_.active.reasoning_effort.empty()
            ? EffectiveDefaultReasoningEffort(*model) : config_.active.reasoning_effort;
        menu.Append(30000, T(L"Reasoning effort \u00b7 ") + Effort(effort));
        picker_rows.push_back({30000, T(L"Reasoning effort \u00b7 ") + Effort(effort),
                               NativeModelPickerKind::Reasoning});
    }
    wxWeakRef<NativeFrame> guard(this);
    const auto anchor = model_->ClientToScreen({0, 0});
    const int selected = picker_rows.size() > 12
        ? ShowNativeModelPicker(this, anchor, std::move(picker_rows), checked_model)
        : ShowNativeMenu(this, menu, anchor, NativeMenuAlignment::Bottom);
    if (!guard || guard->IsBeingDeleted()) return;
    if (selected >= 100 && static_cast<size_t>(selected - 100) < targets.size()) {
        config_.active.provider = targets[selected - 100].first;
        config_.active.model = targets[selected - 100].second;
        if (const auto* model = SelectedModel()) config_.active.reasoning_effort = EffectiveDefaultReasoningEffort(*model);
        const auto saved = SaveConfigAtomically(config_);
        if (!saved.ok) Error(saved.error);
        RefreshModels();
        ReasoningMenu();
    } else if (selected == 30000) ReasoningMenu();
}

void NativeFrame::ReasoningMenu() {
    const auto* model = SelectedModel();
    if (!model) return;
    const auto efforts = EffectiveReasoningEfforts(*model);
    if (efforts.empty()) return;
    wxMenu menu;
    const auto active = config_.active.reasoning_effort.empty()
        ? EffectiveDefaultReasoningEffort(*model) : config_.active.reasoning_effort;
    for (size_t index = 0; index < efforts.size(); ++index)
        menu.AppendCheckItem(100 + static_cast<int>(index), Effort(efforts[index]))->Check(efforts[index] == active);
    wxWeakRef<NativeFrame> guard(this);
    const int selected = ShowNativeMenu(this, menu, model_->ClientToScreen({0, 0}), NativeMenuAlignment::Bottom);
    if (!guard || guard->IsBeingDeleted()) return;
    if (selected < 100 || static_cast<size_t>(selected - 100) >= efforts.size()) return;
    config_.active.reasoning_effort = efforts[selected - 100];
    const auto saved = SaveConfigAtomically(config_);
    if (!saved.ok) Error(saved.error);
    RefreshModels();
}

}  // namespace spark
