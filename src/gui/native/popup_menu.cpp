#include "gui/native/popup_menu.h"
#include "gui/native/controls.h"

#include <wx/dcbuffer.h>
#include <wx/dcclient.h>
#include <wx/display.h>
#include <wx/evtloop.h>
#include <wx/image.h>
#include <wx/menu.h>
#include <wx/menuitem.h>
#include <wx/nonownedwnd.h>
#include <wx/popupwin.h>
#include <wx/thread.h>
#include <wx/timer.h>
#include <wx/utils.h>
#include <wx/weakref.h>
#include <algorithm>
#include <memory>
#include <utility>
#include <vector>

namespace spark {
namespace {
constexpr size_t kMaxEntries = 4096;
constexpr size_t kMaxLabelChars = 512;
constexpr size_t kMaxShortcutChars = 128;
constexpr int kMaxDepth = 16;
struct Entry {
    int id;
    wxString label, shortcut;
    wxBitmap bitmap;
    bool enabled, separator, checked, radio, submenu;
    std::vector<Entry> children;
};
std::vector<Entry> Snapshot(wxMenu& menu, wxWindow* owner, size_t& remaining, int depth = 0) {
    std::vector<Entry> entries;
    for (auto* item : menu.GetMenuItems()) {
        if (remaining == 0) break;
        --remaining;
        const auto text = item->GetItemLabel();
        // Bound before stripping mnemonic markers; escaped ampersands need two characters.
        const auto prefix = text.Left(2 * kMaxLabelChars + 1);
        const auto tab = prefix.find('\t');
        Entry entry{item->GetId(), wxMenuItem::GetLabelText(prefix).Left(kMaxLabelChars),
                    tab != wxString::npos ? text.Mid(tab + 1, kMaxShortcutChars) : wxString{}, {},
                    item->IsEnabled(), item->IsSeparator(), item->IsCheckable() && item->IsChecked(),
                    item->IsRadio(), item->GetSubMenu() != nullptr, {}};
        entry.label.Replace("\r", " ");
        entry.label.Replace("\n", " ");
        entry.shortcut.Replace("\r", " ");
        entry.shortcut.Replace("\n", " ");
        if (entry.submenu) {
            if (depth + 1 < kMaxDepth)
                entry.children = Snapshot(*item->GetSubMenu(), owner, remaining, depth + 1);
            if (entry.children.empty()) entry.enabled = false;
        }
        const auto bitmap = item->GetBitmap();
        if (bitmap.IsOk()) {
            auto image = bitmap.ConvertToImage();
            const double scale = std::min(1.0, static_cast<double>(owner->FromDIP(16)) /
                                               std::max(image.GetWidth(), image.GetHeight()));
            image.Rescale(std::max(1, static_cast<int>(image.GetWidth() * scale)),
                          std::max(1, static_cast<int>(image.GetHeight() * scale)),
                          wxIMAGE_QUALITY_HIGH);
            entry.bitmap = wxBitmap(entry.enabled ? image : image.ConvertToGreyscale());
        }
        entries.push_back(std::move(entry));
    }
    return entries;
}
struct Session {
    wxEventLoop* loop = nullptr;
    bool running = false, done = false;
    int result = wxID_NONE;
    void Finish(int id) {
        if (done) return;
        done = true;
        result = id;
        if (running && loop) loop->ScheduleExit();
    }
};
struct Page {
    const std::vector<Entry>* entries;
    wxRect rect;
    int width = 0, total = 0, selected = -1, scroll = 0;
    bool icons = false;
};
class MenuPopup final : public wxPopupTransientWindow {
public:
    MenuPopup(wxWindow* owner, std::vector<Entry> entries, wxPoint anchor,
              wxRect work, NativeMenuAlignment alignment, std::shared_ptr<Session> session)
        : wxPopupTransientWindow(owner, wxBORDER_NONE | wxFRAME_SHAPED), owner_(owner),
          entries_(std::move(entries)), work_(work), anchor_(anchor),
          session_(std::move(session)), hover_(this),
          colors_(NativeColors(owner->GetForegroundColour().Red() > 180)) {
        SetName("spark.menu.popup");
        canvas_ = new wxPanel(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxWANTS_CHARS);
        canvas_->SetName("spark.menu.canvas");
        canvas_->SetFont(owner->GetFont());
        canvas_->SetBackgroundStyle(wxBG_STYLE_PAINT);
        canvas_->SetBackgroundColour(colors_.panel);
        canvas_->SetForegroundColour(colors_.text);
        canvas_->Bind(wxEVT_PAINT, &MenuPopup::Paint, this);
        canvas_->Bind(wxEVT_MOTION, &MenuPopup::Motion, this);
        canvas_->Bind(wxEVT_LEFT_DOWN, &MenuPopup::Press, this);
        canvas_->Bind(wxEVT_LEFT_UP, &MenuPopup::Release, this);
        canvas_->Bind(wxEVT_MOUSEWHEEL, &MenuPopup::Wheel, this);
        canvas_->Bind(wxEVT_KEY_DOWN, &MenuPopup::Key, this);
        canvas_->Bind(wxEVT_RIGHT_DOWN, [this](wxMouseEvent&) { Stop(wxID_NONE); });
        canvas_->Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent&) {
            hover_.Stop();
            if (!session_->done && !pages_.empty()) Select(pages_.size() - 1, -1);
        });
        Bind(wxEVT_KEY_DOWN, &MenuPopup::Key, this);
        Bind(wxEVT_CHAR, [](wxKeyEvent&) {});
        Bind(wxEVT_TIMER, [this](wxTimerEvent&) {
            if (!session_->done && !pages_.empty()) Open(pages_.size() - 1, false);
        });
        owner->Bind(wxEVT_DESTROY, &MenuPopup::OwnerDestroyed, this);
        Push(entries_);
        const int x = std::clamp(anchor_.x, work_.x, work_.GetRight() + 1 - pages_[0].width);
        leftward_ = work_.GetRight() + 1 - x - pages_[0].width < x - work_.x;
        anchor_.x = x;
        if (alignment == NativeMenuAlignment::Bottom)
            anchor_.y -= std::min({Dip(480), work_.height, pages_[0].total + 2});
        Reflow();
    }
    ~MenuPopup() override {
        if (owner_) owner_->Unbind(wxEVT_DESTROY, &MenuPopup::OwnerDestroyed, this);
        session_->Finish(wxID_NONE);
    }
    wxWindow* Canvas() const { return canvas_; }
    bool ProcessLeftDown(wxMouseEvent& event) override { Press(event); return true; }
private:
    int Dip(int value) const { return canvas_->FromDIP(value); }
    int Height(const Entry& entry) const { return Dip(entry.separator ? 9 : 36); }
    int Back(size_t page) const { return page == first_ && first_ != 0 ? Dip(24) : 0; }
    int Band(size_t page) const {
        return pages_[page].total > pages_[page].rect.height - 2 - Back(page) ? Dip(18) : 0;
    }
    wxRect Body(size_t page) const {
        auto rect = pages_[page].rect;
        rect.Deflate(1);
        rect.y += Back(page) + Band(page);
        rect.height = std::max(1, rect.height - Back(page) - 2 * Band(page));
        return rect;
    }
    int Top(const Page& page, int row) const {
        int top = 0;
        for (int i = 0; i < row; ++i) top += Height((*page.entries)[i]);
        return top;
    }
    wxPoint Point(const wxMouseEvent& event) const {
        auto* source = wxDynamicCast(event.GetEventObject(), wxWindow);
        return canvas_->ScreenToClient(source ? source->ClientToScreen(event.GetPosition()) :
                                               wxGetMousePosition());
    }
    void Push(const std::vector<Entry>& entries) {
        Page page{&entries, {}};
        wxClientDC dc(canvas_);
        dc.SetFont(canvas_->GetFont());
        int width = Dip(220);
        for (const auto& entry : entries) page.icons |= entry.bitmap.IsOk();
        for (const auto& entry : entries) {
            page.total += Height(entry);
            width = std::max(width, dc.GetTextExtent(entry.label).x +
                (entry.shortcut.empty() ? 0 : dc.GetTextExtent(entry.shortcut).x + Dip(24)) +
                Dip((entry.submenu ? 68 : 52) + (page.icons ? 24 : 0)));
        }
        page.width = std::min({width, Dip(420), work_.width});
        pages_.push_back(page);
    }
    void Reflow() {
        int width = 0;
        for (const auto& page : pages_) width += page.width;
        first_ = 0;
        while (width > work_.width && first_ + 1 < pages_.size()) width -= pages_[first_++].width;
        const int limit = std::min(Dip(480), work_.height);
        int used = 0, height = 0;
        for (size_t i = first_; i < pages_.size(); ++i) {
            auto& page = pages_[i];
            const int h = std::min(limit, page.total + 2 + Back(i));
            int y = 0;
            if (i > first_) {
                const auto& parent = pages_[i - 1];
                y = std::clamp(Body(i - 1).y + Top(parent, std::max(0, parent.selected)) -
                               parent.scroll, 0, limit - h);
            }
            page.rect = wxRect(leftward_ ? width - used - page.width : used, y, page.width, h);
            page.scroll = std::clamp(page.scroll, 0, std::max(0, page.total - Body(i).height));
            used += page.width;
            height = std::max(height, y + h);
        }
        const int x = leftward_ ? anchor_.x + pages_[0].width - width : anchor_.x;
        SetSize(std::clamp(x, work_.x, work_.GetRight() + 1 - width),
                std::clamp(anchor_.y, work_.y, work_.GetBottom() + 1 - height), width, height);
        ApplyRoundedPopupShape(this, 8);
        canvas_->SetSize(GetClientRect());
        canvas_->Refresh();
    }
    // Negative rows denote padding (-1), scroll up/down (-2/-3), or back (-4).
    std::pair<int, int> Hit(wxPoint point) const {
        for (size_t i = first_; i < pages_.size(); ++i) {
            const auto& page = pages_[i];
            if (!page.rect.Contains(point)) continue;
            const int index = static_cast<int>(i);
            if (Back(i) && point.y < page.rect.y + 1 + Back(i)) return {index, -4};
            const auto body = Body(i);
            if (Band(i) && point.y < body.y) return {index, -2};
            if (Band(i) && point.y >= body.y + body.height) return {index, -3};
            if (!body.Contains(point)) return {index, -1};
            int y = body.y - page.scroll;
            for (size_t row = 0; row < page.entries->size(); ++row) {
                y += Height((*page.entries)[row]);
                if (point.y < y) return {index, static_cast<int>(row)};
            }
            return {index, -1};
        }
        return {-1, -1};
    }
    void Select(size_t page, int row) {
        hover_.Stop();
        pages_.resize(page + 1);
        pages_[page].selected = row;
        if (row >= 0) {
            const int top = Top(pages_[page], row);
            auto& scroll = pages_[page].scroll;
            const int visible = Body(page).height;
            if (top < scroll) scroll = top;
            else if (top + Height((*pages_[page].entries)[row]) > scroll + visible)
                scroll = top + Height((*pages_[page].entries)[row]) - visible;
        }
        Reflow();
    }
    void Open(size_t page, bool keyboard) {
        if (session_->done) return;
        const int row = pages_[page].selected;
        if (row < 0) return;
        const auto& entry = (*pages_[page].entries)[row];
        if (!entry.enabled || !entry.submenu || entry.children.empty()) return;
        hover_.Stop();
        pages_.resize(page + 1);
        Push(entry.children);
        Reflow();
        if (keyboard) Move(1);
    }
    void Move(int direction) {
        auto& page = pages_.back();
        const int count = static_cast<int>(page.entries->size());
        int row = page.selected;
        if (row < 0) row = direction > 0 ? -1 : 0;
        for (int i = 0; i < count; ++i) {
            row = (row + direction + count) % count;
            const auto& entry = (*page.entries)[row];
            if (!entry.separator && entry.enabled) { Select(pages_.size() - 1, row); return; }
        }
    }
    void Scroll(size_t page, int delta) {
        hover_.Stop();
        pressed_ = {-1, -1};
        pages_.resize(page + 1);
        auto& current = pages_[page];
        current.scroll = std::clamp(current.scroll + delta, 0,
                                   std::max(0, current.total - Body(page).height));
        Reflow();
    }
    void Motion(wxMouseEvent& event) {
        if (session_->done) return;
        const auto hit = Hit(Point(event));
        if (hit.first < 0 || hit.second < 0) {
            hover_.Stop();
            if (hit.second != -4)
                Select(hit.first < 0 ? pages_.size() - 1 : static_cast<size_t>(hit.first), -1);
            return;
        }
        const auto& entry = (*pages_[hit.first].entries)[hit.second];
        if (!entry.enabled || entry.separator) { Select(hit.first, -1); return; }
        if (pages_[hit.first].selected == hit.second) {
            if (entry.submenu && pages_.size() == static_cast<size_t>(hit.first + 1) && !hover_.IsRunning())
                hover_.StartOnce(180);
            return;
        }
        Select(hit.first, hit.second);
        if (entry.submenu) hover_.StartOnce(180);
    }
    void Press(wxMouseEvent& event) {
        if (session_->done) return;
        hover_.Stop();
        pressed_ = Hit(Point(event));
        const auto hit = pressed_;
        if (hit.first < 0) { Stop(wxID_NONE); return; }
        if (hit.second == -4) { pages_.resize(first_); Reflow(); return; }
        if (hit.second == -2 || hit.second == -3) {
            Scroll(hit.first, Dip(hit.second == -2 ? -36 : 36)); return;
        }
        if (hit.second >= 0) {
            const auto& entry = (*pages_[hit.first].entries)[hit.second];
            if (entry.enabled && !entry.separator) Select(hit.first, hit.second);
        }
    }
    void Release(wxMouseEvent& event) {
        if (session_->done) return;
        const auto hit = Hit(Point(event));
        const auto pressed = pressed_;
        pressed_ = {-1, -1};
        if (hit != pressed || hit.first < 0 || hit.second < 0) return;
        const auto& entry = (*pages_[hit.first].entries)[hit.second];
        if (!entry.enabled || entry.separator) return;
        if (entry.submenu) Open(hit.first, false);
        else Stop(entry.id);
    }
    void Wheel(wxMouseEvent& event) {
        if (session_->done || event.GetWheelDelta() == 0) return;
        const auto hit = Hit(Point(event));
        if (hit.first >= 0) Scroll(hit.first, -event.GetWheelRotation() * Dip(36) / event.GetWheelDelta());
    }
    void Key(wxKeyEvent& event) {
        if (session_->done) return;
        hover_.Stop();
        pressed_ = {-1, -1};
        switch (event.GetKeyCode()) {
            case WXK_ESCAPE: Stop(wxID_NONE); break;
            case WXK_UP: Move(-1); break;
            case WXK_DOWN: Move(1); break;
            case WXK_LEFT:
                if (pages_.size() > 1) { pages_.pop_back(); Reflow(); }
                break;
            case WXK_RIGHT: Open(pages_.size() - 1, true); break;
            case WXK_RETURN:
            case WXK_NUMPAD_ENTER: {
                const auto& page = pages_.back();
                if (page.selected < 0) { Move(1); break; }
                const auto& entry = (*page.entries)[page.selected];
                if (entry.enabled && !entry.separator) {
                    if (entry.submenu) Open(pages_.size() - 1, true);
                    else Stop(entry.id);
                }
                break;
            }
            default: break;
        }
    }
    void Paint(wxPaintEvent&) {
        wxAutoBufferedPaintDC dc(canvas_);
        dc.SetBackground(wxBrush(colors_.panel)); dc.Clear();
        dc.SetFont(canvas_->GetFont());
        const auto chevron = [&](int x, int y, int direction, bool vertical) {
            const int d = Dip(3);
            dc.DrawLine(x - (vertical ? d : direction * d), y - (vertical ? direction * d : d), x, y);
            dc.DrawLine(x, y, x + (vertical ? d : -direction * d), y + (vertical ? -direction * d : d));
        };
        for (size_t i = first_; i < pages_.size(); ++i) {
            const auto& page = pages_[i];
            dc.SetPen(wxPen(colors_.border)); dc.SetBrush(wxBrush(colors_.panel));
            dc.DrawRectangle(page.rect);
            const auto body = Body(i);
            if (Back(i)) {
                dc.SetPen(wxPen(colors_.text));
                chevron(page.rect.x + Dip(16), page.rect.y + 1 + Back(i) / 2, -1, false);
            }
            if (Band(i)) {
                dc.SetPen(wxPen(page.scroll > 0 ? colors_.text : colors_.muted));
                chevron(page.rect.x + page.rect.width / 2, body.y - Band(i) / 2, -1, true);
                dc.SetPen(wxPen(page.scroll + body.height < page.total ? colors_.text : colors_.muted));
                chevron(page.rect.x + page.rect.width / 2, body.y + body.height + Band(i) / 2, 1, true);
            }
            dc.SetClippingRegion(body);
            int y = body.y - page.scroll;
            for (size_t row = 0; row < page.entries->size(); ++row) {
                const auto& entry = (*page.entries)[row];
                const int h = Height(entry), middle = y + h / 2;
                const wxRect rect(body.x, y, body.width, h);
                y += h;
                if (!body.Intersects(rect)) continue;
                if (entry.separator) {
                    dc.SetPen(wxPen(colors_.border));
                    dc.DrawLine(body.x + Dip(12), middle, body.GetRight() - Dip(12), middle);
                    continue;
                }
                if (entry.enabled && static_cast<int>(row) == page.selected) {
                    dc.SetPen(*wxTRANSPARENT_PEN); dc.SetBrush(wxBrush(colors_.selection));
                    dc.DrawRoundedRectangle(wxRect(rect).Deflate(Dip(3), Dip(1)), Dip(4));
                }
                const auto color = entry.enabled ? colors_.text : colors_.muted;
                dc.SetPen(wxPen(color, std::max(1, Dip(1)))); dc.SetBrush(wxBrush(color));
                if (entry.checked) {
                    const int x = body.x + Dip(12);
                    if (entry.radio) dc.DrawCircle(x, middle, Dip(3));
                    else {
                        dc.DrawLine(x - Dip(3), middle, x, middle + Dip(4));
                        dc.DrawLine(x, middle + Dip(4), x + Dip(6), middle - Dip(5));
                    }
                }
                if (entry.bitmap.IsOk())
                    dc.DrawBitmap(entry.bitmap, body.x + Dip(28),
                                  middle - entry.bitmap.GetHeight() / 2, true);
                if (entry.submenu) chevron(body.GetRight() - Dip(16), middle, 1, false);
                const int left = body.x + Dip(28 + (page.icons ? 24 : 0));
                int right = body.GetRight() - Dip(entry.submenu ? 28 : 12);
                dc.SetTextForeground(color);
                if (!entry.shortcut.empty() && right > left) {
                    const auto shortcut = wxControl::Ellipsize(entry.shortcut, dc, wxELLIPSIZE_END,
                                                               std::max(1, (right - left) / 2));
                    const auto size = dc.GetTextExtent(shortcut);
                    dc.DrawText(shortcut, right - size.x, middle - size.y / 2);
                    right -= size.x + Dip(24);
                }
                if (right > left) {
                    const auto label = wxControl::Ellipsize(entry.label, dc, wxELLIPSIZE_END, right - left);
                    dc.DrawText(label, left, middle - dc.GetTextExtent(label).y / 2);
                }
            }
            dc.DestroyClippingRegion();
        }
    }
    void Stop(int id) {
        hover_.Stop();
        session_->Finish(id);
        Dismiss();
    }
    void OnDismiss() override { hover_.Stop(); session_->Finish(wxID_NONE); }
    void OwnerDestroyed(wxWindowDestroyEvent& event) {
        if (event.GetEventObject() == owner_.get()) Stop(wxID_NONE);
        event.Skip();
    }
    wxWeakRef<wxWindow> owner_;
    std::vector<Entry> entries_;
    std::vector<Page> pages_;
    wxRect work_;
    wxPoint anchor_;
    std::shared_ptr<Session> session_;
    wxTimer hover_;
    NativePalette colors_;
    wxPanel* canvas_ = nullptr;
    size_t first_ = 0;
    bool leftward_ = false;
    std::pair<int, int> pressed_{-1, -1};
};
}  // namespace

int ShowNativeMenu(wxWindow* owner, wxMenu& menu, const wxPoint& screen_anchor,
                   NativeMenuAlignment alignment) {
    if (!wxIsMainThread() || !owner || owner->IsBeingDeleted() || menu.GetMenuItemCount() == 0)
        return wxID_NONE;
    const wxPoint anchor = screen_anchor == wxDefaultPosition ? wxGetMousePosition() : screen_anchor;
    int display = wxDisplay::GetFromPoint(anchor);
    if (display == wxNOT_FOUND) display = wxDisplay::GetFromWindow(owner);
    if (display == wxNOT_FOUND) return wxID_NONE;
    wxRect work = wxDisplay(display).GetClientArea();
    work.Deflate(std::min(owner->FromDIP(8), std::min(work.width, work.height) / 4));
    if (work.width < owner->FromDIP(80) || work.height < owner->FromDIP(80)) return wxID_NONE;
    wxWeakRef<wxWindow> focus(wxWindow::FindFocus());
    wxWeakRef<wxWindow> owner_guard(owner);
    auto session = std::make_shared<Session>();
    wxEventLoop loop;
    session->loop = &loop;
    size_t remaining = kMaxEntries;
    auto* popup = new MenuPopup(owner, Snapshot(menu, owner, remaining), anchor, work, alignment, session);
    wxWeakRef<MenuPopup> guard(popup);
    {
        struct Cleanup {
            wxWeakRef<MenuPopup> popup;
            std::shared_ptr<Session> session;
            ~Cleanup() {
                session->running = false;
                session->loop = nullptr;
                session->Finish(wxID_NONE);
                if (popup && !popup->IsBeingDeleted()) { popup->Dismiss(); popup->Destroy(); }
            }
        } cleanup{guard, session};
        popup->Popup(popup->Canvas());
        if (!session->done) {
            session->running = true;
            loop.Run();
        }
    }
    if (owner_guard && !owner_guard->IsBeingDeleted() && focus && !focus->IsBeingDeleted() &&
        focus->IsShownOnScreen() && focus->IsEnabled() &&
        wxGetActiveWindow() == wxGetTopLevelParent(owner_guard.get()))
        focus->SetFocus();
    return session->result;
}

}  // namespace spark
