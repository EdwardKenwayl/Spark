#include "gui/native/attachment_picker.h"
#include "gui/localization.h"

#include <wx/button.h>
#include <wx/filedlg.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/weakref.h>

namespace spark {
#ifdef __WXOSX__
wxString NativeAttachmentPickerDirectory(const wxFileDialog& picker);
bool CancelNativeAttachmentPicker(wxFileDialog& picker);
#endif

namespace {
class AttachmentPicker final : public wxFileDialog {
public:
    AttachmentPicker(wxWindow* owner, const std::string& initial_directory)
        : wxFileDialog(owner, wxString(i18n::Text(L"Attach files and folders")),
                       wxString::FromUTF8(initial_directory), {},
                       wxString(i18n::Text(L"All files")) + " (*.*)|*",
                       wxFD_OPEN | wxFD_FILE_MUST_EXIST | wxFD_MULTIPLE) {
        SetName("spark.attachment-picker");
        SetExtraControlCreator([](wxWindow* parent) -> wxWindow* {
            auto* picker = dynamic_cast<AttachmentPicker*>(parent);
            auto* panel = new wxPanel(parent);
            panel->SetName("spark.attachment-picker.actions");
            auto* layout = new wxBoxSizer(wxHORIZONTAL);
            auto* folder = new wxButton(panel, wxID_ANY, wxString(i18n::Text(L"Attach current folder")));
            folder->SetName("spark.attachment-picker.folder");
            layout->Add(folder, 0, wxALL, panel->FromDIP(6));
            panel->SetSizerAndFit(layout);
            folder->Enable(picker != nullptr);
            folder->Bind(wxEVT_BUTTON, [picker](wxCommandEvent&) {
                if (!picker) return;
                picker->folder_ = picker->GetDirectory();
                if (picker->folder_.empty()) return;
#ifdef __WXOSX__
                if (!CancelNativeAttachmentPicker(*picker)) picker->folder_.clear();
#else
                picker->EndModal(wxID_CANCEL);
#endif
            });
            return panel;
        });
    }

    wxString GetDirectory() const override {
#ifdef __WXGTK__
        if (GetHandle()) {
            // wxFileDialog caches m_dir only on acceptance; query the live chooser.
            wxGtkFileChooser chooser;
            chooser.SetWidget(reinterpret_cast<GtkFileChooser*>(GetHandle()));
            return chooser.GetDirectory();
        }
#elif defined(__WXOSX__)
        const auto directory = NativeAttachmentPickerDirectory(*this);
        if (!directory.empty()) return directory;
#endif
        return wxFileDialog::GetDirectory();
    }

    const wxString& Folder() const { return folder_; }

private:
    wxString folder_;
};
}  // namespace

Result<std::vector<std::string>> PickNativeAttachments(wxWindow* owner, const std::string& initial_directory) {
    if (!owner || owner->IsBeingDeleted()) return Result<std::vector<std::string>>::Success({});
    wxWeakRef<wxWindow> guard(owner);
    AttachmentPicker picker(owner, initial_directory);
    const int result = picker.ShowModal();
    if (!guard || guard->IsBeingDeleted()) return Result<std::vector<std::string>>::Success({});
    wxArrayString paths;
    if (!picker.Folder().empty()) paths.Add(picker.Folder());
    else if (result == wxID_OK) picker.GetPaths(paths);
    std::vector<std::string> selected;
    for (const auto& path : paths) {
        if (selected.size() == 64) break;
        const auto utf8 = path.ToUTF8();
        selected.emplace_back(utf8.data(), utf8.length());
    }
    return Result<std::vector<std::string>>::Success(std::move(selected));
}

}  // namespace spark
