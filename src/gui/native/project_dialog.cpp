#include "gui/native/project_dialog.h"
#include "gui/native/controls.h"
#include "gui/localization.h"

#include <wx/dcclient.h>
#include <wx/dirdlg.h>
#include <wx/filename.h>
#include <wx/msgdlg.h>
#include <wx/settings.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/vlbox.h>
#include <algorithm>

namespace spark {
namespace {

wxString T(std::wstring_view message) {
    return wxString(i18n::Text(message));
}

}  // namespace

class NativeProjectDialog::FolderList final : public wxVListBox {
public:
    FolderList(wxWindow* parent, std::vector<std::string>& folders)
        : wxVListBox(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE),
          folders_(folders) {
        SetName("spark.project.folders");
        SetMinSize(FromDIP(wxSize(512, 150)));
        Bind(wxEVT_MOTION, [this](wxMouseEvent& event) {
            int row = VirtualHitTest(event.GetY());
            if (row < 0 || static_cast<size_t>(row) >= folders_.size()) row = wxNOT_FOUND;
            if (row != hovered_) {
                hovered_ = row;
                RefreshAll();
            }
            event.Skip();
        });
        Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& event) {
            hovered_ = wxNOT_FOUND;
            RefreshAll();
            event.Skip();
        });
        Bind(wxEVT_LEFT_UP, [this](wxMouseEvent& event) {
            const int row = VirtualHitTest(event.GetY());
            if (row >= 0 && static_cast<size_t>(row) < folders_.size()
                && event.GetX() >= GetClientSize().x - FromDIP(42)) {
                Remove(row);
            } else {
                event.Skip();
            }
        });
        Reload();
    }

    void Reload(int selected = wxNOT_FOUND) {
        hovered_ = wxNOT_FOUND;
        SetItemCount(folders_.size());
        if (selected < 0 || static_cast<size_t>(selected) >= folders_.size()) selected = wxNOT_FOUND;
        SetSelection(selected);
        if (selected != wxNOT_FOUND) ScrollToRow(static_cast<size_t>(selected));
        RefreshAll();
    }

protected:
    wxCoord OnMeasureItem(size_t) const override {
        return std::max(FromDIP(34), GetCharHeight() + FromDIP(8));
    }

    void OnDrawBackground(wxDC&, const wxRect&, size_t) const override {}

    void OnDrawItem(wxDC& dc, const wxRect& rect, size_t index) const override {
        if (index >= folders_.size()) return;
        const auto palette = NativeColors(GetForegroundColour().Red() > 180);
        const bool hovered = static_cast<int>(index) == hovered_;
        dc.SetPen(*wxTRANSPARENT_PEN);
        dc.SetBrush(wxBrush(hovered ? wxColour(52, 53, 57) : palette.panel));
        dc.DrawRectangle(rect);
        const int x = rect.x + FromDIP(18);
        const int y = rect.y + rect.height / 2;
        const wxPoint glyph[] = {
            {x - FromDIP(7), y + FromDIP(6)}, {x - FromDIP(7), y - FromDIP(6)},
            {x - FromDIP(2), y - FromDIP(6)}, {x + FromDIP(1), y - FromDIP(3)},
            {x + FromDIP(8), y - FromDIP(3)}, {x + FromDIP(8), y + FromDIP(6)},
            {x - FromDIP(7), y + FromDIP(6)}
        };
        dc.SetPen(wxPen(palette.muted));
        dc.DrawLines(7, glyph);
        dc.SetFont(GetFont());
        dc.SetTextForeground(palette.text);
        const auto path = wxString::FromUTF8(folders_[index]);
        const int available = std::max(1, rect.width - FromDIP(38 + (hovered ? 42 : 14)));
        const auto label = wxControl::Ellipsize(path, dc, wxELLIPSIZE_END, available);
        dc.DrawText(label, rect.x + FromDIP(38), y - dc.GetTextExtent(label).y / 2);
        if (hovered) {
            dc.SetPen(wxPen(wxColour(232, 135, 135)));
            const int close_x = rect.GetRight() + 1 - FromDIP(24);
            const int arm = FromDIP(4);
            dc.DrawLine(close_x - arm, y - arm, close_x + arm, y + arm);
            dc.DrawLine(close_x + arm, y - arm, close_x - arm, y + arm);
        }
    }

private:
    void Remove(int row) {
        if (row < 0 || static_cast<size_t>(row) >= folders_.size()) return;
        folders_.erase(folders_.begin() + row);
        const int next = folders_.empty() ? wxNOT_FOUND
                                         : std::min(row, static_cast<int>(folders_.size()) - 1);
        Reload(next);
    }

    std::vector<std::string>& folders_;
    int hovered_ = wxNOT_FOUND;
};

NativeProjectDialog::NativeProjectDialog(wxWindow* parent, const std::string& existing_name,
                                         const std::vector<std::string>& existing_folders, Mode mode)
    : wxDialog(parent, wxID_ANY,
               mode == Mode::Edit ? T(L"Edit project") : T(L"Create project"),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE),
      name_(existing_name), folders_(existing_folders), draft_folders_(existing_folders) {
    auto font = GetFont();
    font.SetPointSize(10);
    SetFont(font);
    SetName("spark.project.dialog");
    SetAffirmativeId(wxID_OK);
    SetEscapeId(wxID_CANCEL);

    auto* root = new wxBoxSizer(wxHORIZONTAL);
    root->AddSpacer(FromDIP(24));
    auto* content = new wxBoxSizer(wxVERTICAL);
    root->Add(content, 1, wxEXPAND);
    root->AddSpacer(FromDIP(24));
    content->AddSpacer(FromDIP(18));
    auto* name_label = new wxStaticText(this, wxID_ANY, T(L"Project name"));
    name_label->SetMinSize(wxSize(-1, std::max(FromDIP(20), name_label->GetBestSize().y)));
    content->Add(name_label, 0, wxEXPAND);
    content->AddSpacer(FromDIP(4));
    name_edit_ = new wxTextCtrl(this, wxID_ANY, wxString::FromUTF8(name_),
                                wxDefaultPosition, wxDefaultSize, wxTE_PROCESS_ENTER | wxBORDER_NONE);
    name_edit_->SetName("spark.project.name");
    name_edit_->SetMinSize(wxSize(-1, std::max(FromDIP(34), name_edit_->GetBestSize().y)));
    content->Add(name_edit_, 0, wxEXPAND);
    content->AddSpacer(FromDIP(18));
    auto* folder_label = new wxStaticText(this, wxID_ANY, T(L"Source folders"));
    folder_label->SetMinSize(wxSize(-1, std::max(FromDIP(20), folder_label->GetBestSize().y)));
    content->Add(folder_label, 0, wxEXPAND);
    content->AddSpacer(FromDIP(6));
    folder_list_ = new FolderList(this, draft_folders_);
    content->Add(folder_list_, 1, wxEXPAND);
    content->AddSpacer(FromDIP(8));

    // NativeButton's generic 110-DIP minimum would distort Windows' 84/102-DIP buttons.
    const auto button = [this](const wxString& label, int width, wxWindowID id) {
        auto* control = new NativeButton(this, label);
        control->SetId(id);
        control->SetLabelAlignment(wxALIGN_CENTER);
        wxClientDC dc(control);
        dc.SetFont(control->GetFont());
        control->SetMinSize(wxSize(std::max(FromDIP(width), dc.GetTextExtent(label).x + FromDIP(20)),
                                   std::max(FromDIP(34), dc.GetTextExtent(label).y + FromDIP(8))));
        return control;
    };
    auto* add = button(T(L"Add folder"), 512, wxID_ADD);
    content->Add(add, 0, wxEXPAND);
    content->AddSpacer(FromDIP(14));
    auto* footer = new wxBoxSizer(wxHORIZONTAL);
    if (mode == Mode::Edit) {
        auto* remove = button(T(L"Remove local project"), 142, wxID_DELETE);
        remove->SetAppearance(NativeButtonAppearance::Destructive);
        footer->Add(remove, 0, wxALIGN_CENTER_VERTICAL);
        remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
            remove_requested_ = true;
            EndModal(wxID_DELETE);
        });
    }
    footer->AddStretchSpacer();
    auto* cancel = button(T(L"Cancel"), 84, wxID_CANCEL);
    footer->Add(cancel, 0, wxALIGN_CENTER_VERTICAL);
    footer->AddSpacer(FromDIP(10));
    auto* save = button(mode == Mode::Edit ? T(L"Save") : T(L"Create project"),
                        102, wxID_OK);
    save->SetAppearance(NativeButtonAppearance::Primary);
    footer->Add(save, 0, wxALIGN_CENTER_VERTICAL);
    content->Add(footer, 0, wxEXPAND);
    content->AddSpacer(FromDIP(14));
    SetSizer(root);
    const auto minimum = root->GetMinSize();
    SetMinClientSize(minimum);
    SetClientSize(wxSize(std::max(FromDIP(560), minimum.x), std::max(FromDIP(374), minimum.y)));

    const bool dark = parent && parent->GetForegroundColour().IsOk()
                          ? parent->GetForegroundColour().Red() > 180
                          : wxSystemSettings::GetAppearance().IsDark();
    ApplyNativeColors(this, dark);
    const auto palette = NativeColors(dark);
    SetBackgroundColour(palette.panel);
    name_label->SetBackgroundColour(palette.panel);
    folder_label->SetBackgroundColour(palette.panel);
    folder_list_->SetBackgroundColour(palette.panel);
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { AddFolder(); });
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Accept(); }, wxID_OK);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Cancel(); }, wxID_CANCEL);
    name_edit_->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent&) { Accept(); });
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { Cancel(); });
    Bind(wxEVT_CHAR_HOOK, [this](wxKeyEvent& event) {
        if (event.GetKeyCode() == WXK_ESCAPE) {
            Cancel();
        } else if ((event.GetKeyCode() == WXK_RETURN || event.GetKeyCode() == WXK_NUMPAD_ENTER)
                   && !event.HasModifiers() && !dynamic_cast<NativeButton*>(wxWindow::FindFocus())) {
            Accept();
        } else {
            event.Skip();
        }
    });
}

int NativeProjectDialog::ShowModal() {
    remove_requested_ = false;
    draft_folders_ = folders_;
    name_edit_->ChangeValue(wxString::FromUTF8(name_));
    folder_list_->Reload();
    Layout();
    if (GetParent()) CentreOnParent();
    else CentreOnScreen();
    name_edit_->SetFocus();
    return wxDialog::ShowModal();
}

void NativeProjectDialog::AddFolder() {
    const auto initial = draft_folders_.empty() ? wxString{} : wxString::FromUTF8(draft_folders_.back());
    wxDirDialog picker(this, T(L"Choose a project folder"), initial,
                       wxDD_DEFAULT_STYLE | wxDD_DIR_MUST_EXIST);
    if (picker.ShowModal() != wxID_OK) return;
    const auto folder = picker.GetPath();
    if (folder.empty()) return;
    const bool duplicate = std::any_of(draft_folders_.begin(), draft_folders_.end(),
                                       [&folder](const std::string& existing) {
        return wxString::FromUTF8(existing).CmpNoCase(folder) == 0;
    });
    if (duplicate) return;
    const auto utf8 = folder.ToUTF8();
    draft_folders_.emplace_back(utf8.data(), utf8.length());
    folder_list_->Reload(static_cast<int>(draft_folders_.size()) - 1);
    folder_list_->SetFocus();
}

void NativeProjectDialog::Accept() {
    if (draft_folders_.empty()) {
        wxMessageBox(T(L"Add at least one source folder first."),
                     T(L"Create project"), wxOK | wxICON_INFORMATION, this);
        folder_list_->SetFocus();
        return;
    }
    auto name = name_edit_->GetValue();
    auto nonblank = name;
    if (nonblank.Trim().Trim(false).empty()) {
        const auto first = wxFileName::DirName(wxString::FromUTF8(draft_folders_.front()));
        const auto parts = first.GetDirs();
        name = parts.empty() ? first.GetFullPath() : parts.Last();
        if (name.empty()) name = T(L"Create project");
    }
    const auto utf8 = name.ToUTF8();
    name_.assign(utf8.data(), utf8.length());
    folders_ = draft_folders_;
    remove_requested_ = false;
    EndModal(wxID_OK);
}

void NativeProjectDialog::Cancel() {
    remove_requested_ = false;
    if (IsModal()) EndModal(wxID_CANCEL);
    else {
        SetReturnCode(wxID_CANCEL);
        Hide();
    }
}

}  // namespace spark
