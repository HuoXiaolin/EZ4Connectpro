#include <winsock2.h>
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <gdiplus.h>
#include <cstdio>
#include <string>
#include <vector>

std::wstring objectName(HANDLE handle) {
    if (!handle) return {};
    wchar_t buffer[256]{};
    if (!GetUserObjectInformationW(handle, UOI_NAME, buffer, sizeof(buffer), nullptr)) return {};
    return buffer;
}
std::wstring inputName() {
    HDESK input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
    const auto name = objectName(input);
    if (input) CloseDesktop(input);
    return name;
}
struct WindowSearch { DWORD pid; HWND hwnd = nullptr; std::wstring title = L"EZ4Connectpro"; bool includeHidden = false; };
BOOL CALLBACK findWindow(HWND hwnd, LPARAM value) {
    auto &search = *reinterpret_cast<WindowSearch *>(value);
    wchar_t title[256]{};
    GetWindowTextW(hwnd, title, 256);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const bool matchingTitle = std::wstring(title) == search.title ||
        (search.title == L"EZ4Connectpro" && std::wstring(title) == L"\u4e3b\u754c\u9762 - EZ4Connectpro");
    if ((search.includeHidden || IsWindowVisible(hwnd)) && matchingTitle && (!search.pid || search.pid == pid)) {
        search.hwnd = hwnd;
        return FALSE;
    }
    return TRUE;
}
struct AuxiliaryWindow { HWND hwnd = nullptr; bool tray = false; };
BOOL CALLBACK findAuxiliaryWindow(HWND hwnd, LPARAM value) {
    auto &search = *reinterpret_cast<AuxiliaryWindow *>(value);
    wchar_t name[256]{};
    if (search.tray) {
        GetWindowTextW(hwnd, name, 256);
        if (std::wstring(name) != L"QTrayIconMessageWindow") return TRUE;
    } else {
        GetClassNameW(hwnd, name, 256);
        if (!IsWindowVisible(hwnd) || std::wstring(name).find(L"Popup") == std::wstring::npos) return TRUE;
    }
    search.hwnd = hwnd;
    return FALSE;
}
HWND auxiliaryWindow(HWND app, bool tray) {
    AuxiliaryWindow search{nullptr, tray};
    EnumThreadWindows(GetWindowThreadProcessId(app, nullptr), findAuxiliaryWindow, reinterpret_cast<LPARAM>(&search));
    return search.hwnd;
}
bool isAbove(HWND app, HWND cover) {
    for (HWND previous = GetWindow(cover, GW_HWNDPREV); previous; previous = GetWindow(previous, GW_HWNDPREV)) {
        if (previous == app) return true;
    }
    return false;
}
bool requestTrayExit(HWND app) {
    HWND tray = auxiliaryWindow(app, true);
    if (!tray || !PostMessageW(tray, WM_APP + 101, MAKELPARAM(80, 80), WM_CONTEXTMENU)) return false;
    Sleep(200);
    HWND menu = auxiliaryWindow(app, false);
    if (!menu) return false;
    RECT client{};
    if (!GetClientRect(menu, &client)) return false;
    // The final tray action is Exit. Mouse messages also work on the isolated
    // desktop, where the popup cannot take keyboard focus from the input desktop.
    const int inset = MulDiv(14, GetDpiForWindow(menu), 96);
    const LPARAM point = MAKELPARAM((client.right - client.left) / 2, client.bottom - inset);
    return PostMessageW(menu, WM_MOUSEMOVE, 0, point)
        && PostMessageW(menu, WM_LBUTTONDOWN, MK_LBUTTON, point)
        && PostMessageW(menu, WM_LBUTTONUP, 0, point);
}
bool testCloseToTray(HWND app, HANDLE process) {
    HWND tray = auxiliaryWindow(app, true);
    if (!tray) return false;
    PostMessageW(app, WM_SYSCOMMAND, SC_CLOSE, 0);
    Sleep(250);
    const bool hiddenAndAlive = !IsWindowVisible(app) && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    PostMessageW(tray, WM_APP + 101, 0, NIN_SELECT);
    Sleep(250);
    const bool restored = IsWindowVisible(app) && !IsIconic(app);
    std::printf("Close to tray: hidden-and-alive=%d restored-by-tray=%d\n", hiddenAndAlive, restored);
    return hiddenAndAlive && restored;
}
bool testTray(HWND app) {
    HWND tray = auxiliaryWindow(app, true);
    if (!tray) {
        std::printf("Qt tray callback window not found\n");
        return false;
    }
    RECT bounds{};
    GetWindowRect(app, &bounds);
    HWND cover = CreateWindowExW(0, L"STATIC", L"EZ4Connectpro QA cover", WS_OVERLAPPEDWINDOW,
        bounds.left, bounds.top, 500, 400, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!cover) return false;
    ShowWindow(cover, SW_SHOWNOACTIVATE);
    SetWindowPos(cover, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const bool covered = isAbove(cover, app);
    const auto restore = [&](UINT reason) {
        PostMessageW(tray, WM_APP + 101, 0, reason); // Qt 6.10.3 notification-area callback.
        Sleep(350);
        return IsWindowVisible(app) && !IsIconic(app) && isAbove(app, cover)
            && !(GetWindowLongPtrW(app, GWL_EXSTYLE) & WS_EX_TOPMOST);
    };
    const bool raised = covered && restore(NIN_SELECT);
    PostMessageW(app, WM_SYSCOMMAND, SC_MAXIMIZE, 0);
    Sleep(350);
    const bool maximized = IsZoomed(app);
    PostMessageW(app, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    Sleep(350);
    const bool hidden = !IsWindowVisible(app) || IsIconic(app);
    const bool restored = restore(NIN_SELECT) && IsZoomed(app);
    PostMessageW(app, WM_SYSCOMMAND, SC_RESTORE, 0);
    Sleep(250);
    PostMessageW(app, WM_SYSCOMMAND, SC_MINIMIZE, 0);
    Sleep(350);
    const bool messageRestored = restore(NIN_BALLOONUSERCLICK) && !IsZoomed(app);
    std::printf("Tray callbacks: raised=%d maximized=%d minimized=%d restored=%d notification=%d focus-on-input-desktop=%d\n",
        raised, maximized, hidden, restored, messageRestored, GetForegroundWindow() == app);
    DestroyWindow(cover);
    return raised && maximized && hidden && restored && messageRestored;
}
std::wstring processImage(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t path[32768]{};
    DWORD length = 32768;
    const bool found = QueryFullProcessImageNameW(process, 0, path, &length);
    CloseHandle(process);
    return found ? std::wstring(path, length) : std::wstring{};
}
std::wstring description(const std::wstring &path) {
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (!size) return {};
    std::vector<BYTE> info(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, info.data())) return {};
    wchar_t *value = nullptr;
    UINT length = 0;
    if (!VerQueryValueW(info.data(), L"\\StringFileInfo\\040904B0\\FileDescription", reinterpret_cast<void **>(&value), &length)) return {};
    return value ? value : L"";
}
bool testProcessTree(HWND app, HANDLE bootstrap, const wchar_t *launcher) {
    DWORD appPid = 0;
    GetWindowThreadProcessId(app, &appPid);
    const bool bootstrapExited = WaitForSingleObject(bootstrap, 3000) == WAIT_OBJECT_0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int workers = 0;
    if (snapshot != INVALID_HANDLE_VALUE) {
        if (Process32FirstW(snapshot, &entry)) do {
            if (entry.th32ParentProcessID != appPid) continue;
            const auto path = processImage(entry.th32ProcessID);
            if (!_wcsicmp(path.c_str(), launcher)) ++workers;
        } while (Process32NextW(snapshot, &entry));
        CloseHandle(snapshot);
    }
    const auto appPath = processImage(appPid);
    const auto corePath = appPath.substr(0, appPath.find_last_of(L"\\/")) + L"\\zju-connect.exe";
    const bool roles = description(appPath) == L"EZ4Connectpro" &&
        description(launcher) == L"EZ4Connectpro \u00b7 \u8fd0\u884c\u6587\u4ef6\u7ba1\u7406" &&
        description(corePath) == L"EZ4Connectpro \u00b7 VPN \u8fde\u63a5\u6838\u5fc3";
    std::printf("Process ownership: bootstrap-exited=%d UI-pid=%lu cleanup-direct-children=%d functional-metadata=%d\n",
        bootstrapExited, appPid, workers, roles);
    return bootstrapExited && workers == 1 && roles;
}
bool capture(HWND window, const wchar_t *path) {
    RECT bounds{};
    GetWindowRect(window, &bounds);
    const int width = bounds.right - bounds.left;
    const int height = bounds.bottom - bounds.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ original = SelectObject(memory, bitmap);
    const bool drawn = PrintWindow(window, memory, 2);
    SelectObject(memory, original);
    bool saved = false;
    if (drawn) {
        Gdiplus::GdiplusStartupInput options;
        ULONG_PTR token = 0;
        if (Gdiplus::GdiplusStartup(&token, &options, nullptr) == Gdiplus::Ok) {
            {
                Gdiplus::Bitmap image(bitmap, nullptr);
                const CLSID png = {0x557cf406, 0x1a04, 0x11d3, {0x9a,0x73,0,0,0xf8,0x1e,0xf3,0x2e}};
                saved = image.Save(path, &png, nullptr) == Gdiplus::Ok;
            }
            Gdiplus::GdiplusShutdown(token);
        }
    }
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    std::wprintf(L"Capture %ls (%d x %d): %ls\n", path, width, height, saved ? L"OK" : L"failed");
    return saved;
}
int wmain(int argc, wchar_t **argv) {
    if (argc < 4) return 2;
    using DpiSetter = BOOL (WINAPI *)(HANDLE);
    const auto setDpi = reinterpret_cast<DpiSetter>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"));
    if (setDpi) setDpi(reinterpret_cast<HANDLE>(-4));
    const auto inputBefore = inputName();
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    const std::wstring name = L"EZ4Connectpro-QA-" + std::to_wstring(GetCurrentProcessId());
    const auto station = objectName(GetProcessWindowStation());
    USEROBJECTFLAGS flags{};
    const bool haveFlags = GetUserObjectInformationW(GetProcessWindowStation(), UOI_FLAGS, &flags, sizeof(flags), nullptr);
    std::wprintf(L"Station=%ls, visible=%d, thread=%ls, input=%ls\n", station.c_str(),
                 haveFlags && (flags.dwFlags & WSF_VISIBLE), objectName(original).c_str(), inputBefore.c_str());
    const std::wstring originalName = objectName(original);
    if (station.empty() || originalName.empty()) {
        std::printf("Input desktop could not be verified\n");
        return 3;
    }
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0,
        DESKTOP_CREATEWINDOW | DESKTOP_CREATEMENU | DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE, nullptr);
    if (!isolated) {
        std::printf("CreateDesktop failed: %lu\n", GetLastError());
        return 3;
    }
    if (objectName(isolated) != name || name == originalName || name == inputBefore || !SetThreadDesktop(isolated)) {
        std::printf("Independent thread desktop unavailable (error %lu)\n", GetLastError());
        CloseDesktop(isolated);
        return 3;
    }
    if (objectName(GetThreadDesktop(GetCurrentThreadId())) != name || inputName() != inputBefore) return 3;
    std::wprintf(L"Verified separate desktop %ls; runner did not request desktop-switch access\n", name.c_str());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    std::wstring desktop = station + L"\\" + name;
    startup.lpDesktop = desktop.data();
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return 4;
    PROCESS_INFORMATION process{};
    std::wstring line = L"\"" + std::wstring(argv[1]) + L"\" " + argv[2];
    if (!CreateProcessW(argv[1], line.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED, nullptr, nullptr, &startup, &process)) {
        std::printf("CreateProcess failed: %lu\n", GetLastError());
        return 4;
    }
    if (!AssignProcessToJobObject(job, process.hProcess)) {
        std::printf("Unable to contain QA child processes: %lu\n", GetLastError());
        TerminateProcess(process.hProcess, 7);
        return 4;
    }
    ResumeThread(process.hThread);
    if (std::wstring(argv[3]) == L"--no-window") {
        DWORD exitCode = 2;
        if (WaitForSingleObject(process.hProcess, 30000) == WAIT_OBJECT_0) GetExitCodeProcess(process.hProcess, &exitCode);
        CloseHandle(job);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        SetThreadDesktop(original);
        CloseDesktop(isolated);
        return exitCode;
    }
    WindowSearch search{0};
    int firstStep = 3;
    bool startupOk = true;
    if (std::wstring(argv[3]) == L"start-hidden") {
        ++firstStep;
        WindowSearch tray{0, nullptr, L"QTrayIconMessageWindow", true};
        for (int poll = 0; poll < 300 && !tray.hwnd && !search.hwnd; ++poll) {
            EnumDesktopWindows(isolated, findWindow, reinterpret_cast<LPARAM>(&search));
            EnumDesktopWindows(isolated, findWindow, reinterpret_cast<LPARAM>(&tray));
            Sleep(20);
        }
        for (int poll = 0; poll < 25 && !search.hwnd; ++poll) {
            EnumDesktopWindows(isolated, findWindow, reinterpret_cast<LPARAM>(&search));
            Sleep(20);
        }
        startupOk = tray.hwnd && !search.hwnd;
        std::printf("Startup stayed hidden until tray click: %d\n", startupOk);
        if (startupOk) PostMessageW(tray.hwnd, WM_APP + 101, 0, NIN_SELECT);
    }
    for (int i = 0; i < 150 && !search.hwnd; ++i) {
        EnumDesktopWindows(isolated, findWindow, reinterpret_cast<LPARAM>(&search));
        // A portable bootstrap exits after transferring cleanup to the UI's child.
        DWORD bootstrapStatus = STILL_ACTIVE;
        if (GetExitCodeProcess(process.hProcess, &bootstrapStatus) && bootstrapStatus != STILL_ACTIVE && bootstrapStatus != 0) break;
        if (!search.hwnd) Sleep(100);
    }
    bool ok = startupOk && search.hwnd != nullptr && inputName() == inputBefore;
    bool expectedCrash = false;
    HANDLE applicationProcess = process.hProcess;
    DWORD applicationPid = process.dwProcessId;
    if (search.hwnd) {
        GetWindowThreadProcessId(search.hwnd, &applicationPid);
        if (applicationPid != process.dwProcessId) {
            applicationProcess = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_TERMINATE, FALSE, applicationPid);
            ok = ok && applicationProcess;
        }
    }
    if (!search.hwnd) std::printf("Application window not found on the isolated desktop\n");
    if (ok) {
        Sleep(700);
        WSADATA wsa{}; WSAStartup(MAKEWORD(2,2), &wsa);
        POINT origin{};
        ClientToScreen(search.hwnd, &origin);
        RECT window{};
        GetWindowRect(search.hwnd, &window);
        std::printf("Screenshot to client offset: %ld,%ld\n", origin.x - window.left, origin.y - window.top);
        for (int i = firstStep; i < argc && ok; ++i) {
            const std::wstring step = argv[i];
            if (step.rfind(L"click=",0) == 0) {
                int x = 0, y = 0;
                swscanf(step.c_str() + 6, L"%d,%d", &x, &y);
                const LPARAM point = MAKELPARAM(x, y);
                PostMessageW(search.hwnd, WM_MOUSEMOVE, 0, point);
                PostMessageW(search.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                PostMessageW(search.hwnd, WM_LBUTTONUP, 0, point);
                Sleep(350);
            } else if (step == L"traycheck") {
                ok = testTray(search.hwnd);
            } else if (step == L"closecheck") {
                ok = testCloseToTray(search.hwnd, applicationProcess);
            } else if (step == L"processcheck") {
                ok = testProcessTree(search.hwnd, process.hProcess, argv[1]);
            } else if (step == L"crashcheck") {
                expectedCrash = true;
                ok = TerminateProcess(applicationProcess, 77);
            } else if (step == L"traymenu") {
                HWND tray = auxiliaryWindow(search.hwnd, true);
                ok = tray && PostMessageW(tray, WM_APP + 101, MAKELPARAM(80, 80), WM_CONTEXTMENU);
                Sleep(350);
            } else if (step.rfind(L"popup=",0) == 0) {
                HWND popup = auxiliaryWindow(search.hwnd, false);
                ok = popup && capture(popup, step.substr(6).c_str());
            } else if (step == L"popupclose") {
                HWND popup = auxiliaryWindow(search.hwnd, false);
                ok = popup && PostMessageW(popup, WM_CLOSE, 0, 0);
                Sleep(150);
            } else if (step.rfind(L"popupclick=",0) == 0) {
                HWND popup = auxiliaryWindow(search.hwnd, false);
                int x = 0, y = 0;
                swscanf(step.c_str() + 11, L"%d,%d", &x, &y);
                const LPARAM point = MAKELPARAM(x, y);
                ok = popup && PostMessageW(popup, WM_MOUSEMOVE, 0, point)
                    && PostMessageW(popup, WM_LBUTTONDOWN, MK_LBUTTON, point)
                    && PostMessageW(popup, WM_LBUTTONUP, 0, point);
                Sleep(350);
            } else if (step.rfind(L"wait=",0) == 0) {
                Sleep(std::stoul(step.substr(5)));
            } else if (step.rfind(L"run=",0) == 0) {
                const auto separator = step.find(L'|');
                if (separator == std::wstring::npos) return 6;
                const auto program = step.substr(4, separator - 4);
                auto command = L"\"" + program + L"\" " + step.substr(separator + 1);
                PROCESS_INFORMATION probe{};
                ok = CreateProcessW(program.c_str(), command.data(), nullptr, nullptr, FALSE,
                                    CREATE_SUSPENDED, nullptr, nullptr, &startup, &probe);
                if (ok) {
                    ok = AssignProcessToJobObject(job, probe.hProcess);
                    if (ok) {
                        ResumeThread(probe.hThread);
                        DWORD status = 1;
                        ok = WaitForSingleObject(probe.hProcess, 35000) == WAIT_OBJECT_0
                            && GetExitCodeProcess(probe.hProcess, &status) && status == 0;
                        std::printf("Isolated browser probe exit=%lu\n", status);
                    }
                    if (!ok) TerminateProcess(probe.hProcess, 9);
                    CloseHandle(probe.hThread);
                    CloseHandle(probe.hProcess);
                }
            } else if (step.rfind(L"open=",0) == 0 || step.rfind(L"closed=",0) == 0) {
                const bool expected = step.rfind(L"open=",0) == 0;
                const int port = std::stoi(step.substr(expected ? 5 : 7));
                const SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(static_cast<u_short>(port));
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                u_long nonBlocking = 1;
                ioctlsocket(sock, FIONBIO, &nonBlocking);
                bool connected = connect(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == 0;
                if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
                    fd_set writable;
                    FD_ZERO(&writable);
                    FD_SET(sock, &writable);
                    timeval deadline{0, 150000};
                    connected = select(0, nullptr, &writable, nullptr, &deadline) > 0;
                }
                closesocket(sock);
                ok = connected == expected;
                std::printf("TCP port %d: %s (expected %s)\n", port, connected ? "open" : "closed", expected ? "open" : "closed");
            } else if (step.rfind(L"dialog=",0) == 0) {
                const auto separator = step.find(L'|');
                if (separator == std::wstring::npos) return 6;
                WindowSearch dialog{0, nullptr, step.substr(7, separator - 7)};
                for (int poll = 0; poll < 25 && !dialog.hwnd; ++poll) {
                    EnumDesktopWindows(isolated, findWindow, reinterpret_cast<LPARAM>(&dialog));
                    if (!dialog.hwnd) Sleep(100);
                }
                const auto targetClick = step.find(L'|', separator + 1);
                if (dialog.hwnd && targetClick != std::wstring::npos) {
                    int x = 0, y = 0;
                    swscanf(step.c_str() + targetClick + 1, L"%d,%d", &x, &y);
                    const LPARAM point = MAKELPARAM(x, y);
                    PostMessageW(dialog.hwnd, WM_MOUSEMOVE, 0, point);
                    PostMessageW(dialog.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
                    PostMessageW(dialog.hwnd, WM_LBUTTONUP, 0, point);
                    Sleep(350);
                }
                const auto image = step.substr(separator + 1, targetClick - separator - 1);
                ok = dialog.hwnd && (image.empty() || capture(dialog.hwnd, image.c_str()));
                if (image.empty()) std::printf("Expected dialog: %s\n", dialog.hwnd ? "opened" : "missing");
                if (dialog.hwnd) PostMessageW(dialog.hwnd, WM_CLOSE, 0, 0);
                Sleep(350);
            } else if (step.rfind(L"size=",0) == 0) {
                int width = 0, height = 0;
                swscanf(step.c_str() + 5, L"%d,%d", &width, &height);
                SetWindowPos(search.hwnd, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOACTIVATE | SWP_NOZORDER);
                Sleep(350);
            } else {
                ok = capture(search.hwnd, step.c_str());
            }
            ok = ok && inputName() == inputBefore;
        }
        if (!expectedCrash && IsWindow(search.hwnd)) {
            const bool requested = requestTrayExit(search.hwnd);
            std::printf("Exit through tray menu: %d\n", requested);
            ok = requested && ok;
        }
    }
    if (applicationProcess && WaitForSingleObject(applicationProcess, 4500) != WAIT_OBJECT_0) {
        std::printf("Test process did not close normally\n");
        TerminateProcess(applicationProcess, 8);
        WaitForSingleObject(applicationProcess, 1500);
        ok = false;
    }
    DWORD exitCode = 0;
    if (applicationProcess) GetExitCodeProcess(applicationProcess, &exitCode);
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    for (int i = 0; i < 60; ++i) {
        if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr)
            || !accounting.ActiveProcesses) break;
        Sleep(100);
    }
    std::printf("Remaining owned QA processes: %lu\n", accounting.ActiveProcesses);
    ok = ok && !accounting.ActiveProcesses;
    if (applicationProcess && applicationProcess != process.hProcess) CloseHandle(applicationProcess);
    CloseHandle(job);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    SetThreadDesktop(original);
    CloseDesktop(isolated);
    std::printf("Test application exit=%lu; input desktop unchanged=%d\n", exitCode, inputName() == inputBefore);
    return ok && exitCode == (expectedCrash ? 77 : 0) && inputName() == inputBefore ? 0 : 1;
}



