#pragma once

#include <wx/dialog.h>
#include <string>
#include <vector>

class wxTextCtrl;

namespace spark {

// Folder paths and names use UTF-8. Name/folder results change only on wxID_OK.
class NativeProjectDialog final : public wxDialog {
public:
    enum class Mode { Create, Edit };

    explicit NativeProjectDialog(wxWindow* parent,
                                 const std::string& existing_name = {},
                                 const std::vector<std::string>& existing_folders = {},
                                 Mode mode = Mode::Create);
    // Returns wxID_OK, wxID_CANCEL, or edit-only wxID_DELETE.
    int ShowModal() override;
    const std::string& GetProjectName() const { return name_; }
    const std::vector<std::string>& GetFolders() const { return folders_; }
    bool RemoveRequested() const { return remove_requested_; }

private:
    class FolderList;
    void AddFolder();
    void Accept();
    void Cancel();

    std::string name_;
    std::vector<std::string> folders_;
    std::vector<std::string> draft_folders_;
    wxTextCtrl* name_edit_ = nullptr;
    FolderList* folder_list_ = nullptr;
    bool remove_requested_ = false;
};

}  // namespace spark
