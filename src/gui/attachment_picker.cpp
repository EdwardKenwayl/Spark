#include "gui/attachment_picker.h"
#include "gui/localization.h"

#ifdef SPARK_WINDOWS_XP
#include <commdlg.h>
#else
#include <shobjidl.h>
#endif

namespace spark {
#ifdef SPARK_WINDOWS_XP
Result<std::vector<std::string>> PickAttachments(HWND owner) {
    std::vector<wchar_t> buffer(65536);
    OPENFILENAMEW dialog{};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = owner;
    const std::wstring title = i18n::Text(L"Attach files and folders");
    const std::wstring filter = std::wstring(i18n::Text(L"All files")) + L'\0' + L"*.*" + L'\0' + L'\0';
    dialog.lpstrTitle = title.c_str();
    dialog.lpstrFilter = filter.c_str();
    dialog.lpstrFile = buffer.data();
    dialog.nMaxFile = static_cast<DWORD>(buffer.size());
    dialog.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
                   OFN_ALLOWMULTISELECT | OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) {
        if (CommDlgExtendedError())
            return Result<std::vector<std::string>>::Failure(
                WideToUtf8(i18n::Text(L"The file picker failed.")));
        return Result<std::vector<std::string>>::Success({});
    }
    const std::wstring first(buffer.data());
    std::vector<std::string> paths;
    size_t offset = first.size() + 1;
    if (offset >= buffer.size() || buffer[offset] == 0) paths.push_back(WideToUtf8(first));
    else while (offset < buffer.size() && buffer[offset] && paths.size() < 64) {
        const std::wstring name(buffer.data() + offset);
        paths.push_back(JoinPath(WideToUtf8(first), WideToUtf8(name)));
        offset += name.size() + 1;
    }
    return Result<std::vector<std::string>>::Success(std::move(paths));
}
#else
namespace {
constexpr DWORD kAttachFolder = 1;

std::string ItemPath(IShellItem* item) {
    PWSTR path = nullptr;
    if (!item || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return {};
    const std::string value = WideToUtf8(path);
    CoTaskMemFree(path);
    return value;
}

class PickerEvents : public IFileDialogEvents, public IFileDialogControlEvents {
public:
    std::string folder;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** value) override {
        if (!value) return E_POINTER;
        *value = nullptr;
        if (iid == IID_IUnknown || iid == IID_IFileDialogEvents) *value = static_cast<IFileDialogEvents*>(this);
        else if (iid == IID_IFileDialogControlEvents) *value = static_cast<IFileDialogControlEvents*>(this);
        else return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG count = --references_;
        if (!count) delete this;
        return count;
    }
    HRESULT STDMETHODCALLTYPE OnFileOk(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChanging(IFileDialog*, IShellItem*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnFolderChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnSelectionChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE* value) override {
        *value = FDESVR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnTypeChange(IFileDialog*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE* value) override {
        *value = FDEOR_DEFAULT; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnItemSelected(IFileDialogCustomize*, DWORD, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnCheckButtonToggled(IFileDialogCustomize*, DWORD, BOOL) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnControlActivating(IFileDialogCustomize*, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnButtonClicked(IFileDialogCustomize* controls, DWORD id) override {
        if (id != kAttachFolder) return S_OK;
        IFileDialog* dialog = nullptr;
        if (FAILED(controls->QueryInterface(IID_PPV_ARGS(&dialog)))) return S_OK;
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetFolder(&item))) {
            folder = ItemPath(item);
            item->Release();
        }
        if (!folder.empty()) dialog->Close(S_OK);
        dialog->Release();
        return S_OK;
    }
private:
    ULONG references_ = 1;
};
}  // namespace

Result<std::vector<std::string>> PickAttachments(HWND owner) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    struct ComLifetime {
        HRESULT result;
        ~ComLifetime() { if (SUCCEEDED(result)) CoUninitialize(); }
    } com{initialized};
    if (FAILED(initialized)) {
        return Result<std::vector<std::string>>::Failure(WideToUtf8(i18n::Text(L"Cannot initialize the file picker.")));
    }
    IFileOpenDialog* dialog = nullptr;
    HRESULT status = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(status)) {
        return Result<std::vector<std::string>>::Failure(WideToUtf8(i18n::Text(L"Cannot open the file picker.")));
    }
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT | FOS_PATHMUSTEXIST);
    dialog->SetTitle(i18n::Text(L"Attach files and folders"));
    dialog->SetOkButtonLabel(i18n::Text(L"Attach"));
    const COMDLG_FILTERSPEC filters[] = {{i18n::Text(L"All files"), L"*.*"}};
    dialog->SetFileTypes(1, filters);
    auto* events = new PickerEvents;
    DWORD cookie = 0;
    IFileDialogCustomize* controls = nullptr;
    if (SUCCEEDED(dialog->QueryInterface(IID_PPV_ARGS(&controls)))) {
        if (SUCCEEDED(dialog->Advise(events, &cookie))) {
            controls->AddPushButton(kAttachFolder, i18n::Text(L"Attach current folder"));
        }
        controls->Release();
    }
    status = dialog->Show(owner);
    std::vector<std::string> paths;
    if (SUCCEEDED(status)) {
        if (!events->folder.empty()) paths.push_back(events->folder);
        else {
            IShellItemArray* items = nullptr;
            if (SUCCEEDED(dialog->GetResults(&items))) {
                DWORD count = 0;
                items->GetCount(&count);
                for (DWORD index = 0; index < count && paths.size() < 64; ++index) {
                    IShellItem* item = nullptr;
                    if (FAILED(items->GetItemAt(index, &item))) continue;
                    const std::string path = ItemPath(item);
                    item->Release();
                    if (!path.empty()) paths.push_back(path);
                }
                items->Release();
            }
        }
    }
    if (cookie) dialog->Unadvise(cookie);
    events->Release();
    dialog->Release();
    if (FAILED(status) && status != HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        return Result<std::vector<std::string>>::Failure(WideToUtf8(i18n::Text(L"The file picker failed.")));
    }
    return Result<std::vector<std::string>>::Success(std::move(paths));
}
#endif

}  // namespace spark
