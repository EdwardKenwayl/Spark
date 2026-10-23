#include "gui/native/app.h"
#include "platform/user_directories.h"

#include <wx/app.h>
#include <wx/image.h>
#include <wx/msgdlg.h>
#include <wx/timer.h>
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace spark {
namespace {

class NativeApplication : public wxApp {
public:
    bool OnInit() override {
        wxInitAllImageHandlers();
        SetAppName("Spark");
        std::string path;
        for (int index = 1; index < argc; ++index) {
            if (wxString(argv[index]) == "--config" && index + 1 < argc)
                path = wxString(argv[++index]).ToUTF8().data();
        }
        const std::string portable = JoinPath(GetExecutableDirectory(), "config.json");
        if (path.empty()) path = PathExists(portable) ? portable :
            JoinPath(JoinPath(UserDirectory(UserDirectoryKind::AppData), "Spark"), "config.json");
        path = AbsolutePath(path);
        std::error_code error;
        spark::fs::create_directories(NativePath(ParentPath(path)), error);
        if (error) return Fail(error.message());
        const auto directory = NativePath(ParentPath(path));
        const auto lock = directory / ".spark-instance.lock";
        instance_ = ::open(lock.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (instance_ < 0) return Fail("Could not open the single-instance lock.");
        struct stat info{};
        if (::fstat(instance_, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid())
            return Fail("Invalid single-instance lock.");
        activation_ = JoinPath(ParentPath(path), ".spark-activate");
        if (::flock(instance_, LOCK_EX | LOCK_NB) != 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                WriteFileUtf8Atomic(activation_, NewSessionId());
                return false;
            }
            return Fail("Could not acquire the single-instance lock.");
        }
        const auto config = LoadConfig(path, true);
        if (!config.ok) return Fail(config.error);
        frame_ = new NativeFrame(config.value);
        frame_->Bind(wxEVT_DESTROY, [this](wxWindowDestroyEvent& event) {
            if (event.GetEventObject() == frame_) {
                frame_ = nullptr;
                if (activation_timer_) activation_timer_->Stop();
            }
            event.Skip();
        });
        SetTopWindow(frame_);
        frame_->Show();
        activation_timer_ = std::make_unique<wxTimer>(this);
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!frame_ || !PathExists(activation_)) return;
            std::error_code ignored;
            spark::fs::remove(NativePath(activation_), ignored);
            frame_->RestoreWindow();
        });
        activation_timer_->Start(250);
        return true;
    }
    int OnExit() override {
        if (activation_timer_) activation_timer_->Stop();
        if (instance_ >= 0) ::close(instance_);
        return 0;
    }
private:
    bool Fail(const std::string& text) {
        wxMessageBox(wxString::FromUTF8(text), "Spark", wxOK | wxICON_ERROR);
        if (instance_ >= 0) { ::close(instance_); instance_ = -1; }
        return false;
    }
    NativeFrame* frame_ = nullptr;
    std::unique_ptr<wxTimer> activation_timer_;
    std::string activation_;
    int instance_ = -1;
};

}  // namespace
}  // namespace spark

wxIMPLEMENT_APP(spark::NativeApplication);
