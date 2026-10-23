#include "gui/native/startup_dpi.h"
#include "gui/native/controls.h"

#include <wx/app.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <cmath>
#include <iostream>
#include <string>

class DpiTestApplication : public wxApp {
public:
    bool OnInit() override { return true; }
};
wxIMPLEMENT_APP_NO_MAIN(DpiTestApplication);

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) { ++failures; std::cerr << message << '\n'; }
}
void Events() {
    for (int index = 0; index < 5; ++index) { wxYield(); wxMilliSleep(20); }
}

void CheckGeometry(wxFrame& frame) {
    const auto check = [&](const wxRect& work, const wxRect& bounds, const wxSize& minimum) {
        const auto result = spark::NativeStartupGeometryFor(frame, work);
        Check(result.bounds.GetSize() == bounds.GetSize() &&
                  std::abs(result.bounds.x - bounds.x) <= 1 &&
                  std::abs(result.bounds.y - bounds.y) <= 1 && result.minimum == minimum,
              "startup geometry matches the logical work area and DPI-converted limits");
        Check(work.Contains(result.bounds) && result.minimum.x <= result.bounds.width &&
                  result.minimum.y <= result.bounds.height,
              "startup bounds and resize minimum remain inside the monitor work area");
    };
    const auto rect = [&](int x, int y, int width, int height) {
        return wxRect(frame.FromDIP(wxPoint(x, y)), frame.FromDIP(wxSize(width, height)));
    };
    check(rect(0, 0, 1920, 1080), rect(280, 150, 1360, 780), frame.FromDIP(wxSize(760, 520)));
    check(rect(-1280, 24, 1280, 800), rect(-1178, 104, 1075, 640), frame.FromDIP(wxSize(760, 520)));
    check(rect(1920, -540, 960, 540), rect(1926, -534, 948, 528), frame.FromDIP(wxSize(760, 520)));
    check(rect(-400, -240, 400, 240), rect(-394, -234, 388, 228), frame.FromDIP(wxSize(388, 228)));
    const auto tiny = spark::NativeStartupGeometryFor(frame, wxRect(50, 60, 1, 1));
    Check(tiny.bounds == wxRect(50, 60, 1, 1) && tiny.minimum == wxSize(1, 1),
          "a work area smaller than the margin still produces positive bounded dimensions");
}

void CheckFonts() {
    wxFont desktop(wxFontInfo(14).Family(wxFONTFAMILY_SWISS));
    desktop.SetFractionalPointSize(14.5);
    Check(spark::NativeStartupFont(desktop) == desktop,
          "startup retains the desktop font, including fractional accessibility text size");
    wxFont small(wxFontInfo(9));
    Check(spark::NativeStartupFont(small).GetFractionalPointSize() == 10 &&
              spark::NativeStartupFont(wxFont{}).GetFractionalPointSize() == 10,
          "small or unavailable desktop fonts retain the Windows ten-point baseline");
}
}  // namespace

int main(int argc, char** argv) {
    double expected_scale = 0;
    if (argc == 3 && std::string(argv[1]) == "--expect-scale") {
        if (!wxString::FromUTF8(argv[2]).ToDouble(&expected_scale) ||
            !std::isfinite(expected_scale) || expected_scale <= 0) return 2;
    } else if (argc != 1) return 2;
    wxSetAssertHandler([](const wxString&, int, const wxString&, const wxString&, const wxString& message) {
        ++failures;
        std::cerr << message << '\n';
    });
    if (!wxEntryStart(argc, argv) || !wxTheApp->CallOnInit()) return 2;
    auto* frame = new wxFrame(nullptr, wxID_ANY, "Synthetic DPI fixture");
    spark::ApplyNativeStartupDpi(*frame);
    CheckGeometry(*frame);
    CheckFonts();
    const auto font = spark::NativeStartupFont(wxSystemSettings::GetFont(wxSYS_DEFAULT_GUI_FONT));
    auto* panel = new wxPanel(frame);
    auto* button = new spark::NativeButton(panel, "Synthetic action");
    auto* input = new wxTextCtrl(panel, wxID_ANY, "Synthetic input");
    auto* row = new wxBoxSizer(wxVERTICAL);
    row->Add(button, 0, wxALL, frame->FromDIP(8));
    row->Add(input, 0, wxEXPAND | wxALL, frame->FromDIP(8));
    panel->SetSizer(row);
    auto* root = new wxBoxSizer(wxVERTICAL);
    root->Add(panel, 1, wxEXPAND);
    frame->SetSizer(root);
    frame->Show();
    Events();
    const wxDisplay display(frame);
    Check(display.IsOk(), "the DPI fixture has a real display");
    if (display.IsOk()) {
        const auto work = display.GetClientArea();
        const auto expected = spark::NativeStartupGeometryFor(*frame, work);
        Check(work.Contains(frame->GetScreenRect()) && frame->GetSize() == expected.bounds.GetSize() &&
                  frame->GetMinSize() == expected.minimum,
              "mapped frame uses the same clamped logical geometry as startup");
    }
    Check(frame->GetFont() == font && panel->GetFont() == font &&
              button->GetFont() == font && input->GetFont() == font,
          "the startup desktop font reaches native and custom controls without point-size scaling");
    Check(button->GetSize().x >= button->FromDIP(110) &&
              button->GetSize().y >= button->FromDIP(34),
          "custom control dimensions follow the toolkit DIP conversion");
#if defined(__WXGTK3__) || defined(__WXOSX__)
    Check(frame->FromDIP(wxSize(320, 200)) == wxSize(320, 200),
          "GTK3 and Cocoa keep logical dimensions independent of backing scale");
#endif
    const double scale = frame->GetDPIScaleFactor();
    if (expected_scale > 0)
        Check(std::abs(scale - expected_scale) < 0.01, "the fixture is running at the requested DPI scale");
    std::cout << "DPI scale: " << scale << "; content scale: " << frame->GetContentScaleFactor()
              << "; font points: " << font.GetFractionalPointSize()
              << "; frame: " << frame->GetSize().x << 'x' << frame->GetSize().y << '\n';
    frame->Destroy();
    Events();
    wxTheApp->OnExit();
    wxEntryCleanup();
    return failures ? 1 : 0;
}
