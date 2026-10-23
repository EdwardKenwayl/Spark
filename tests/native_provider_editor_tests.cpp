#include "gui/native/provider_editor.h"
#include "gui/native/reasoning_popup.h"
#include "gui/localization.h"

#include <wx/app.h>
#include <wx/dcclient.h>
#include <wx/dcmemory.h>
#include <wx/filename.h>
#include <wx/image.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/frame.h>
#include <wx/utils.h>

#include <iostream>
#include <set>

class ProviderTestApplication : public wxApp {
public:
    bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(ProviderTestApplication);

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void Events() {
    for (int index = 0; index < 5; ++index) { wxYield(); wxMilliSleep(20); }
}
void Click(wxWindow* window, wxPoint point) {
    wxMouseEvent event(wxEVT_LEFT_DOWN);
    event.SetEventObject(window);
    event.SetPosition(point);
    window->GetEventHandler()->ProcessEvent(event);
    Events();
}
void Capture(wxWindow* window, const wxString& name) {
    const wxString directory = wxGetenv("SPARK_PROVIDER_SCREENSHOTS");
    if (directory.empty()) return;
    wxFileName::Mkdir(directory, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    const wxSize size = window->GetClientSize();
    wxBitmap bitmap(size.x, size.y);
    wxMemoryDC target(bitmap);
    wxClientDC input(window);
    const wxPoint origin = window->GetScreenPosition();
    std::cout << name << " bounds: " << origin.x << ',' << origin.y << ' '
              << size.x << 'x' << size.y << '\n';
    target.Blit(0, 0, size.x, size.y, &input, 0, 0);
    target.SelectObject(wxNullBitmap);
    const auto image = bitmap.ConvertToImage();
    std::set<unsigned> colors;
    const auto* pixels = image.GetData();
    for (size_t pixel = 0; pixel < static_cast<size_t>(size.x) * size.y && colors.size() < 16; pixel += 13)
        colors.insert((pixels[3 * pixel] << 16) | (pixels[3 * pixel + 1] << 8) | pixels[3 * pixel + 2]);
    Check(colors.size() >= 6, "provider screenshots contain rendered text and controls");
    Check(image.SaveFile(directory + "/" + name + ".jpg", wxBITMAP_TYPE_JPEG),
          "synthetic provider screenshot is saved");
}
}  // namespace

int main(int argc, char** argv) {
    using namespace spark;
    wxSetAssertHandler([](const wxString&, int, const wxString&, const wxString&, const wxString& message) {
        ++failures;
        std::cerr << message << '\n';
    });
    if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit()) return 2;
    wxInitAllImageHandlers();
    i18n::SetLocale("en-US");
    ProviderConfig provider;
    provider.label = "Synthetic provider";
    provider.base_url = "http://127.0.0.1:1";
    ModelConfig model;
    model.id = "deepseek-flash";
    provider.models.push_back(model);
    ModelConfig custom;
    custom.id = "custom-model";
    custom.capabilities.reasoning_efforts = {"low", "high", "max"};
    provider.models.push_back(custom);
    auto* host = new wxFrame(nullptr, wxID_ANY, "Synthetic provider editor");
    auto* editor = new NativeProviderEditor(host, provider, ContextConfig{}, "dark");
    auto* layout = new wxBoxSizer(wxVERTICAL);
    layout->Add(editor, 1, wxEXPAND);
    host->SetSizer(layout);
    host->SetSize(1500, 900);
    host->Show();
    Events();
    auto* reasoning = wxWindow::FindWindowByName("spark.provider.editor.model.0.reasoning", host);
    auto* context = dynamic_cast<wxTextCtrl*>(
        wxWindow::FindWindowByName("spark.provider.editor.model.0.context", host));
    Check(reasoning && reasoning->GetLabel() == "low, high, max",
          "reasoning button displays the supported levels, not only the default");
    Check(context && context->GetHint() == "272000", "empty context shows the effective default");
    for (const int width : {900, 560, 1500}) {
        host->SetSize(width, 900);
        Events();
        for (const auto* field : {"id", "context", "auto-compact"}) {
            auto* label = dynamic_cast<wxStaticText*>(wxWindow::FindWindowByName(
                wxString("spark.provider.editor.model.0.label.") + field, host));
            Check(label != nullptr, "compact model rows expose field descriptions");
            if (!label) continue;
            Check(label->IsShown() == (width < 960), "field labels follow the responsive layout");
            if (label->IsShown()) {
                wxClientDC dc(label);
                dc.SetFont(label->GetFont());
                Check(label->GetSize().y >= dc.GetTextExtent(label->GetLabel()).y,
                      "field descriptions are not vertically clipped");
            }
        }
        if (width != 1500) {
            auto* model_row = wxWindow::FindWindowByName("spark.provider.editor.model.0", host);
            auto* body = model_row ? model_row->GetParent() : nullptr;
            if (body) {
                auto* scrolled = dynamic_cast<wxScrolledWindow*>(body);
                if (scrolled) scrolled->Scroll(0, model_row->GetPosition().y / 12);
                Events();
                Capture(host, wxString::Format("provider-%d", width));
                if (scrolled) scrolled->Scroll(0, 0);
            }
        }
    }
    if (reasoning) {
        auto* row = wxWindow::FindWindowByName("spark.provider.editor.model.0", host);
        if (row) {
            auto* body = dynamic_cast<wxScrolledWindow*>(row->GetParent());
            if (body) body->Scroll(0, row->GetPosition().y / body->FromDIP(12));
            Events();
        }
        NativeReasoningPopupState state;
        state.efforts = {"low", "high", "max"};
        state.selected = state.efforts;
        state.default_effort = "high";
        int changes = 0;
        ShowNativeReasoningPopup(reasoning, reasoning->ClientToScreen({0, reasoning->GetSize().y}), state,
            [&](const NativeReasoningPopupState& next) { state = next; ++changes; });
        Events();
        auto* popup = wxWindow::FindWindowByName("spark.reasoning.popup", host);
        auto* canvas = wxWindow::FindWindowByName("spark.reasoning.popup.canvas", host);
        Check(popup && canvas && popup->IsShownOnScreen(), "reasoning popup opens with a visible canvas");
        if (popup && canvas) {
            Check(canvas->GetClientSize() == popup->GetClientSize(),
                  "reasoning canvas fills the popup on each platform");
            int paints = 0;
            canvas->Bind(wxEVT_PAINT, [&](wxPaintEvent& event) { ++paints; event.Skip(); });
            canvas->Refresh(false);
            canvas->Update();
            Events();
            Check(paints > 0, "reasoning canvas is painted after opening");
            Capture(canvas, "reasoning");
            Check(popup->GetSize().x == host->FromDIP(296), "reasoning popup uses Windows geometry");
            Click(canvas, canvas->FromDIP(wxPoint(40, 42 + 2 * 32 + 16)));
            Check(changes == 1 && state.selected == std::vector<std::string>({"low", "high"}),
                  "clicking the last level changes only that level");
            Click(canvas, canvas->FromDIP(wxPoint(40, 42 + 32 + 16)));
            Check(changes == 2 && state.selected == std::vector<std::string>({"low"}),
                  "adjacent reasoning rows have non-overlapping hit areas");
            const int chip_y = 42 + 3 * 32 + 44 + 16;
            Click(canvas, canvas->FromDIP(wxPoint(35, chip_y)));
            Check(changes == 3 && state.default_effort.empty(), "Auto clears the explicit default");
            Click(canvas, {canvas->GetSize().x / 2, canvas->GetSize().y - canvas->FromDIP(22)});
            Check(changes == 4 && state.built_in_defaults &&
                      state.selected == std::vector<std::string>({"low", "high", "max"}),
                  "reset restores the model's available levels without closing the popup");
            wxKeyEvent escape(wxEVT_KEY_DOWN);
            escape.m_keyCode = WXK_ESCAPE;
            escape.SetEventObject(canvas);
            canvas->GetEventHandler()->ProcessEvent(escape);
            Events();
        }
    }
    auto* custom_reasoning = wxWindow::FindWindowByName("spark.provider.editor.model.1.reasoning", host);
    if (custom_reasoning) {
        wxCommandEvent open(wxEVT_BUTTON, custom_reasoning->GetId());
        open.SetEventObject(custom_reasoning);
        custom_reasoning->GetEventHandler()->ProcessEvent(open);
        Events();
        auto* canvas = wxWindow::FindWindowByName("spark.reasoning.popup.canvas", host);
        if (canvas) {
            Click(canvas, {canvas->GetSize().x / 2, canvas->GetSize().y - canvas->FromDIP(22)});
            for (int pass = 0; pass < 3; ++pass) Events();
            Check(editor->GetProvider().models.size() == 2 &&
                      editor->GetProvider().models[1].capabilities.reasoning_efforts ==
                          custom.capabilities.reasoning_efforts,
                  "editing an unknown model preserves its explicitly configured levels");
        } else Check(false, "custom model reasoning popup opens");
    }
    host->Destroy();
    Events();
    wxTheApp->OnExit();
    wxEntryCleanup();
    if (!failures) std::cout << "PASS: provider model labels, defaults and reasoning popup\n";
    return failures ? 1 : 0;
}
