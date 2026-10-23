#include "gui/native/app.h"
#include "gui/native/add_menu.h"
#include "gui/native/attachment_picker.h"
#include "gui/native/text_prompt.h"
#include "gui/localization.h"
#include "platform/desktop_capture.h"

#include <wx/clipbrd.h>
#include <wx/dataobj.h>
#include <wx/image.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <set>

namespace spark {
namespace {
wxString U(const std::string& text) { return wxString::FromUTF8(text.data(), text.size()); }
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
std::string S(const wxString& text) { return text.ToUTF8().data(); }
bool ImagePath(const std::string& path) {
    static const std::set<std::string> extensions{".png", ".jpg", ".jpeg", ".gif", ".bmp", ".webp"};
    return extensions.count(LowerAscii(PathToUtf8(NativePath(path).extension()))) != 0;
}
}  // namespace

void NativeFrame::AddAttachments() {
    const auto windows = SnapshotWindows();
    auto snapshot = std::find_if(windows.begin(), windows.end(), [this](const DesktopWindow& window) {
        return window.id == snapshot_target_id_;
    });
    if (snapshot == windows.end() && !windows.empty()) snapshot = windows.begin();
    snapshot_target_id_ = snapshot == windows.end() ? 0 : snapshot->id;
    const wxString snapshot_title = snapshot == windows.end() ? wxString{} : U(snapshot->title);
    wxWeakRef<NativeFrame> guard(this);
    const auto selected = ShowNativeAddMenu(this, input_panel_->GetScreenRect(), snapshot_title, U(goal_), plan_);
    if (!guard || guard->IsBeingDeleted()) return;
    composer_->SetFocus();
    if (selected == NativeAddAction::Files) {
        const auto paths = PickNativeAttachments(this);
        if (!guard || guard->IsBeingDeleted()) return;
        if (!paths.ok) { Error(paths.error); return; }
        for (const auto& path : paths.value) AttachPath(path);
        SaveDraft();
        composer_->SetFocus();
    } else if (selected == NativeAddAction::Snapshot) {
        if (snapshot == windows.end()) return;
        const std::string directory = JoinPath(JoinPath(config_.storage.data_dir, "drafts"), "clipboard");
        std::error_code error;
        spark::fs::create_directories(NativePath(directory), error);
        if (error) { Error(error.message()); return; }
        const std::string path = JoinPath(directory, "snapshot-" + NewSessionId() + ".png");
        const auto captured = CaptureDesktopWindow(snapshot->id, path);
        if (!captured.ok) { Error(captured.error); return; }
        AttachPath(path);
        SaveDraft();
        composer_->SetFocus();
    } else if (selected == NativeAddAction::Plan) { plan_ = !plan_; draft_dirty_ = true; SaveDraft(); }
    else if (selected == NativeAddAction::Goal) {
        NativeTextPrompt goal(this, T(L"Goal (clear to remove)"), U(goal_));
        const int result = goal.ShowModal();
        if (!guard || guard->IsBeingDeleted()) return;
        if (result == wxID_OK) {
            goal_ = TruncateUtf8(Trim(S(goal.GetValue())), 4096);
            draft_dirty_ = true;
            SaveDraft();
        }
        composer_->SetFocus();
    }
}

void NativeFrame::AttachPath(const std::string& path) {
    if (attachments_.size() >= 64 || std::find(attachments_.begin(), attachments_.end(), path) != attachments_.end()) return;
    std::error_code error;
    const auto native = NativePath(path);
    const bool file = spark::fs::is_regular_file(native, error);
    if (error || (!file && !spark::fs::is_directory(native, error)) || error) {
        Error("Attachment is not a file or folder: " + path); return;
    }
    if (file && ImagePath(path)) {
        const auto bytes = spark::fs::file_size(native, error);
        if (error || bytes == 0 || bytes > 16 * 1024 * 1024) { Error("Images must be between 1 byte and 16 MiB."); return; }
    }
    attachments_.push_back(path);
    draft_dirty_ = true;
    RefreshAttachments();
    UpdateControls();
}
bool NativeFrame::PasteAttachments() {
    if (!wxTheClipboard->Open()) return false;
    if (wxTheClipboard->IsSupported(wxDF_FILENAME)) {
        wxFileDataObject files;
        const bool received = wxTheClipboard->GetData(files);
        wxTheClipboard->Close();
        if (received) for (const auto& path : files.GetFilenames()) AttachPath(S(path));
        return received;
    }
    if (!wxTheClipboard->IsSupported(wxDF_BITMAP)) { wxTheClipboard->Close(); return false; }
    wxBitmapDataObject bitmap;
    const bool received = wxTheClipboard->GetData(bitmap);
    wxTheClipboard->Close();
    if (!received || !bitmap.GetBitmap().IsOk()) return false;
    const auto image = bitmap.GetBitmap().ConvertToImage();
    if (static_cast<uint64_t>(image.GetWidth()) * image.GetHeight() > 16 * 1024 * 1024) {
        Error("The clipboard image exceeds 16 megapixels."); return true;
    }
    const std::string directory = JoinPath(JoinPath(config_.storage.data_dir, "drafts"), "clipboard");
    std::error_code error;
    spark::fs::create_directories(NativePath(directory), error);
    if (error) { Error(error.message()); return true; }
    const std::string path = JoinPath(directory, "clipboard-" + NewSessionId() + ".png");
    if (!image.SaveFile(U(path), wxBITMAP_TYPE_PNG)) { Error("Could not save the clipboard image."); return true; }
    AttachPath(path);
    return true;
}

void NativeFrame::RefreshAttachments() {
    attachment_panel_->SetAttachments(attachments_);
    LayoutComposer();
}
void NativeFrame::PreviewAttachment(const std::string& path) {
    if (!path.empty()) wxLaunchDefaultApplication(U(path));
}
}  // namespace spark
