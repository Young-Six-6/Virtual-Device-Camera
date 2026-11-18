#pragma once
#include <Windows.h>
#include <shellapi.h>
#include <atomic>
#include <mutex>

namespace vcam {
class TrayController {
    static constexpr UINT tray_message_ = WM_APP + 41;
    static constexpr UINT toggle_command_ = 1;
    static constexpr UINT exit_command_ = 2;
    inline static const wchar_t* class_name_ = L"VirtualDeviceCameraTrayWindow";
    HWND window_ = nullptr;
    HANDLE stop_event_ = nullptr;
    NOTIFYICONDATAW icon_{};
    UINT taskbar_created_ = 0;
    mutable std::mutex mutex_;
    std::atomic<bool> hidden_{false};

    static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        TrayController* self = reinterpret_cast<TrayController*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<TrayController*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wparam, lparam);
        if (message == self->taskbar_created_ && self->hidden_.load()) {
            std::lock_guard<std::mutex> lock(self->mutex_);
            self->add_icon();
            return 0;
        }
        if (message == tray_message_) {
            if (lparam == WM_RBUTTONUP || lparam == WM_CONTEXTMENU) self->show_menu();
            else if (lparam == WM_LBUTTONDBLCLK) self->set_hidden(!self->hidden_.load());
            return 0;
        }
        if (message == WM_DESTROY) {
            if (self->hidden_.load()) Shell_NotifyIconW(NIM_DELETE, &self->icon_);
            return 0;
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }

    bool add_icon() {
        icon_.cbSize = sizeof(icon_);
        icon_.hWnd = window_;
        icon_.uID = 1;
        icon_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        icon_.uCallbackMessage = tray_message_;
        icon_.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512));
        wcscpy_s(icon_.szTip, L"Virtual Device Camera");
        if (Shell_NotifyIconW(NIM_ADD, &icon_)) return true;
        return Shell_NotifyIconW(NIM_MODIFY, &icon_) != FALSE;
    }

    void show_menu() {
        HMENU menu = CreatePopupMenu();
        if (!menu) return;
        const wchar_t* label = hidden_.load() ? L"显示控制台" : L"隐藏到托盘";
        AppendMenuW(menu, MF_STRING, toggle_command_, label);
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, exit_command_, L"退出");
        POINT cursor{};
        GetCursorPos(&cursor);
        SetForegroundWindow(window_);
        const UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD,
            cursor.x, cursor.y, 0, window_, nullptr);
        DestroyMenu(menu);
        if (command == toggle_command_) set_hidden(!hidden_.load());
        else if (command == exit_command_) {
            set_hidden(false);
            if (stop_event_) SetEvent(stop_event_);
        }
        PostMessageW(window_, WM_NULL, 0, 0);
    }

public:
    explicit TrayController(HANDLE stop_event) : stop_event_(stop_event) {}
    TrayController(const TrayController&) = delete;
    TrayController& operator=(const TrayController&) = delete;
    ~TrayController() {
        if (window_) DestroyWindow(window_);
    }

    bool initialize() {
        taskbar_created_ = RegisterWindowMessageW(L"TaskbarCreated");
        WNDCLASSEXW klass{};
        klass.cbSize = sizeof(klass);
        klass.lpfnWndProc = &TrayController::window_proc;
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpszClassName = class_name_;
        if (!RegisterClassExW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        window_ = CreateWindowExW(0, class_name_, L"Virtual Device Camera tray controller", 0,
            0, 0, 0, 0, nullptr, nullptr, klass.hInstance, this);
        return window_ != nullptr;
    }

    bool hidden() const { return hidden_.load(); }

    bool set_hidden(bool hide) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (hide == hidden_.load()) return true;
        const HWND console = GetConsoleWindow();
        if (hide) {
            if (!window_ || !add_icon()) return false;
            if (console) ShowWindow(console, SW_HIDE);
            hidden_.store(true);
        } else {
            if (console) {
                ShowWindow(console, SW_SHOW);
                SetForegroundWindow(console);
            }
            if (window_) Shell_NotifyIconW(NIM_DELETE, &icon_);
            hidden_.store(false);
        }
        return true;
    }
};
}
