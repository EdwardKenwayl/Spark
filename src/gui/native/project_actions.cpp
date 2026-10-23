#include "gui/native/app.h"
#include "gui/localization.h"
#include "gui/native/popup_menu.h"
#include "gui/native/project_dialog.h"

#include <wx/menu.h>
#include <wx/msgdlg.h>
#include <wx/utils.h>
#include <wx/weakref.h>

#include <algorithm>

namespace spark {
namespace {
wxString T(const wchar_t* text) { return wxString(i18n::Text(text)); }
}

void NativeFrame::AddProject() {
    NativeProjectDialog dialog(this);
    if (dialog.ShowModal() != wxID_OK || dialog.GetFolders().empty()) return;
    const auto folders = dialog.GetFolders();
    for (size_t index = 0; index < projects_.size(); ++index)
        if (NormalizePathForComparison(projects_[index].workspace) == NormalizePathForComparison(folders.front())) {
            return;
        }
    Project project;
    project.name = dialog.GetProjectName();
    project.workspace = folders.front();
    project.folders = folders;
    project.last_opened = UnixTimeSeconds();
    projects_.push_back(std::move(project));
    project_section_expanded_ = true;
    SaveProjects();
    RenderSidebar();
}

void NativeFrame::ProjectMenu(int index, const wxPoint& anchor) {
    if (index < 0 || static_cast<size_t>(index) >= projects_.size()) return;
    const auto identity = NormalizePathForComparison(projects_[index].workspace);
    const auto locate = [&] {
        const auto found = std::find_if(projects_.begin(), projects_.end(), [&](const auto& project) {
            return NormalizePathForComparison(project.workspace) == identity;
        });
        return found == projects_.end() ? -1 : static_cast<int>(found - projects_.begin());
    };
    wxMenu menu;
    menu.Append(1, T(projects_[index].pinned ? L"Unpin" : L"Pin"));
    menu.Append(2, T(L"Edit"));
    menu.AppendSeparator();
    menu.Append(3, T(L"Open in File Explorer"));
    menu.Append(4, T(L"Import Codex chat branch"));
    menu.Append(5, T(L"Archive chats"));
    menu.Append(6, T(L"Remove project"));
    wxWeakRef<NativeFrame> owner(this);
    const int command = ShowNativeMenu(this, menu, anchor);
    if (!owner || (index = locate()) < 0) return;
    if (command == 1) projects_[index].pinned = !projects_[index].pinned;
    else if (command == 2) {
        NativeProjectDialog dialog(this, projects_[index].name, projects_[index].folders,
                                    NativeProjectDialog::Mode::Edit);
        const int result = dialog.ShowModal();
        if (!owner || (index = locate()) < 0) return;
        if (result == wxID_DELETE) {
            if (wxMessageBox(T(L"Remove this project from the sidebar? Local folders will be kept."),
                             T(L"Remove project"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
                return;
            if (!owner || (index = locate()) < 0) return;
            projects_.erase(projects_.begin() + index);
        }
        else if (result == wxID_OK && !dialog.GetFolders().empty()) {
            projects_[index].name = dialog.GetProjectName();
            projects_[index].folders = dialog.GetFolders();
            projects_[index].workspace = projects_[index].folders.front();
        } else return;
    } else if (command == 3) {
        wxLaunchDefaultApplication(wxString::FromUTF8(projects_[index].workspace)); return;
    } else if (command == 4) { ImportConversation(index); return; }
    else if (command == 5) {
        const auto project = projects_[index];
        if (wxMessageBox(T(L"Archive all chats in this project? You can restore them in Settings > Archived chats."), T(L"Archive chats"),
                         wxYES_NO | wxNO_DEFAULT | wxICON_QUESTION, this) != wxYES) return;
        if (!owner || (index = locate()) < 0) return;
        std::vector<std::string> paths;
        for (const auto& item : history_items_) {
            if (!item.has_messages || item.scope == "recent" || runtime_.Running(item.path) || external_.count(item.path))
                continue;
            if (std::any_of(project.folders.begin(), project.folders.end(), [&](const auto& folder) {
                return NormalizePathForComparison(folder) == NormalizePathForComparison(item.workspace);
            })) paths.push_back(item.path);
        }
        const auto archive = NativePath(JoinPath(config_.storage.data_dir, "archive"));
        std::error_code error;
        spark::fs::create_directories(archive, error);
        if (error) { Error(error.message()); return; }
        for (const auto& path : paths) {
            const auto file = NativePath(path), destination = archive / file.filename();
            if (spark::fs::exists(destination, error)) { Error("An archive with this filename already exists."); break; }
            if (error) { Error(error.message()); break; }
            spark::fs::rename(file, destination, error);
            if (error) { Error(error.message()); break; }
            if (active_ == path) NewConversation(index);
            views_.erase(path);
        }
        RefreshSidebar(); return;
    } else if (command == 6) {
        if (wxMessageBox(T(L"Remove this project from the sidebar? Local folders will be kept."),
                         T(L"Remove project"), wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this) != wxYES)
            return;
        if (!owner || (index = locate()) < 0) return;
        projects_.erase(projects_.begin() + index);
    }
    else return;
    SaveProjects(); RenderSidebar();
}

}  // namespace spark
