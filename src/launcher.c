/* PoP2 Launcher: picks the game folder and options, writes pop2.ini, starts pop2.exe. */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <commctrl.h>
#include <mmsystem.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <stdio.h>
#include <string.h>

enum { ID_DIR = 101, ID_BROWSE, ID_FULL, ID_SCALE, ID_PAD, ID_PADSTAT, ID_LEVEL, ID_PLAY, ID_CONTROLS, ID_SHORTCUT, ID_TIMER, ID_FILTER, ID_ASPECT, ID_CP, ID_MUSIC, ID_COFFEE };

static HWND hwnd, h_dir, h_full, h_scale, h_pad, h_padstat, h_level, h_play, h_filter, h_aspect, h_cp, h_music;
#define COFFEE_URL "https://buymeacoffee.com/mohmmadpodt"
static HFONT font, font_big;
static HBITMAP banner;
static char ini[MAX_PATH], exe_dir[MAX_PATH];
static int dpi = 96;
#define S(x) MulDiv((x), dpi, 96)

/* ---- XInput presence (status line only; the game reads the pad itself) */
typedef DWORD (WINAPI *XInputGetStateFn)(DWORD, void *);
static XInputGetStateFn xget;
static int pad_connected(void) {
    BYTE st[16];
    if (!xget) return -1;
    for (int i = 0; i < 4; i++) if (xget(i, st) == 0) return 1;
    return 0;
}
static void update_pad_status(void) {
    char msg[160];
    if (pad_connected() > 0) { SetWindowTextA(h_padstat, "Xbox controller connected"); return; }
    /* other pads (PlayStation, Switch, generic USB) through the Windows joystick API */
    UINT n = joyGetNumDevs();
    for (UINT i = 0; i < n && i < 16; i++) {
        JOYINFOEX ji = { sizeof ji, JOY_RETURNALL };
        if (joyGetPosEx(i, &ji) == JOYERR_NOERROR) {
            JOYCAPSA caps;
            if (joyGetDevCapsA(i, &caps, sizeof caps) != JOYERR_NOERROR) strcpy(caps.szPname, "Game controller");
            snprintf(msg, sizeof msg, "Connected: %s", caps.szPname);
            SetWindowTextA(h_padstat, msg); return;
        }
    }
    SetWindowTextA(h_padstat, "No controller connected");
}

static int has_game(const char *dir) {
    char p[MAX_PATH]; snprintf(p, MAX_PATH, "%s\\PRINCE.EXE", dir);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}

static void browse(void) {
    BROWSEINFOA bi = {0}; char name[MAX_PATH];
    bi.hwndOwner = hwnd; bi.pszDisplayName = name;
    bi.lpszTitle = "Choose the Prince of Persia 2 folder (the one with PRINCE.EXE)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
    if (!pidl) return;
    char path[MAX_PATH];
    if (SHGetPathFromIDListA(pidl, path)) {
        SetWindowTextA(h_dir, path);
        if (!has_game(path)) MessageBoxA(hwnd, "PRINCE.EXE was not found in that folder.", "Prince of Persia 2", MB_ICONWARNING);
    }
    CoTaskMemFree(pidl);
}

static void save_settings(void) {
    char dir[MAX_PATH], v[16];
    GetWindowTextA(h_dir, dir, MAX_PATH);
    WritePrivateProfileStringA("pop2", "GameDir", dir, ini);
    WritePrivateProfileStringA("pop2", "Fullscreen", SendMessageA(h_full, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", ini);
    snprintf(v, sizeof v, "%d", (int)SendMessageA(h_scale, CB_GETCURSEL, 0, 0) + 1);
    WritePrivateProfileStringA("pop2", "Scale", v, ini);
    WritePrivateProfileStringA("pop2", "Controller", SendMessageA(h_pad, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", ini);
    snprintf(v, sizeof v, "%d", (int)SendMessageA(h_level, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringA("launcher", "StartLevel", v, ini);
    snprintf(v, sizeof v, "%d", (int)SendMessageA(h_filter, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringA("pop2", "Filter", v, ini);
    snprintf(v, sizeof v, "%d", (int)SendMessageA(h_aspect, CB_GETCURSEL, 0, 0));
    WritePrivateProfileStringA("pop2", "Aspect", v, ini);
    WritePrivateProfileStringA("pop2", "Checkpoints", SendMessageA(h_cp, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0", ini);
    WritePrivateProfileStringA("pop2", "Music", SendMessageA(h_music, CB_GETCURSEL, 0, 0) == 1 ? "gm" : "fm", ini);
}

static void play(void) {
    char dir[MAX_PATH]; GetWindowTextA(h_dir, dir, MAX_PATH);
    if (!has_game(dir)) { MessageBoxA(hwnd, "PRINCE.EXE was not found in the game folder.\nPress Browse and choose the Prince of Persia 2 folder.", "Prince of Persia 2", MB_ICONWARNING); return; }
    save_settings();
    char exe[MAX_PATH], cmd[MAX_PATH * 2];
    snprintf(exe, MAX_PATH, "%s\\pop2.exe", exe_dir);
    int lv = (int)SendMessageA(h_level, CB_GETCURSEL, 0, 0);
    if (lv > 0) snprintf(cmd, sizeof cmd, "\"%s\" MAKINIT LEVEL%d", exe, lv);
    else snprintf(cmd, sizeof cmd, "\"%s\"", exe);
    STARTUPINFOA si = { sizeof si }; PROCESS_INFORMATION pi;
    if (!CreateProcessA(exe, cmd, NULL, NULL, FALSE, 0, NULL, exe_dir, &si, &pi)) {
        MessageBoxA(hwnd, "Could not start pop2.exe (it must be in the same folder as the launcher).", "Prince of Persia 2", MB_ICONERROR);
        return;
    }
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DestroyWindow(hwnd);
}

static void show_controls(void) {
    MessageBoxA(hwnd,
        "KEYBOARD\n"
        "  Arrow keys\tRun left / right, jump (Up), crouch (Down)\n"
        "  Shift\t\tAction: grab ledges, pick up, drink; careful step\n"
        "  Ctrl\t\tSword strike   (Up = block)\n"
        "  Space\t\tSkip intro / show time left\n"
        "  Esc\t\tPause\n"
        "  Ctrl+A\t\tRestart level      Ctrl+G  Save game\n"
        "  Alt+Enter / F11\tFullscreen on / off\n"
        "  F7 / F8\t\tChange filter / view (4:3, wide, panorama)\n"
        "\n"
        "CONTROLLER\t  Xbox\t\tPlayStation\n"
        "  Move\t\t  D-pad / stick\tD-pad / stick\n"
        "  Jump / block\t  A\t\tCross\n"
        "  Action (Shift)\t  B, RB, RT\tCircle, R1, R2\n"
        "  Sword (Ctrl)\t  X, LB, LT\tSquare, L1, L2\n"
        "  Crouch\t\t  Y\t\tTriangle\n"
        "  Pause\t\t  Start\t\tOptions\n"
        "  Skip / time\t  Back\t\tCreate / Share\n",
        "Prince of Persia 2 - Controls", MB_OK);
}

static void make_shortcut(void) {
    char desk[MAX_PATH], lnk[MAX_PATH], target[MAX_PATH];
    if (FAILED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, 0, desk))) return;
    snprintf(lnk, MAX_PATH, "%s\\Prince of Persia 2.lnk", desk);
    GetModuleFileNameA(NULL, target, MAX_PATH);
    IShellLinkA *sl; IPersistFile *pf; WCHAR wl[MAX_PATH];
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkA, (void **)&sl))) return;
    IShellLinkA_SetPath(sl, target);
    IShellLinkA_SetWorkingDirectory(sl, exe_dir);
    IShellLinkA_SetIconLocation(sl, target, 0);
    IShellLinkA_SetDescription(sl, "Prince of Persia 2 - The Shadow and the Flame");
    int ok = 0;
    if (SUCCEEDED(IShellLinkA_QueryInterface(sl, &IID_IPersistFile, (void **)&pf))) {
        MultiByteToWideChar(CP_ACP, 0, lnk, -1, wl, MAX_PATH);
        ok = SUCCEEDED(IPersistFile_Save(pf, wl, TRUE));
        IPersistFile_Release(pf);
    }
    IShellLinkA_Release(sl);
    MessageBoxA(hwnd, ok ? "A \"Prince of Persia 2\" shortcut was added to your desktop." : "Could not create the desktop shortcut.",
                "Prince of Persia 2", ok ? MB_ICONINFORMATION : MB_ICONWARNING);
}

/* ---- layout */
#define BANNER_H 180
static HWND ctl(const char *cls, const char *text, DWORD style, int x, int y, int w, int h, int id) {
    HWND c = CreateWindowExA(0, cls, text, WS_CHILD | WS_VISIBLE | style, S(x), S(y), S(w), S(h), hwnd, (HMENU)(INT_PTR)id, GetModuleHandleA(NULL), NULL);
    SendMessageA(c, WM_SETFONT, (WPARAM)font, TRUE);
    return c;
}
static void build(void) {
    NONCLIENTMETRICSA ncm = { sizeof ncm };
    SystemParametersInfoA(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0);
    ncm.lfMessageFont.lfHeight = -S(15);
    font = CreateFontIndirectA(&ncm.lfMessageFont);
    ncm.lfMessageFont.lfHeight = -S(20); ncm.lfMessageFont.lfWeight = FW_BOLD;
    font_big = CreateFontIndirectA(&ncm.lfMessageFont);

    int y = BANNER_H + 14;
    ctl("STATIC", "Game folder", 0, 16, y + 3, 90, 20, 0);
    h_dir = ctl("EDIT", "", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, 110, y, 300, 24, ID_DIR);
    ctl("BUTTON", "Browse...", BS_PUSHBUTTON | WS_TABSTOP, 418, y - 1, 86, 26, ID_BROWSE);
    y += 40;
    ctl("BUTTON", "Display", BS_GROUPBOX, 16, y, 264, 150, 0);
    h_full = ctl("BUTTON", "Start in fullscreen", BS_AUTOCHECKBOX | WS_TABSTOP, 30, y + 24, 200, 22, ID_FULL);
    ctl("STATIC", "Window size", 0, 28, y + 56, 80, 20, 0);
    h_scale = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 108, y + 52, 164, 200, ID_SCALE);
    ctl("STATIC", "Filter", 0, 28, y + 88, 80, 20, 0);
    h_filter = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 108, y + 84, 164, 200, ID_FILTER);
    ctl("STATIC", "View", 0, 28, y + 120, 80, 20, 0);
    h_aspect = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 108, y + 116, 164, 200, ID_ASPECT);
    ctl("BUTTON", "Controller", BS_GROUPBOX, 288, y, 216, 88, 0);
    h_pad = ctl("BUTTON", "Use game controller", BS_AUTOCHECKBOX | WS_TABSTOP, 300, y + 24, 196, 22, ID_PAD);
    h_padstat = ctl("STATIC", "", 0, 300, y + 48, 200, 36, ID_PADSTAT);
    ctl("BUTTON", "Gameplay", BS_GROUPBOX, 288, y + 96, 216, 90, 0);
    h_cp = ctl("BUTTON", "Extra checkpoints", BS_AUTOCHECKBOX | WS_TABSTOP, 300, y + 120, 196, 22, ID_CP);
    ctl("STATIC", "Music", 0, 300, y + 152, 56, 20, 0);
    h_music = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 356, y + 148, 140, 200, ID_MUSIC);
    y += 200;
    ctl("STATIC", "Start at", 0, 16, y + 3, 90, 20, 0);
    h_level = ctl("COMBOBOX", "", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 110, y, 220, 300, ID_LEVEL);
    ctl("SysLink", "<a href=\"" COFFEE_URL "\">Buy me a coffee</a>", 0, 346, y + 3, 158, 22, ID_COFFEE);
    y += 46;
    ctl("BUTTON", "Controls", BS_PUSHBUTTON | WS_TABSTOP, 16, y, 100, 34, ID_CONTROLS);
    ctl("BUTTON", "Desktop shortcut", BS_PUSHBUTTON | WS_TABSTOP, 124, y, 140, 34, ID_SHORTCUT);
    h_play = ctl("BUTTON", "PLAY", BS_DEFPUSHBUTTON | WS_TABSTOP, 364, y, 140, 34, ID_PLAY);
    SendMessageA(h_play, WM_SETFONT, (WPARAM)font_big, TRUE);

    for (int s = 1; s <= 6; s++) { char t[32]; snprintf(t, sizeof t, "%dx  (%d x %d)", s, 320 * s, 240 * s); SendMessageA(h_scale, CB_ADDSTRING, 0, (LPARAM)t); }
    SendMessageA(h_music, CB_ADDSTRING, 0, (LPARAM)"FM (original)");
    SendMessageA(h_music, CB_ADDSTRING, 0, (LPARAM)"General MIDI");
    SendMessageA(h_level, CB_ADDSTRING, 0, (LPARAM)"Beginning (normal game)");
    for (int l = 1; l <= 14; l++) { char t[48]; snprintf(t, sizeof t, "Level %d  (cheat mode)", l); SendMessageA(h_level, CB_ADDSTRING, 0, (LPARAM)t); }

    /* current settings */
    char dir[MAX_PATH];
    GetPrivateProfileStringA("pop2", "GameDir", "", dir, MAX_PATH, ini);
    if (!dir[0]) { if (has_game(exe_dir)) strcpy(dir, exe_dir); else if (has_game("D:\\Dos\\prince 2.pc")) strcpy(dir, "D:\\Dos\\prince 2.pc"); }
    SetWindowTextA(h_dir, dir);
    SendMessageA(h_full, BM_SETCHECK, GetPrivateProfileIntA("pop2", "Fullscreen", 0, ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    int sc = GetPrivateProfileIntA("pop2", "Scale", 3, ini); if (sc < 1 || sc > 6) sc = 3;
    SendMessageA(h_scale, CB_SETCURSEL, sc - 1, 0);
    SendMessageA(h_pad, BM_SETCHECK, GetPrivateProfileIntA("pop2", "Controller", 1, ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    int lv = GetPrivateProfileIntA("launcher", "StartLevel", 0, ini); if (lv < 0 || lv > 14) lv = 0;
    SendMessageA(h_level, CB_SETCURSEL, lv, 0);
    const char *filters[] = { "Sharp (best)", "Pixel (nearest)", "Soft (bilinear)", "Smooth HQ (Scale3x)" };
    for (int i = 0; i < 4; i++) SendMessageA(h_filter, CB_ADDSTRING, 0, (LPARAM)filters[i]);
    const char *views[] = { "4:3 (original)", "Wide (stretch)", "Wide (panorama)" };
    for (int i = 0; i < 3; i++) SendMessageA(h_aspect, CB_ADDSTRING, 0, (LPARAM)views[i]);
    int fi = GetPrivateProfileIntA("pop2", "Filter", 0, ini); if (fi < 0 || fi > 3) fi = 0;
    int as = GetPrivateProfileIntA("pop2", "Aspect", 0, ini); if (as < 0 || as > 2) as = 0;
    SendMessageA(h_filter, CB_SETCURSEL, fi, 0); SendMessageA(h_aspect, CB_SETCURSEL, as, 0);
    SendMessageA(h_cp, BM_SETCHECK, GetPrivateProfileIntA("pop2", "Checkpoints", 0, ini) ? BST_CHECKED : BST_UNCHECKED, 0);
    { char m[16]; GetPrivateProfileStringA("pop2", "Music", "fm", m, sizeof m, ini);
      SendMessageA(h_music, CB_SETCURSEL, _stricmp(m, "fm") == 0 ? 0 : 1, 0); }
    update_pad_status();
    SetTimer(hwnd, ID_TIMER, 1000, NULL);
    SetFocus(h_play);
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NOTIFY: {            /* the "Buy me a coffee" link */
        NMHDR *n = (NMHDR *)l;
        if (n->idFrom == ID_COFFEE && (n->code == NM_CLICK || n->code == NM_RETURN)) {
            ShellExecuteA(hwnd, "open", COFFEE_URL, NULL, NULL, SW_SHOWNORMAL);
            return 0;
        }
        break; }
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case ID_BROWSE: browse(); return 0;
        case ID_PLAY: case IDOK: play(); return 0;
        case ID_CONTROLS: show_controls(); return 0;
        case ID_SHORTCUT: make_shortcut(); return 0;
        case IDCANCEL: DestroyWindow(h); return 0;
        }
        break;
    case WM_TIMER: update_pad_status(); return 0;
    case WM_CTLCOLORSTATIC: {
        HDC dc = (HDC)w; SetBkMode(dc, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    }
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT r; GetClientRect(h, &r);
        if (banner) {
            BITMAP bm; GetObject(banner, sizeof bm, &bm);
            HDC mdc = CreateCompatibleDC(dc); HGDIOBJ old = SelectObject(mdc, banner);
            SetStretchBltMode(dc, COLORONCOLOR);
            StretchBlt(dc, 0, 0, r.right, S(BANNER_H), mdc, 0, 0, bm.bmWidth, bm.bmHeight, SRCCOPY);
            SelectObject(mdc, old); DeleteDC(mdc);
        }
        EndPaint(h, &ps); return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcA(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    CoInitialize(NULL);
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES | ICC_LINK_CLASS };
    InitCommonControlsEx(&icc);
    GetModuleFileNameA(NULL, exe_dir, MAX_PATH);
    char *s = strrchr(exe_dir, '\\'); if (s) *s = 0;
    snprintf(ini, MAX_PATH, "%s\\pop2.ini", exe_dir);
    const char *dll[] = { "xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll" };
    for (int i = 0; i < 3 && !xget; i++) { HMODULE m = LoadLibraryA(dll[i]); if (m) xget = (XInputGetStateFn)(void *)GetProcAddress(m, "XInputGetState"); }

    HDC sdc = GetDC(NULL); dpi = GetDeviceCaps(sdc, LOGPIXELSY); ReleaseDC(NULL, sdc);
    banner = (HBITMAP)LoadImageA(hi, MAKEINTRESOURCEA(100), IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION);

    WNDCLASSEXA wc = { sizeof wc };
    wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.lpszClassName = "PoP2Launcher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconA(hi, MAKEINTRESOURCEA(1));
    wc.hIconSm = (HICON)LoadImageA(hi, MAKEINTRESOURCEA(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    RegisterClassExA(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r = { 0, 0, S(520), S(BANNER_H + 14 + 40 + 200 + 46 + 34 + 16) };
    AdjustWindowRect(&r, style, FALSE);
    int ww = r.right - r.left, wh = r.bottom - r.top;
    hwnd = CreateWindowExA(0, "PoP2Launcher", "Prince of Persia 2", style,
                           (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2, ww, wh,
                           NULL, NULL, hi, NULL);
    build();
    ShowWindow(hwnd, show); UpdateWindow(hwnd);
    MSG msg;
    while (GetMessageA(&msg, NULL, 0, 0)) {
        if (IsDialogMessageA(hwnd, &msg)) continue;
        TranslateMessage(&msg); DispatchMessageA(&msg);
    }
    CoUninitialize();
    return 0;
}
