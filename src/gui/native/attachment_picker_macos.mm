#include "gui/native/attachment_picker.h"

#include <wx/filedlg.h>
#import <AppKit/AppKit.h>

namespace spark {

wxString NativeAttachmentPickerDirectory(const wxFileDialog& picker) {
    id window = (id)picker.GetWXWindow();
    if (!window || ![window isKindOfClass:[NSSavePanel class]]) return {};
    const char* path = [[(NSSavePanel*)window directoryURL] fileSystemRepresentation];
    return path ? wxString::FromUTF8(path) : wxString{};
}

bool CancelNativeAttachmentPicker(wxFileDialog& picker) {
    id window = (id)picker.GetWXWindow();
    if (!window || ![window isKindOfClass:[NSSavePanel class]]) return false;
    // Finish NSOpenPanel's modal loop without requiring a selected file.
    [(NSSavePanel*)window cancel:nil];
    return true;
}

}  // namespace spark
