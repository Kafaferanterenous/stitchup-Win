// Stitchup PDF Editor - portable Nitro-style PDF viewer/editor.
// Native Win32 GUI + PDFium (BSD-3-Clause, statically wired via import lib).
// Single page source: GUI host, rendering, self-test.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <windowsx.h>

#pragma warning(push, 0)
#include <fpdfview.h>
#include <fpdf_edit.h>
#include <fpdf_save.h>
#include <fpdf_ppo.h>
#include <fpdf_doc.h>
#include <fpdf_annot.h>
#include <fpdf_transformpage.h>
#include <fpdf_text.h>
#pragma warning(pop)

#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <cmath>
#include <algorithm>
#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Command ids (shared by toolbar buttons and menu items)
// ---------------------------------------------------------------------------
enum
{
  ID_NEW = 4001,
  ID_OPEN,
  ID_SAVE,
  ID_SAVEAS,
  ID_IMPORT,
  ID_DELETE,
  ID_ADD,
  ID_ROTL,
  ID_ROTR,
  ID_ZOOM_OUT,
  ID_ZOOM_IN,
  ID_ZOOM100,
  ID_FITW,
  ID_FITP,
  ID_PREV,
  ID_NEXT,
  ID_ABOUT,
  ID_EXIT,
  ID_TAB_HOME,
  ID_TAB_VIEW,
  ID_TAB_TOOLS,
  ID_PANE_THUMBS,
  ID_PANE_BOOKMARKS,
  ID_ANN_HL,
  ID_ANN_UL,
  ID_ANN_NOTE,
  ID_ANN_TEXT,
  ID_ANN_SHAPE,
  ID_ANN_STAMP,
  ID_PAGE_EXTRACT,
  ID_PAGE_SPLIT,
  ID_PAGE_CROP,
  ID_EXPORT_TEXT,
  ID_EXPORT_CSV,
  ID_THEME,
  ID_WATERMARK,
  ID_SAVEENC,
  ID_TOOL_SELECT,
  ID_OBJ_EDIT,
  ID_OBJ_DELETE,
  ID_OBJ_RECOLOR,
};

enum
{
  RIB_TAB_H = 24,   // ribbon tab strip height
  RIB_BTN_Y = 32,   // button row top
  RIB_BTN_H = 34,   // button height
  RIB_CAP_Y = 70,   // group caption row top
  RIB_H = 92,       // full ribbon height
  PANE_TAB_H = 26,  // navigation-pane header height
};

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------
struct Btn
{
  std::wstring label;
  bool tab = false;      // ribbon/pane tab styling
  bool pressed = false;  // active tab state
  bool hover = false;
  bool down = false;
  int rtab = -1;         // owning ribbon tab (ID_TAB_*), -1 = not a ribbon btn
  int rgroup = -1;       // group index within that tab
};

struct PageCache
{
  int key;      // zoom key used to render
  HBITMAP bmp;
  int w;
  int h;
};

struct FileWriter
{
  FPDF_FILEWRITE base;
  std::vector<unsigned char> buf;
};

struct App
{
  HINSTANCE inst = nullptr;
  HWND frame = nullptr;
  HWND toolbar = nullptr;
  HWND thumbs = nullptr;
  HWND split = nullptr;
  HWND canvas = nullptr;
  HWND status = nullptr;
  HWND paneTabs = nullptr;
  HWND bookmarks = nullptr;
  HFONT font = nullptr;
  HFONT treeFont = nullptr;
  int dpi = 96;

  FPDF_DOCUMENT doc = nullptr;
  bool dirty = false;
  std::wstring path;
  std::wstring name;
  int pageCount = 0;
  int selected = 0;
  double zoom = 1.0;   // screen px per PDF pt (1.0 == 96 dpi)
  int scrollX = 0;
  int scrollY = 0;
  int thumbsW = 210;
  int pane = 0;        // 0 = thumbnails, 1 = bookmarks
  int ribbonTab = 0;   // 0 = Home, 1 = View, 2 = Tools
  bool bmDirty = true;

  struct GroupBox
  {
    int tab;
    std::wstring name;
    RECT rc{};
  };
  std::vector<GroupBox> groups;
  std::vector<HWND> ribbonBtns;
  HWND tabBtns[3] = {};

  std::map<int, PageCache> canvasCache;
  std::map<int, HBITMAP> thumbCache;

  bool thumbDrag = false;
  int dragPage = -1;
  int dragCursor = -1;

  bool toolSelect = false;   // Select/Move content-object tool
  struct Sel
  {
    bool active = false;
    int page = -1;
    int index = -1;
    int type = 0;
    float l = 0, b = 0, r = 0, t = 0;
  } sel;
  FPDF_PAGE editPage = nullptr;   // page kept open during a live object drag
  FPDF_PAGEOBJECT editObj = nullptr;
  bool selDrag = false;
  bool selDragMoved = false;
  double dragLastX = 0, dragLastY = 0;
};

static App g;

// ---------------------------------------------------------------------------
// Theme (light / dark) - flat modern palette
// ---------------------------------------------------------------------------
struct Theme
{
  COLORREF ribbonBg;    // toolbar background
  COLORREF card;        // ribbon group card fill
  COLORREF cardBorder;  // ribbon group card border
  COLORREF accent;      // accent (selection, active tab, frames)
  COLORREF accentDeep;  // pressed accent
  COLORREF text;        // primary text
  COLORREF textDim;     // captions / secondary text
  COLORREF btnHover;    // button hover fill
  COLORREF btnDown;     // button pressed fill
  COLORREF btnBorder;   // button resting border
  COLORREF thumbBg;     // thumbnails panel background
  COLORREF canvasBg;    // canvas background
  COLORREF pageFrame;   // page frame on canvas
  COLORREF split;       // splitter / chrome
  COLORREF statusBg;    // status bar background
  COLORREF statusTxt;   // status bar text
  COLORREF treeBg;      // pane tree background
  COLORREF treeTxt;     // pane tree text
};

static bool g_dark = false;

static Theme LightTheme()
{
  Theme t{};
  t.ribbonBg   = RGB(0xF7, 0xF8, 0xFA);
  t.card       = RGB(0xEF, 0xF0, 0xF3);
  t.cardBorder = RGB(0xD8, 0xDA, 0xDE);
  t.accent     = RGB(0x0B, 0x6C, 0xE0);
  t.accentDeep = RGB(0x0B, 0x3E, 0x77);
  t.text       = RGB(0x20, 0x20, 0x20);
  t.textDim    = RGB(0x80, 0x80, 0x80);
  t.btnHover   = RGB(0xE6, 0xEF, 0xFB);
  t.btnDown    = RGB(0xC8, 0xDC, 0xF2);
  t.btnBorder  = RGB(0xD5, 0xD5, 0xD5);
  t.thumbBg    = RGB(0xEC, 0xEC, 0xEC);
  t.canvasBg   = RGB(0xE2, 0xE2, 0xE2);
  t.pageFrame  = RGB(0x99, 0x99, 0x99);
  t.split      = RGB(0xD8, 0xDA, 0xDE);
  t.statusBg   = RGB(0x2B, 0x2B, 0x2B);
  t.statusTxt  = RGB(0xE8, 0xE8, 0xE8);
  t.treeBg     = RGB(0xFF, 0xFF, 0xFF);
  t.treeTxt    = RGB(0x20, 0x20, 0x20);
  return t;
}

static Theme DarkTheme()
{
  Theme t{};
  t.ribbonBg   = RGB(0x20, 0x20, 0x20);
  t.card       = RGB(0x28, 0x28, 0x28);
  t.cardBorder = RGB(0x3A, 0x3A, 0x3A);
  t.accent     = RGB(0x4C, 0xA0, 0xFF);
  t.accentDeep = RGB(0x1E, 0x6F, 0xC9);
  t.text       = RGB(0xE8, 0xE8, 0xE8);
  t.textDim    = RGB(0x9A, 0x9A, 0x9A);
  t.btnHover   = RGB(0x33, 0x39, 0x43);
  t.btnDown    = RGB(0x3D, 0x46, 0x54);
  t.btnBorder  = RGB(0x3A, 0x3A, 0x3A);
  t.thumbBg    = RGB(0x23, 0x23, 0x23);
  t.canvasBg   = RGB(0x1B, 0x1B, 0x1B);
  t.pageFrame  = RGB(0x6A, 0x6A, 0x6A);
  t.split      = RGB(0x2E, 0x2E, 0x2E);
  t.statusBg   = RGB(0x16, 0x16, 0x16);
  t.statusTxt  = RGB(0xC9, 0xC9, 0xC9);
  t.treeBg     = RGB(0x20, 0x20, 0x20);
  t.treeTxt    = RGB(0xE0, 0xE0, 0xE0);
  return t;
}

static Theme ThemeNow() { return g_dark ? DarkTheme() : LightTheme(); }

static void ApplyTreeTheme()
{
  if (!g.bookmarks) return;
  const Theme& th = ThemeNow();
  SendMessageW(g.bookmarks, TVM_SETBKCOLOR, 0, (LPARAM)th.treeBg);
  SendMessageW(g.bookmarks, TVM_SETTEXTCOLOR, 0, (LPARAM)th.treeTxt);
  SendMessageW(g.bookmarks, TVM_SETLINECOLOR, 0, (LPARAM)th.treeTxt);
  InvalidateRect(g.bookmarks, nullptr, TRUE);
}

static void ToggleTheme()
{
  g_dark = !g_dark;
  if (g.toolbar)  InvalidateRect(g.toolbar, nullptr, TRUE);
  if (g.thumbs)   InvalidateRect(g.thumbs, nullptr, TRUE);
  if (g.split)    InvalidateRect(g.split, nullptr, TRUE);
  if (g.canvas)   InvalidateRect(g.canvas, nullptr, TRUE);
  if (g.status)   InvalidateRect(g.status, nullptr, TRUE);
  for (int i = 0; i < 3 && g.tabBtns[i]; ++i)
    InvalidateRect(g.tabBtns[i], nullptr, TRUE);
  for (HWND hw : g.ribbonBtns)
    InvalidateRect(hw, nullptr, TRUE);
  ApplyTreeTheme();
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER,
                      L"Software\\StitchupPDFEditor", 0, nullptr, 0,
                      KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS)
  {
    DWORD v = g_dark ? 1 : 0;
    RegSetValueExW(key, L"Dark", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
  }
  CheckMenuItem(GetMenu(g.frame), ID_THEME,
                MF_BYCOMMAND | (g_dark ? MF_CHECKED : MF_UNCHECKED));
}

static void ApplyInitialThemePref()
{
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StitchupPDFEditor",
                    0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
  {
    DWORD v = 0, sz = sizeof(v);
    if (RegQueryValueExW(key, L"Dark", nullptr, nullptr,
                         (LPBYTE)&v, &sz) == ERROR_SUCCESS && v)
      g_dark = true;
    RegCloseKey(key);
  }
}

static std::string Utf8(const std::wstring& w)
{
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(),
                              nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0],
                      n, nullptr, nullptr);
  return s;
}

// ---------------------------------------------------------------------------
// PDFium helpers
// ---------------------------------------------------------------------------
static void CheckLib()
{
  if (!g.doc)
  {
    g.doc = FPDF_CreateNewDocument();
    FPDFPage_New(g.doc, 0, 612.0, 792.0);
    g.pageCount = FPDF_GetPageCount(g.doc);
  }
}

static int ZoomKey() { return (int)(g.zoom * 1000.0 + 0.5); }

static float PageW(int i)
{
  FPDF_PAGE p = FPDF_LoadPage(g.doc, i);
  float w = p ? FPDF_GetPageWidthF(p) : 0.0f;
  if (p) FPDF_ClosePage(p);
  return w;
}

static float PageH(int i)
{
  FPDF_PAGE p = FPDF_LoadPage(g.doc, i);
  float h = p ? FPDF_GetPageHeightF(p) : 0.0f;
  if (p) FPDF_ClosePage(p);
  return h;
}

static double MaxPageW()
{
  double m = 0;
  for (int i = 0; i < g.pageCount; ++i)
    m = std::max(m, (double)PageW(i));
  return m;
}

static double MaxPageH()
{
  double m = 0;
  for (int i = 0; i < g.pageCount; ++i)
    m = std::max(m, (double)PageH(i));
  return m;
}

static HBITMAP RenderPageBitmapInto(FPDF_PAGE p, int w, int h)
{
  if (!p || w < 1 || h < 1) return nullptr;
  HDC hdc = GetDC(g.canvas ? g.canvas : nullptr);
  BITMAPINFO bi{};
  bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bi.bmiHeader.biWidth = w;
  bi.bmiHeader.biHeight = -h;
  bi.bmiHeader.biPlanes = 1;
  bi.bmiHeader.biBitCount = 32;
  bi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP hb = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (hdc) ReleaseDC(g.canvas ? g.canvas : nullptr, hdc);
  if (!hb || !bits) { if (hb) DeleteObject(hb); return nullptr; }

  int stride = ((w * 32 + 31) / 32) * 4;
  FPDF_BITMAP fb = FPDFBitmap_CreateEx(w, h, FPDFBitmap_BGRA, bits, stride);
  FPDFBitmap_FillRect(fb, 0, 0, w, h, 0xFFFFFFFF);

  FPDF_RenderPageBitmap(fb, p, 0, 0, w, h, 0,
                        FPDF_ANNOT | FPDF_LCD_TEXT);
  FPDFBitmap_Destroy(fb);
  return hb;
}

static HBITMAP RenderPageBitmap(int index, int w, int h)
{
  if (g.pageCount == 0 || w < 1 || h < 1) return nullptr;
  FPDF_PAGE p = FPDF_LoadPage(g.doc, index);
  HBITMAP hb = p ? RenderPageBitmapInto(p, w, h) : nullptr;
  if (p) FPDF_ClosePage(p);
  return hb;
}

static void ClearCanvasCache()
{
  for (auto& kv : g.canvasCache)
    if (kv.second.bmp) DeleteObject(kv.second.bmp);
  g.canvasCache.clear();
}

static void ClearThumbCache()
{
  for (auto& kv : g.thumbCache)
    if (kv.second) DeleteObject(kv.second);
  g.thumbCache.clear();
}

// ---------------------------------------------------------------------------
// Document operations
// ---------------------------------------------------------------------------
static void CloseDoc()
{
  if (g.doc)
  {
    FPDF_CloseDocument(g.doc);
    g.doc = nullptr;
  }
  ClearCanvasCache();
  ClearThumbCache();
  g.pageCount = 0;
  g.selected = 0;
  g.path.clear();
  g.name.clear();
  g.dirty = false;
  g.bmDirty = true;
}

static void RefreshState()
{
  g.pageCount = g.doc ? FPDF_GetPageCount(g.doc) : 0;
  if (g.selected >= g.pageCount) g.selected = g.pageCount ? g.pageCount - 1 : 0;
  if (g.selected < 0) g.selected = 0;
  ClearCanvasCache();
  if (g.status) InvalidateRect(g.status, nullptr, TRUE);
}

// Password-required dialog for opening encrypted PDFs.
static HWND   g_pwdEdit = nullptr;
static bool   g_pwdOk = false;
static std::wstring g_pwdValue;

static LRESULT CALLBACK PwdProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_CREATE:
      CreateWindowExW(0, L"STATIC",
        L"This PDF is password-protected.\nEnter the password to open it:",
        WS_CHILD | WS_VISIBLE, 16, 14, 308, 34, h, nullptr, g.inst, nullptr);
      g_pwdEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        16, 54, 308, 24, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Ok", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        100, 96, 74, 28, h, (HMENU)1, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        186, 96, 74, 28, h, (HMENU)2, g.inst, nullptr);
      SetFocus(g_pwdEdit);
      return 0;
    case WM_COMMAND:
      if (LOWORD(w) == 1 || LOWORD(w) == 2)
      {
        if (LOWORD(w) == 1 && g_pwdEdit)
        {
          wchar_t buf[256];
          GetWindowTextW(g_pwdEdit, buf, 256);
          g_pwdValue = buf;
        }
        g_pwdOk = (LOWORD(w) == 1);
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_CLOSE:
      g_pwdOk = false;
      DestroyWindow(h);
      return 0;
    case WM_CTLCOLORSTATIC:
      return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
  }
  return DefWindowProcW(h, m, w, l);
}

static bool PromptPassword(std::wstring& out)
{
  const wchar_t cls[] = L"SKPwdWnd";
  static bool reg = false;
  if (!reg)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = PwdProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    reg = true;
  }
  g_pwdEdit = nullptr;
  g_pwdOk = false;
  g_pwdValue.clear();
  HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Password required",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 348, 172,
                            g.frame, nullptr, g.inst, nullptr);
  if (!hw) return false;
  RECT fr, rc;
  GetWindowRect(g.frame, &fr);
  GetWindowRect(hw, &rc);
  SetWindowPos(hw, nullptr,
               fr.left + (fr.right - fr.left - (rc.right - rc.left)) / 2,
               fr.top + (fr.bottom - fr.top - (rc.bottom - rc.top)) / 2,
               0, 0, SWP_NOSIZE | SWP_NOZORDER);
  ShowWindow(hw, SW_SHOW);
  UpdateWindow(hw);
  HWND owner = g.frame;
  EnableWindow(owner, FALSE);
  MSG msg;
  while (IsWindow(hw))
  {
    const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
    if (r <= 0) break;
    if (!IsDialogMessageW(hw, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  SetFocus(owner);
  out = g_pwdValue;
  return g_pwdOk;
}

static FPDF_DOCUMENT LoadWithPassword(const std::wstring& file, const wchar_t* pwd)
{
  if (pwd && pwd[0])
    return FPDF_LoadDocument(Utf8(file).c_str(), Utf8(pwd).c_str());
  return FPDF_LoadDocument(Utf8(file).c_str(), nullptr);
}

static void LoadDoc(const std::wstring& file)
{
  FPDF_DOCUMENT d = LoadWithPassword(file, nullptr);
  if (!d && FPDF_GetLastError() == FPDF_ERR_PASSWORD)
  {
    for (int attempt = 0; attempt < 3 && !d; ++attempt)
    {
      std::wstring pwd;
      if (!PromptPassword(pwd)) return;
      d = LoadWithPassword(file, pwd.c_str());
    }
    if (!d)
    {
      std::wstring msg = (FPDF_GetLastError() == FPDF_ERR_PASSWORD)
        ? L"The password was incorrect (3 attempts)."
        : L"Could not open the file after unlocking.\nPDFium error: "
          + std::to_wstring(FPDF_GetLastError());
      MessageBoxW(g.frame, msg.c_str(), L"Stitchup", MB_OK | MB_ICONERROR);
      return;
    }
  }
  if (!d)
  {
    unsigned long e = FPDF_GetLastError();
    std::wstring msg = L"Could not open file.\nPDFium error: " + std::to_wstring(e);
    MessageBoxW(g.frame, msg.c_str(), L"Stitchup", MB_OK | MB_ICONERROR);
    return;
  }
  CloseDoc();
  g.doc = d;
  g.path = file;
  g.bmDirty = true;
  size_t p = file.find_last_of(L"\\/");
  g.name = (p == std::wstring::npos) ? file : file.substr(p + 1);
  SetWindowTextW(g.frame, (g.name + L" - Stitchup PDF Editor").c_str());
  RefreshState();
  double mx = std::max(1.0, MaxPageW());
  RECT rc{};
  if (g.canvas) GetClientRect(g.canvas, &rc);
  double cw = std::max(200, (int)(rc.right - rc.left));
  g.zoom = std::min(2.5, std::max(0.35, (cw - 60.0) / mx));
  g.scrollX = g.scrollY = 0;
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
  SetFocus(g.frame);
}

static void NewDoc()
{
  CloseDoc();
  g.doc = FPDF_CreateNewDocument();
  FPDFPage_New(g.doc, 0, 612.0, 792.0);
  SetWindowTextW(g.frame, L"Stitchup PDF Editor");
  RefreshState();
  g.bmDirty = true;
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
  SetFocus(g.frame);
}

static bool DocToFile(FPDF_DOCUMENT d, const std::wstring& target)
{
  if (!d) return false;
  FileWriter fw{};
  fw.buf.reserve(65536);
  fw.base.version = 1;
  fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
  {
    FileWriter* fw = reinterpret_cast<FileWriter*>(self);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    fw->buf.insert(fw->buf.end(), p, p + size);
    return 1;
  };
  if (!FPDF_SaveAsCopy(d, &fw.base, FPDF_NO_INCREMENTAL))
    return false;
  std::wstring tmp = target + L".tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) return false;
  bool ok = fwrite(fw.buf.data(), 1, fw.buf.size(), f) == fw.buf.size();
  fclose(f);
  if (!ok) { DeleteFileW(tmp.c_str()); return false; }
  if (!MoveFileExW(tmp.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

static std::wstring PageTextRaw(FPDF_PAGE page)
{
  std::wstring out;
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
  if (!tp) return out;
  const int n = FPDFText_CountChars(tp);
  if (n > 0)
  {
    out.resize(static_cast<size_t>(n) + 1);
    const int got = FPDFText_GetText(tp, 0, n,
                                     reinterpret_cast<unsigned short*>(&out[0]));
    if (got > 0) out.resize(static_cast<size_t>(got));
    else out.clear();
  }
  FPDFText_ClosePage(tp);
  return out;
}

static std::wstring DocTextRaw(FPDF_DOCUMENT d)
{
  std::wstring all;
  if (!d) return all;
  const int np = FPDF_GetPageCount(d);
  for (int i = 0; i < np; i++)
  {
    FPDF_PAGE p = FPDF_LoadPage(d, i);
    if (!p) continue;
    std::wstring t = PageTextRaw(p);
    FPDF_ClosePage(p);
    if (!all.empty() || !t.empty()) all += L"\r\n";
    all += t;
  }
  return all;
}

static bool ExportTextToFile(FPDF_DOCUMENT d, const std::wstring& target)
{
  std::wstring all = DocTextRaw(d);
  if (all.empty()) return false;
  const int sz = WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(),
                                     nullptr, 0, nullptr, nullptr);
  if (sz <= 0) return false;
  std::vector<unsigned char> buf(static_cast<size_t>(sz) + 3);
  buf[0] = 0xEF; buf[1] = 0xBB; buf[2] = 0xBF;
  WideCharToMultiByte(CP_UTF8, 0, all.c_str(), (int)all.size(),
                      reinterpret_cast<char*>(&buf[3]), sz, nullptr, nullptr);
  FILE* f = nullptr;
  if (_wfopen_s(&f, target.c_str(), L"wb") != 0) return false;
  bool ok = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
  fclose(f);
  return ok;
}

static void ExportTextAll()
{
  if (!g.doc)
  {
    MessageBoxW(g.frame, L"No document open.", L"Export Text", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring all = DocTextRaw(g.doc);
  if (all.empty())
  {
    MessageBoxW(g.frame, L"No extractable text in this document.",
                L"Export Text", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring target = g.path;
  const size_t dot = target.find_last_of(L'.');
  if (dot != std::wstring::npos) target.resize(dot);
  target += L".txt";
  wchar_t buf[MAX_PATH];
  wcscpy_s(buf, target.c_str());
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.frame;
  ofn.lpstrFilter = L"Text Files (*.txt)\0*.txt\0All Files\0*.*\0\0";
  ofn.lpstrDefExt = L"txt";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (GetSaveFileNameW(&ofn))
  {
    if (ExportTextToFile(g.doc, buf))
      MessageBoxW(g.frame, (std::wstring(L"Exported text to:\n") + buf).c_str(),
                  L"Export Text", MB_OK | MB_ICONINFORMATION);
    else
      MessageBoxW(g.frame, L"Export failed.", L"Export Text", MB_OK | MB_ICONERROR);
  }
}

static std::wstring DocCsv(FPDF_DOCUMENT d)
{
  std::wstring csv = L"Page,Width (pt),Height (pt),Text chars,Annotations\r\n";
  if (!d) return csv;
  const int np = FPDF_GetPageCount(d);
  for (int i = 0; i < np; i++)
  {
    FPDF_PAGE p = FPDF_LoadPage(d, i);
    if (!p) continue;
    float L, B, R2, T;
    FPDFPage_GetMediaBox(p, &L, &B, &R2, &T);
    const float w = R2 - L, h = T - B;
    const int tchars = (int)PageTextRaw(p).size();
    const int anns = FPDFPage_GetAnnotCount(p);
    FPDF_ClosePage(p);
    wchar_t row[96];
    swprintf_s(row, L"%d,%.1f,%.1f,%d,%d\r\n",
               i + 1, (double)w, (double)h, tchars, anns);
    csv += row;
  }
  return csv;
}

static bool ExportCsvToFile(FPDF_DOCUMENT d, const std::wstring& target)
{
  if (!d || FPDF_GetPageCount(d) < 1) return false;
  std::wstring csv = DocCsv(d);
  if (csv.empty()) return false;
  const int sz = WideCharToMultiByte(CP_UTF8, 0, csv.c_str(), (int)csv.size(),
                                     nullptr, 0, nullptr, nullptr);
  if (sz <= 0) return false;
  std::vector<unsigned char> buf(static_cast<size_t>(sz) + 3);
  buf[0] = 0xEF; buf[1] = 0xBB; buf[2] = 0xBF;
  WideCharToMultiByte(CP_UTF8, 0, csv.c_str(), (int)csv.size(),
                      reinterpret_cast<char*>(&buf[3]), sz, nullptr, nullptr);
  FILE* f = nullptr;
  if (_wfopen_s(&f, target.c_str(), L"wb") != 0) return false;
  bool ok = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
  fclose(f);
  return ok;
}

static void ExportCsvAll()
{
  if (!g.doc)
  {
    MessageBoxW(g.frame, L"No document open.", L"Export CSV", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (FPDF_GetPageCount(g.doc) < 1)
  {
    MessageBoxW(g.frame, L"No pages to export.", L"Export CSV", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring target = g.path;
  const size_t dot = target.find_last_of(L'.');
  if (dot != std::wstring::npos) target.resize(dot);
  target += L".csv";
  wchar_t buf[MAX_PATH];
  wcscpy_s(buf, target.c_str());
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.frame;
  ofn.lpstrFilter = L"CSV Files (*.csv)\0*.csv\0All Files\0*.*\0\0";
  ofn.lpstrDefExt = L"csv";
  ofn.lpstrFile = buf;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
  if (GetSaveFileNameW(&ofn))
  {
    if (ExportCsvToFile(g.doc, buf))
      MessageBoxW(g.frame, (std::wstring(L"Exported CSV to:\n") + buf).c_str(),
                  L"Export CSV", MB_OK | MB_ICONINFORMATION);
    else
      MessageBoxW(g.frame, L"Export failed.", L"Export CSV", MB_OK | MB_ICONERROR);
  }
}

// ---------------------------------------------------------------------------
// Watermark
// ---------------------------------------------------------------------------
enum { WMDP_CENTER = 0, WMDP_TOP = 1, WMDP_TILED = 2 };

static bool ApplyWatermarkDoc(FPDF_DOCUMENT doc, const wchar_t* text,
                              float sizePts, int mode)
{
  if (!doc || !text || !text[0] || sizePts <= 0) return false;
  CheckLib();
  const int np = FPDF_GetPageCount(doc);
  if (np < 1) return false;
  const double rad = 45.0 * 3.14159265358979323846 / 180.0;
  const float co = static_cast<float>(cos(rad));
  const float si = static_cast<float>(sin(rad));
  const unsigned short* u16 = reinterpret_cast<const unsigned short*>(text);
  bool any = false;
  for (int i = 0; i < np; ++i)
  {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) continue;
    const float pw = FPDF_GetPageWidthF(page);
    const float ph = FPDF_GetPageHeightF(page);
    if (pw <= 0 || ph <= 0) { FPDF_ClosePage(page); continue; }
    int inserted = 0;
    auto placeOne = [&](float tx, float ty, bool rotate) {
      FPDF_PAGEOBJECT obj = FPDFPageObj_NewTextObj(doc, "Helvetica", sizePts);
      if (!obj) return;
      if (!FPDFText_SetText(obj, u16)) { FPDFPageObj_Destroy(obj); return; }
      FS_MATRIX m{};
      if (rotate) { m.a = co; m.b = si; m.c = -si; m.d = co; }
      else { m.a = 1; m.d = 1; }
      FPDFPageObj_SetMatrix(obj, &m);
      float l = 0, b = 0, r = 0, t = 0;
      bool gb = FPDFPageObj_GetBounds(obj, &l, &b, &r, &t);
      if (gb)
      {
        m.e = tx - (l + r) / 2;
        m.f = ty - (b + t) / 2;
        FPDFPageObj_SetMatrix(obj, &m);
      }
      FPDFPageObj_SetFillColor(obj, 140, 140, 140, 128);
      FPDFPage_InsertObject(page, obj);
      ++inserted;
    };
    if (mode == WMDP_TILED)
    {
      const float step = sizePts * 2.5f;
      for (float gx = -ph; gx < pw + ph; gx += step)
        for (float gy = -pw; gy < ph + pw; gy += step)
          placeOne(gx, gy, true);
    }
    else if (mode == WMDP_TOP)
    {
      placeOne(pw / 2, ph - 30, false);
    }
    else
    {
      placeOne(pw / 2, ph / 2, true);
    }
    if (inserted > 0 && FPDFPage_GenerateContent(page)) any = true;
    FPDF_ClosePage(page);
  }
  return any;
}

struct WmCtx
{
  HWND edit = nullptr, size = nullptr, pos = nullptr;
  bool ok = false;
  std::wstring text;
  int sizeSel = 2;   // default 36 pt
  int posSel = 0;    // default center diagonal
};
static const int kWmSizes[] = { 16, 24, 36, 48, 64, 96 };

static LRESULT CALLBACK WmProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_CREATE:
    {
      WmCtx* ctx = reinterpret_cast<WmCtx*>(
        reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      CreateWindowExW(0, L"STATIC", L"Watermark text:",
                      WS_CHILD | WS_VISIBLE, 16, 14, 140, 16, h, nullptr, g.inst, nullptr);
      ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                  16, 32, 312, 24, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"STATIC", L"Size (pt):",
                      WS_CHILD | WS_VISIBLE, 16, 66, 120, 16, h, nullptr, g.inst, nullptr);
      ctx->size = CreateWindowExW(0, L"COMBOBOX", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                  140, 64, 120, 130, h, nullptr, g.inst, nullptr);
      for (int n : kWmSizes)
      {
        std::wstring s = std::to_wstring(n);
        SendMessageW(ctx->size, CB_ADDSTRING, 0, (LPARAM)s.c_str());
      }
      SendMessageW(ctx->size, CB_SETCURSEL, ctx->sizeSel, 0);
      CreateWindowExW(0, L"STATIC", L"Position:",
                      WS_CHILD | WS_VISIBLE, 16, 98, 100, 16, h, nullptr, g.inst, nullptr);
      ctx->pos = CreateWindowExW(0, L"COMBOBOX", L"",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
                                 140, 96, 160, 130, h, nullptr, g.inst, nullptr);
      SendMessageW(ctx->pos, CB_ADDSTRING, 0, (LPARAM)L"Center (diagonal)");
      SendMessageW(ctx->pos, CB_ADDSTRING, 0, (LPARAM)L"Top center");
      SendMessageW(ctx->pos, CB_ADDSTRING, 0, (LPARAM)L"Tiled (diagonal)");
      SendMessageW(ctx->pos, CB_SETCURSEL, ctx->posSel, 0);
      CreateWindowExW(0, L"BUTTON", L"Ok",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      100, 136, 74, 28, h, (HMENU)1, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel",
                      WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      186, 136, 74, 28, h, (HMENU)2, g.inst, nullptr);
      SetFocus(ctx->edit);
      SetWindowTextW(ctx->edit, L"Confidential");
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(w) == 1 || LOWORD(w) == 2)
      {
        WmCtx* ctx = reinterpret_cast<WmCtx*>(
          GetWindowLongPtrW(h, GWLP_USERDATA));
        if (ctx)
        {
          if (LOWORD(w) == 1)
          {
            wchar_t buf[512] = { 0 };
            ctx->text = GetWindowTextW(ctx->edit, buf, 512) > 0 ? buf : L"";
            ctx->sizeSel = (int)SendMessageW(ctx->size, CB_GETCURSEL, 0, 0);
            ctx->posSel = (int)SendMessageW(ctx->pos, CB_GETCURSEL, 0, 0);
          }
          ctx->ok = (LOWORD(w) == 1);
        }
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_CLOSE:
    {
      WmCtx* ctx = reinterpret_cast<WmCtx*>(
        GetWindowLongPtrW(h, GWLP_USERDATA));
      if (ctx) ctx->ok = false;
      DestroyWindow(h);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
      return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
  }
  return DefWindowProcW(h, m, w, l);
}

static bool PromptWatermark(std::wstring& text, float& sizePts, int& mode)
{
  const wchar_t cls[] = L"SKWmWnd";
  static bool reg = false;
  if (!reg)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WmProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    reg = true;
  }
  WmCtx ctx;
  HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Watermark",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 352, 212,
                            g.frame, nullptr, g.inst, &ctx);
  if (!hw) return false;
  RECT fr, rc;
  GetWindowRect(g.frame, &fr);
  GetWindowRect(hw, &rc);
  SetWindowPos(hw, nullptr,
               fr.left + (fr.right - fr.left - (rc.right - rc.left)) / 2,
               fr.top + (fr.bottom - fr.top - (rc.bottom - rc.top)) / 2,
               0, 0, SWP_NOSIZE | SWP_NOZORDER);
  ShowWindow(hw, SW_SHOW);
  UpdateWindow(hw);
  HWND owner = g.frame;
  EnableWindow(owner, FALSE);
  MSG msg;
  while (IsWindow(hw))
  {
    const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
    if (r <= 0) break;
    if (!IsDialogMessageW(hw, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  SetFocus(owner);
  text = ctx.text;
  if (ctx.sizeSel >= 0 && ctx.sizeSel < 6) sizePts = (float)kWmSizes[ctx.sizeSel];
  else sizePts = 36.0f;
  mode = (ctx.posSel == 1) ? WMDP_TOP : (ctx.posSel == 2) ? WMDP_TILED : WMDP_CENTER;
  return ctx.ok;
}

static void WatermarkCurrentDoc()
{
  static bool active = false;
  if (active) return;
  active = true;
  struct WmGuard
  {
    bool& f;
    ~WmGuard() { f = false; }
  } guard{active};
  if (!g.doc || FPDF_GetPageCount(g.doc) < 1)
  {
    MessageBoxW(g.frame, L"No document open.", L"Watermark", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring text;
  float sizePts = 36.0f;
  int mode = WMDP_CENTER;
  if (!PromptWatermark(text, sizePts, mode)) return;
  if (text.empty())
  {
    MessageBoxW(g.frame, L"Watermark text is empty.", L"Watermark",
                MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (!ApplyWatermarkDoc(g.doc, text.c_str(), sizePts, mode))
  {
    MessageBoxW(g.frame, L"Could not apply watermark.", L"Watermark",
                MB_OK | MB_ICONERROR);
    return;
  }
  g.dirty = true;
  RefreshState();
  for (auto& kv : g.thumbCache) DeleteObject(kv.second);
  g.thumbCache.clear();
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

static bool SaveDocTo(const std::wstring& target)
{
  if (!g.doc) return false;
  FileWriter fw{};
  fw.buf.reserve(65536);
  fw.base.version = 1;
  fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
  {
    FileWriter* fw = reinterpret_cast<FileWriter*>(self);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    fw->buf.insert(fw->buf.end(), p, p + size);
    return 1;
  };
  if (!FPDF_SaveAsCopy(g.doc, &fw.base, FPDF_NO_INCREMENTAL))
    return false;

  std::wstring tmp = target + L".tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) return false;
  bool ok = fwrite(fw.buf.data(), 1, fw.buf.size(), f) == fw.buf.size();
  fclose(f);
  if (!ok) { DeleteFileW(tmp.c_str()); return false; }
  if (!MoveFileExW(tmp.c_str(), target.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DWORD err = GetLastError();
    std::wstring msg = L"Save failed: could not finalize file.\n"
                       L"A recovery copy was kept at:\n" + tmp;
    if (err != ERROR_FILE_NOT_FOUND) DeleteFileW(tmp.c_str());
    MessageBoxW(g.frame, msg.c_str(), L"Stitchup", MB_OK | MB_ICONWARNING);
    return false;
  }
  return true;
}

static void SaveInPlace();

static void SaveAs()
{
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.frame;
  ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"pdf";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (!GetSaveFileNameW(&ofn)) return;
  if (file == g.path)
  {
    SaveInPlace();
    return;
  }
  if (SaveDocTo(file))
  {
    std::wstring f = file;
    g.path = f;
    g.dirty = false;
    size_t p = f.find_last_of(L"\\/");
    g.name = (p == std::wstring::npos) ? f : f.substr(p + 1);
    SetWindowTextW(g.frame, (g.name + L" - Stitchup PDF Editor").c_str());
    RefreshState();
    InvalidateRect(g.status, nullptr, TRUE);
  }
}

static void SaveInPlace()
{
  if (g.path.empty()) { SaveAs(); return; }
  if (!g.doc) return;
  std::wstring target = g.path;
  FileWriter fw{};
  fw.buf.reserve(65536);
  fw.base.version = 1;
  fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
  {
    FileWriter* fw2 = reinterpret_cast<FileWriter*>(self);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    fw2->buf.insert(fw2->buf.end(), p, p + size);
    return 1;
  };
  if (!FPDF_SaveAsCopy(g.doc, &fw.base, FPDF_NO_INCREMENTAL))
  {
    MessageBoxW(g.frame, L"Save failed: the document could not be serialized.",
                L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  std::wstring tmp = target + L".tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) return;
  bool okw = fwrite(fw.buf.data(), 1, fw.buf.size(), f) == fw.buf.size();
  fclose(f);
  if (!okw) { DeleteFileW(tmp.c_str()); return; }
  // PDFium keeps the source file handle open; release it before replacing the
  // file on disk (otherwise the atomic replace fails with a sharing violation).
  CloseDoc();
  if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING))
  {
    std::wstring msg = L"Save failed: could not finalize the file.\n"
                       L"A recovery copy was kept at:\n" + tmp;
    MessageBoxW(g.frame, msg.c_str(), L"Stitchup", MB_OK | MB_ICONWARNING);
    LoadDoc(target);  // restore the pre-save file (unsaved edits are lost)
    return;
  }
  g.dirty = false;
  LoadDoc(target);
  InvalidateRect(g.status, nullptr, TRUE);
}

static bool SaveAsString(FPDF_DOCUMENT d, std::vector<unsigned char>& out);
static std::vector<unsigned char> EncryptPdfBytes(
    const std::vector<unsigned char>& plain,
    const std::string& userPw, const std::string& ownerPw);

// Password-setting dialog for "Save As Encrypted...".
static HWND g_epwEdit1 = nullptr;
static HWND g_epwEdit2 = nullptr;
static HWND g_epwEdit3 = nullptr;
static bool g_epwOk = false;
static std::wstring g_epwUser, g_epwOwner;

static LRESULT CALLBACK EpwProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_CREATE:
      CreateWindowExW(0, L"STATIC",
        L"Save a password-protected copy of this PDF.",
        WS_CHILD | WS_VISIBLE, 16, 12, 312, 18, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"STATIC", L"User password:",
        WS_CHILD | WS_VISIBLE, 16, 38, 128, 18, h, nullptr, g.inst, nullptr);
      g_epwEdit1 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        150, 36, 178, 24, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"STATIC", L"Confirm password:",
        WS_CHILD | WS_VISIBLE, 16, 68, 128, 18, h, nullptr, g.inst, nullptr);
      g_epwEdit2 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        150, 66, 178, 24, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"STATIC", L"Owner password (optional):",
        WS_CHILD | WS_VISIBLE, 16, 98, 128, 18, h, nullptr, g.inst, nullptr);
      g_epwEdit3 = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_PASSWORD | ES_AUTOHSCROLL,
        150, 96, 178, 24, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Ok", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        100, 136, 74, 28, h, (HMENU)1, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        186, 136, 74, 28, h, (HMENU)2, g.inst, nullptr);
      SetFocus(g_epwEdit1);
      return 0;
    case WM_COMMAND:
      if (LOWORD(w) == 1 || LOWORD(w) == 2)
      {
        if (LOWORD(w) == 1)
        {
          wchar_t b1[256], b2[256], b3[256];
          GetWindowTextW(g_epwEdit1, b1, 256);
          GetWindowTextW(g_epwEdit2, b2, 256);
          GetWindowTextW(g_epwEdit3, b3, 256);
          if (b1[0] == 0)
          {
            MessageBoxW(h, L"User password cannot be empty.", L"Stitchup",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          if (wcscmp(b1, b2) != 0)
          {
            MessageBoxW(h, L"Passwords do not match.", L"Stitchup",
                        MB_OK | MB_ICONINFORMATION);
            return 0;
          }
          g_epwUser = b1;
          g_epwOwner = b3;
        }
        g_epwOk = (LOWORD(w) == 1);
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_CLOSE:
      g_epwOk = false;
      DestroyWindow(h);
      return 0;
    case WM_CTLCOLORSTATIC:
      return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
  }
  return DefWindowProcW(h, m, w, l);
}

static bool PromptSetPassword(std::wstring& userOut, std::wstring& ownerOut)
{
  const wchar_t cls[] = L"SKEpwWnd";
  static bool reg = false;
  if (!reg)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = EpwProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    reg = true;
  }
  g_epwEdit1 = g_epwEdit2 = g_epwEdit3 = nullptr;
  g_epwOk = false;
  g_epwUser.clear();
  g_epwOwner.clear();
  HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Set password",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 348, 200,
                            g.frame, nullptr, g.inst, nullptr);
  if (!hw) return false;
  RECT fr, rc;
  GetWindowRect(g.frame, &fr);
  GetWindowRect(hw, &rc);
  SetWindowPos(hw, nullptr,
               fr.left + (fr.right - fr.left - (rc.right - rc.left)) / 2,
               fr.top + (fr.bottom - fr.top - (rc.bottom - rc.top)) / 2,
               0, 0, SWP_NOSIZE | SWP_NOZORDER);
  ShowWindow(hw, SW_SHOW);
  UpdateWindow(hw);
  HWND owner = g.frame;
  EnableWindow(owner, FALSE);
  MSG msg;
  while (IsWindow(hw))
  {
    const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
    if (r <= 0) break;
    if (!IsDialogMessageW(hw, &msg))
    {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
  }
  EnableWindow(owner, TRUE);
  SetActiveWindow(owner);
  SetFocus(owner);
  userOut = g_epwUser;
  ownerOut = g_epwOwner;
  return g_epwOk;
}

static void SaveAsEncrypted()
{
  if (!g.doc)
  {
    MessageBoxW(g.frame, L"No document open.", L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring userPw, ownerPw;
  if (!PromptSetPassword(userPw, ownerPw)) return;
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.frame;
  ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"pdf";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (!GetSaveFileNameW(&ofn)) return;
  if (file == g.path)
  {
    MessageBoxW(g.frame,
                L"Choose a different file name: the open document is not replaced.\n"
                L"The encrypted copy is saved separately.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::vector<unsigned char> buf;
  if (!SaveAsString(g.doc, buf))
  {
    MessageBoxW(g.frame, L"Encrypted save failed: the document could not be serialized.",
                L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  std::vector<unsigned char> enc = EncryptPdfBytes(buf, Utf8(userPw), Utf8(ownerPw));
  if (enc.empty())
  {
    MessageBoxW(g.frame, L"Encrypted save failed: could not build the encrypted copy.",
                L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  std::wstring tmp = std::wstring(file) + L".tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0)
  {
    MessageBoxW(g.frame, L"Encrypted save failed: could not write the file.",
                L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  bool ok = fwrite(enc.data(), 1, enc.size(), f) == enc.size();
  fclose(f);
  if (!ok) { DeleteFileW(tmp.c_str()); MessageBoxW(g.frame, L"Encrypted save failed: disk write error.", L"Stitchup", MB_OK | MB_ICONWARNING); return; }
  if (!MoveFileExW(tmp.c_str(), file, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
  {
    DWORD err = GetLastError();
    std::wstring msg = L"Encrypted save failed: could not finalize the file.\n"
                       L"A recovery copy was kept at:\n" + tmp;
    if (err != ERROR_FILE_NOT_FOUND) DeleteFileW(tmp.c_str());
    MessageBoxW(g.frame, msg.c_str(), L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  MessageBoxW(g.frame,
              L"Saved encrypted PDF.\nThe open document is unchanged; reopening this "
              L"file will ask for the password.",
              L"Stitchup", MB_OK | MB_ICONINFORMATION);
}

static void ImportPdf()
{
  wchar_t file[MAX_PATH] = L"";
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.frame;
  ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"pdf";
  ofn.Flags = OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (!GetOpenFileNameW(&ofn)) return;

  FPDF_DOCUMENT src = FPDF_LoadDocument(Utf8(file).c_str(), nullptr);
  if (!src)
  {
    MessageBoxW(g.frame, L"Could not open the PDF to import.",
                L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  CheckLib();
  bool ok = FPDF_ImportPagesByIndex(g.doc, src, nullptr, 0, g.pageCount);
  FPDF_CloseDocument(src);
  if (!ok)
  {
    MessageBoxW(g.frame, L"Page import failed.", L"Stitchup", MB_OK | MB_ICONWARNING);
    return;
  }
  g.dirty = true;
  RefreshState();
  g.bmDirty = true;
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

static void DeletePage()
{
  if (g.pageCount == 0) return;
  if (MessageBoxW(g.frame,
                  (L"Delete page " + std::to_wstring(g.selected + 1) + L"?")
                      .c_str(),
                  L"Stitchup", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  FPDFPage_Delete(g.doc, g.selected);
  g.dirty = true;
  RefreshState();
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

static void AddPage()
{
  CheckLib();
  int r = MessageBoxW(g.frame, L"Blank page size?\nYes = Letter (8.5x11 in)\nNo = A4\nCancel = abort",
                      L"Add Page", MB_YESNOCANCEL | MB_ICONQUESTION);
  if (r == IDCANCEL) return;
  double w = (r == IDYES) ? 612.0 : 595.0;
  double h = (r == IDYES) ? 792.0 : 842.0;
  if (FPDFPage_New(g.doc, g.selected + 1, w, h))
  {
    g.dirty = true;
    RefreshState();
    g.selected = (g.selected + 1 < g.pageCount) ? g.selected + 1 : g.pageCount - 1;
    InvalidateRect(g.canvas, nullptr, TRUE);
    InvalidateRect(g.thumbs, nullptr, TRUE);
  }
}

static void RotatePage(int turns)
{
  if (g.pageCount == 0) return;
  CheckLib();
  FPDF_PAGE p = FPDF_LoadPage(g.doc, g.selected);
  if (!p) return;
  int r = (FPDFPage_GetRotation(p) + turns) & 3;
  FPDFPage_SetRotation(p, r);
  FPDF_ClosePage(p);
  g.dirty = true;
  // refresh affected caches
  auto c = g.canvasCache.find(g.selected);
  if (c != g.canvasCache.end()) { DeleteObject(c->second.bmp); g.canvasCache.erase(c); }
  auto t = g.thumbCache.find(g.selected);
  if (t != g.thumbCache.end()) { DeleteObject(t->second); g.thumbCache.erase(t); }
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

static void UpdateScrollbars();

static void ExtractCurrentPage()
{
  if (!g.doc || g.pageCount == 0) return;
  wchar_t file[MAX_PATH] = L"";
  std::wstring base = g.name.empty() ? L"extract" : (g.name.size() > 4 && _wcsicmp(g.name.c_str() + g.name.size() - 4, L".pdf") == 0
                                                      ? g.name.substr(0, g.name.size() - 4) : g.name);
  std::wstring def = base + L" - extract.pdf";
  wcsncpy(file, def.c_str(), MAX_PATH - 1);
  file[MAX_PATH - 1] = 0;
  OPENFILENAMEW ofn{};
  ofn.lStructSize = sizeof(ofn);
  ofn.hwndOwner = g.frame;
  ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0\0";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.lpstrDefExt = L"pdf";
  ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
  if (!GetSaveFileNameW(&ofn)) return;
  int idx = g.selected;
  FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
  bool ok = nd && FPDF_ImportPagesByIndex(nd, g.doc, &idx, 1, 0) != 0;
  if (ok) ok = DocToFile(nd, file);
  if (nd) FPDF_CloseDocument(nd);
  if (ok)
    MessageBoxW(g.frame, (L"Extracted page " + std::to_wstring(idx + 1) + L" to:\n" +
                          std::wstring(file)).c_str(),
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
  else
    MessageBoxW(g.frame, L"Extract failed.", L"Stitchup", MB_OK | MB_ICONWARNING);
}

static void SplitAllPages()
{
  if (!g.doc || g.pageCount == 0) return;
  std::wstring prefix;
  if (!g.path.empty() && g.path.size() > 4 &&
      _wcsicmp(g.path.c_str() + g.path.size() - 4, L".pdf") == 0)
    prefix = g.path.substr(0, g.path.size() - 4) + L" - split";
  else
    prefix = L"Stitchup - split";
  std::wstring dir = prefix.substr(0, prefix.find_last_of(L"\\/") + 1);
  int done = 0, failed = 0;
  for (int i = 0; i < g.pageCount; ++i)
  {
    int idx = i;
    FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
    bool ok = nd && FPDF_ImportPagesByIndex(nd, g.doc, &idx, 1, 0) != 0;
    if (ok) ok = DocToFile(nd, prefix + L" - page " + std::to_wstring(i + 1) + L".pdf");
    if (nd) FPDF_CloseDocument(nd);
    ok ? ++done : ++failed;
  }
  MessageBoxW(g.frame,
              (L"Split complete: " + std::to_wstring(done) + L" page" +
               (done == 1 ? L"" : L"s") + L" written to:\n" + dir +
               (failed ? (L"\nFailed: " + std::to_wstring(failed)) : L"")).c_str(),
              L"Stitchup", MB_OK | MB_ICONINFORMATION);
}

static bool InkBounds(FPDF_PAGE page, float& L, float& B, float& R, float& T)
{
  double w = FPDF_GetPageWidth(page);
  double h = FPDF_GetPageHeight(page);
  int bw = (int)std::ceil(w), bh = (int)std::ceil(h);
  if (bw < 1 || bh < 1) return false;
  int stride = ((bw * 32 + 31) / 32) * 4;
  std::vector<unsigned char> px((size_t)bh * stride, 0xFF);
  FPDF_BITMAP fb = FPDFBitmap_CreateEx(bw, bh, FPDFBitmap_BGRA, px.data(), stride);
  if (!fb) return false;
  FPDFBitmap_FillRect(fb, 0, 0, bw, bh, 0xFFFFFFFF);
  FPDF_RenderPageBitmap(fb, page, 0, 0, bw, bh, 0, 0);
  int minX = bw, minY = bh, maxX = -1, maxY = -1;
  for (int yy = 0; yy < bh; ++yy)
  {
    const unsigned char* row = px.data() + (size_t)yy * stride;
    for (int xx = 0; xx < bw; ++xx)
    {
      const unsigned char* p4 = row + (size_t)xx * 4;
      if (p4[0] < 250 || p4[1] < 250 || p4[2] < 250)
      {
        minX = std::min(minX, xx); maxX = std::max(maxX, xx);
        minY = std::min(minY, yy); maxY = std::max(maxY, yy);
      }
    }
  }
  FPDFBitmap_Destroy(fb);
  if (maxX < minX || maxY < minY) return false;
  // render is top-left origin; PDF page coordinates are bottom-left (y up)
  L = (float)minX;
  R = (float)(maxX + 1);
  T = (float)(h - minY);
  B = (float)(h - (maxY + 1));
  return true;
}

static void CropCurrentPageToContent()
{
  if (!g.doc || g.pageCount == 0) return;
  CheckLib();
  FPDF_PAGE p = FPDF_LoadPage(g.doc, g.selected);
  if (!p) return;
  float L, B, R, T;
  if (!InkBounds(p, L, B, R, T))
  {
    FPDF_ClosePage(p);
    MessageBoxW(g.frame, L"This page has no content to trim to.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  FPDF_ClosePage(p);
  FPDF_PAGE pp = FPDF_LoadPage(g.doc, g.selected);
  if (!pp) return;
  float mL, mB, mR, mT;
  FPDFPage_GetMediaBox(pp, &mL, &mB, &mR, &mT);
  L = std::max(L, mL); T = std::min(T, mT);
  R = std::min(R, mR); B = std::max(B, mB);
  if (L < R && B < T)
  {
    FPDFPage_SetMediaBox(pp, L, B, R, T);
    g.dirty = true;
  }
  FPDF_ClosePage(pp);
  auto c = g.canvasCache.find(g.selected);
  if (c != g.canvasCache.end()) { DeleteObject(c->second.bmp); g.canvasCache.erase(c); }
  auto t = g.thumbCache.find(g.selected);
  if (t != g.thumbCache.end()) { DeleteObject(t->second); g.thumbCache.erase(t); }
  UpdateScrollbars();
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

static void ReorderDoc(int from, int to)
{
  if (from == to || g.pageCount < 2) return;
  std::vector<int> order;
  order.reserve(g.pageCount);
  for (int i = 0; i < g.pageCount; ++i) order.push_back(i);
  int moving = order[from];
  order.erase(order.begin() + from);
  if (to >= (int)order.size()) to = (int)order.size() - 1;
  order.insert(order.begin() + (to < from ? to : to), moving);

  std::map<int, int> rot;
  for (int i = 0; i < g.pageCount; ++i)
  {
    FPDF_PAGE p = FPDF_LoadPage(g.doc, i);
    if (p) { rot[i] = FPDFPage_GetRotation(p); FPDF_ClosePage(p); }
  }

  FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
  for (int k = 0; k < (int)order.size(); ++k)
  {
    int idx = order[k];
    FPDF_ImportPagesByIndex(nd, g.doc, &idx, 1, k);
  }
  CloseDoc();
  g.doc = nd;
  RefreshState();

  for (int k = 0; k < g.pageCount; ++k)
  {
    FPDF_PAGE p = FPDF_LoadPage(g.doc, k);
    if (p)
    {
      int srcIdx = order[k];
      FPDFPage_SetRotation(p, rot[srcIdx]);
      FPDF_ClosePage(p);
    }
  }
  g.dirty = true;
  g.selected = order.size() ? 0 : 0;
  g.bmDirty = true;
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Layout helpers
// ---------------------------------------------------------------------------
static void UpdateScrollbars()
{
  if (!g.canvas) return;
  RECT rc{};
  GetClientRect(g.canvas, &rc);
  int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
  double s = g.zoom;
  double span = std::max(1.0, MaxPageW()) * s;
  int contentW = (int)std::ceil(span) + 32;
  int th = 0;
  for (int i = 0; i < g.pageCount; ++i)
    th += (int)std::ceil(PageH(i) * s) + 14;
  int contentH = th + 24;

  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  si.nMin = 0;
  si.nMax = std::max(cw, contentW);
  si.nPage = cw;
  if (g.scrollX > si.nMax - 1) g.scrollX = si.nMax - 1;
  si.nPos = g.scrollX;
  SetScrollInfo(g.canvas, SB_HORZ, &si, TRUE);

  si.nMax = std::max(ch, contentH);
  si.nPage = ch;
  if (g.scrollY > si.nMax - 1) g.scrollY = si.nMax - 1;
  si.nPos = g.scrollY;
  SetScrollInfo(g.canvas, SB_VERT, &si, TRUE);
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
static LRESULT CALLBACK StatusProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp,
                                   UINT_PTR, DWORD_PTR)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      RECT rc;
      GetClientRect(hw, &rc);
      const Theme& th = ThemeNow();
      HBRUSH bg = CreateSolidBrush(th.statusBg);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, th.statusTxt);
      std::wstring left = g.name.empty() ? L"Stitchup PDF Editor"
                                         : g.name + (g.dirty ? L"  *" : L"");
      RECT lrc = rc;
      lrc.left += 10;
      DrawTextW(dc, left.c_str(), -1, &lrc, DT_SINGLELINE | DT_VCENTER);
      std::wstring right =
        L"Page " + std::to_wstring(g.pageCount ? g.selected + 1 : 0) + L" of " +
        std::to_wstring(g.pageCount) + L"      Zoom " +
        std::to_wstring((int)std::lround(g.zoom * 100.0)) + L"%";
      RECT rrc = rc;
      rrc.right -= 10;
      SetTextAlign(dc, TA_RIGHT | TA_TOP);
      DrawTextW(dc, right.c_str(), -1, &rrc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
      EndPaint(hw, &ps);
      return 0;
    }
  }
  return DefSubclassProc(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Thumbnails panel
// ---------------------------------------------------------------------------
static int ThumbForY(int y)
{
  RECT rc;
  GetClientRect(g.thumbs, &rc);
  int w = rc.right - rc.left;
  int th = w - 24;
  int yc = 30; // header area + scroll offset handled by caller
  for (int i = 0; i < g.pageCount; ++i)
  {
    float pw = PageW(i), ph = PageH(i);
    if (pw < 1 || ph < 1) continue;
    double scale = (double)(th - 8) / (double)pw;
    scale = std::min(scale, 220.0 / (double)ph);
    int hi = (int)std::ceil(ph * scale) + 12;
    if (y >= yc && y < yc + hi) return i;
    yc += hi;
  }
  return -1;
}

static HBITMAP GetThumb(int i)
{
  auto it = g.thumbCache.find(i);
  if (it != g.thumbCache.end()) return it->second;
  float pw = PageW(i), ph = PageH(i);
  RECT rc;
  GetClientRect(g.thumbs, &rc);
  int w = (rc.right - rc.left) - 24;
  if (w < 40) w = 40;
  double scale = std::min((double)(w - 8) / pw, 220.0 / ph);
  int bw = (int)std::ceil(pw * scale);
  int bh = (int)std::ceil(ph * scale);
  HBITMAP hb = RenderPageBitmap(i, bw, bh);
  g.thumbCache[i] = hb;
  return hb;
}

static void ThumbScroll(int delta)
{
  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  GetScrollInfo(g.thumbs, SB_VERT, &si);
  si.nPos -= delta;
  si.nPos = std::max(si.nMin, std::min((int)si.nMax, si.nPos));
  si.fMask = SIF_POS;
  SetScrollInfo(g.thumbs, SB_VERT, &si, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
}

static LRESULT CALLBACK ThumbsProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;
    case WM_CREATE:
      ShowScrollBar(hw, SB_VERT, TRUE);
      return 0;
    case WM_SIZE:
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      InvalidateRect(hw, nullptr, TRUE);
      return 0;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      RECT rc;
      GetClientRect(hw, &rc);
      const Theme& thm = ThemeNow();
      HBRUSH bgb = CreateSolidBrush(thm.thumbBg);
      FillRect(dc, &rc, bgb);
      DeleteObject(bgb);

      RECT hr{rc.left + 10, 6, rc.right - 10, 26};
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, thm.textDim);
      DrawTextW(dc, L"Pages", -1, &hr, DT_SINGLELINE);

      int w = rc.right - rc.left;
      int x = 10;
      int thumbW = w - 26;
      if (thumbW < 50) thumbW = 50;

      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_POS | SIF_RANGE | SIF_PAGE;
      GetScrollInfo(hw, SB_VERT, &si);
      int top = -si.nPos;
      int yc = 34 + top;

      if (g.dragPage >= 0 && g.dragCursor == 0)
      {
        HPEN pn = CreatePen(PS_SOLID, 2, thm.accent);
        SelectObject(dc, pn);
        MoveToEx(dc, x, yc - 4, nullptr);
        LineTo(dc, x + thumbW, yc - 4);
        DeleteObject(pn);
      }

      for (int i = 0; i < g.pageCount; ++i)
      {
        float pw = PageW(i), ph = PageH(i);
        if (pw < 1 || ph < 1) continue;
        double scale = std::min((double)(thumbW - 8) / pw, 220.0 / ph);
        int tw = (int)std::ceil(pw * scale);
        int th = (int)std::ceil(ph * scale);
        RECT slot{x, yc, x + thumbW, yc + th + 14};
        if (slot.bottom > 0 && slot.top < rc.bottom)
        {
          HBITMAP hb = GetThumb(i);
          HDC mem = CreateCompatibleDC(dc);
          SelectObject(mem, hb);
          BitBlt(dc, x + 4, yc, tw, th, mem, 0, 0, SRCCOPY);
          DeleteDC(mem);
          bool sel = (i == g.selected);
          if (g.dragPage >= 0 && i == g.dragPage)
          {
            HBRUSH dim = CreateSolidBrush(thm.btnHover);
            RECT dr{x + 2, yc - 2, x + 4 + tw, yc + th + 6};
            FillRect(dc, &dr, dim);
            DeleteObject(dim);
          }
          RECT pr{x + 2, yc - 2, x + 4 + tw, yc + th + 6};
          HBRUSH phb = CreateSolidBrush(sel ? thm.accent
                                            : thm.pageFrame);
          FrameRect(dc, &pr, phb);
          DeleteObject(phb);
          std::wstring num = std::to_wstring(i + 1);
          RECT nr{x + 4, yc + th + 4, x + thumbW, yc + th + 14};
          SetTextColor(dc, sel ? thm.accent : thm.textDim);
          DrawTextW(dc, num.c_str(), -1, &nr, DT_SINGLELINE);
        }
        yc += th + 16;
        if (g.dragPage >= 0 && i + 1 == g.dragCursor)
        {
          HPEN pn = CreatePen(PS_SOLID, 2, thm.accent);
          SelectObject(dc, pn);
          MoveToEx(dc, x, yc - 4, nullptr);
          LineTo(dc, x + thumbW, yc - 4);
          DeleteObject(pn);
        }
      }
      EndPaint(hw, &ps);
      return 0;
    }
    case WM_VSCROLL:
    {
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(hw, SB_VERT, &si);
      int pos = si.nPos;
      switch (LOWORD(wp))
      {
        case SB_LINEUP: pos -= 24; break;
        case SB_LINEDOWN: pos += 24; break;
        case SB_PAGEUP: pos -= si.nPage; break;
        case SB_PAGEDOWN: pos += si.nPage; break;
        case SB_THUMBTRACK: pos = HIWORD(wp); break;
        case SB_TOP: pos = si.nMin; break;
        case SB_BOTTOM: pos = si.nMax; break;
      }
      pos = std::max(si.nMin, std::min((int)si.nMax, pos));
      si.nPos = pos;
      si.fMask = SIF_POS;
      SetScrollInfo(hw, SB_VERT, &si, TRUE);
      InvalidateRect(hw, nullptr, TRUE);
      return 0;
    }
    case WM_MOUSEWHEEL:
    {
      short d = GET_WHEEL_DELTA_WPARAM(wp);
      if (d != 0) ThumbScroll(d / WHEEL_DELTA * 40);
      return 0;
    }
    case WM_LBUTTONDOWN:
    {
      SetFocus(g.frame);
      SetCapture(hw);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_POS;
      GetScrollInfo(hw, SB_VERT, &si);
      int y = pt.y + si.nPos;
      int pi = ThumbForY(y);
      if (pi >= 0)
      {
        g.selected = pi;
        g.dragPage = pi;
        g.dragCursor = pi;
        InvalidateRect(hw, nullptr, TRUE);
        InvalidateRect(g.canvas, nullptr, TRUE);
        UpdateScrollbars();
      }
      return 0;
    }
    case WM_MOUSEMOVE:
    {
      if ((wp & MK_LBUTTON) && g.dragPage >= 0)
      {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        GetScrollInfo(hw, SB_VERT, &si);
        int y = pt.y + si.nPos;
        int target = ThumbForY(y);
        if (target < 0) target = g.pageCount - 1;
        if (target < 0) target = 0;
        int from = g.dragPage;
        int to = (target >= from) ? target + 1 : target;
        if (to != g.dragCursor)
        {
          g.dragCursor = to;
          InvalidateRect(hw, nullptr, TRUE);
        }
      }
      return 0;
    }
    case WM_LBUTTONUP:
    {
      ReleaseCapture();
      if (g.dragPage >= 0)
      {
        int from = g.dragPage;
        int to = g.dragCursor;
        g.dragPage = -1;
        g.dragCursor = -1;
        if (from >= 0 && to >= 0 && from != to && from != to - 1)
          ReorderDoc(from, to);
        InvalidateRect(hw, nullptr, TRUE);
      }
      return 0;
    }
    case WM_CANCELMODE:
      ReleaseCapture();
      g.dragPage = -1;
      g.dragCursor = -1;
      InvalidateRect(hw, nullptr, TRUE);
      return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Canvas
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Content object editing (Select / Move / Delete / Recolor / Edit text)
// ---------------------------------------------------------------------------
struct PageObject
{
  int index = -1;
  int type = 0;             // FPDF_PAGEOBJ_*
  float l = 0, b = 0, r = 0, t = 0;
  std::wstring text;        // TEXT only
  std::string fontName;     // TEXT only (base name, subset prefix stripped)
  float fontSize = 0;
  bool isText() const { return type == FPDF_PAGEOBJ_TEXT; }
  bool isPainted() const { return type == FPDF_PAGEOBJ_TEXT ||
                                  type == FPDF_PAGEOBJ_PATH ||
                                  type == FPDF_PAGEOBJ_IMAGE; }
  bool hit(double px, double py) const
  {
    return isPainted() && px >= l && px <= r && py >= b && py <= t;
  }
};

static std::vector<PageObject> ListPageObjects(int pi)
{
  std::vector<PageObject> out;
  if (!g.doc || pi < 0 || pi >= g.pageCount) return out;
  FPDF_PAGE page = FPDF_LoadPage(g.doc, pi);
  if (!page) return out;
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
  const int n = FPDFPage_CountObjects(page);
  for (int i = 0; i < n; ++i)
  {
    FPDF_PAGEOBJECT o = FPDFPage_GetObject(page, i);
    if (!o) continue;
    float l = 0, b = 0, r = 0, t = 0;
    if (!FPDFPageObj_GetBounds(o, &l, &b, &r, &t)) continue;
    PageObject po;
    po.index = i;
    po.type = FPDFPageObj_GetType(o);
    po.l = l; po.b = b; po.r = r; po.t = t;
    if (po.isText())
    {
      if (tp)
      {
        unsigned long len = FPDFTextObj_GetText(o, tp, nullptr, 0);
        if (len > 1)
        {
          std::vector<unsigned short> buf(len, 0);
          if (FPDFTextObj_GetText(o, tp, buf.data(), len) >= 2)
          {
            po.text.assign(reinterpret_cast<const wchar_t*>(buf.data()), len);
            while (!po.text.empty() && po.text.back() == L'\0')
              po.text.pop_back();
          }
        }
      }
      FPDFTextObj_GetFontSize(o, &po.fontSize);
      FPDF_FONT font = FPDFTextObj_GetFont(o);
      if (font)
      {
        size_t fn = FPDFFont_GetBaseFontName(font, nullptr, 0);
        if (fn > 1)
        {
          std::vector<char> nb(fn, 0);
          FPDFFont_GetBaseFontName(font, nb.data(), nb.size());
          po.fontName.assign(nb.data());
          size_t plus = po.fontName.find('+');
          if (plus != std::string::npos) po.fontName.erase(0, plus + 1);
        }
      }
    }
    out.push_back(po);
  }
  if (tp) FPDFText_ClosePage(tp);
  FPDF_ClosePage(page);
  return out;
}

static bool HitTestObject(int pi, double px, double py, PageObject& best)
{
  const std::vector<PageObject> objs = ListPageObjects(pi);
  // Last painted is the topmost.
  for (auto it = objs.rbegin(); it != objs.rend(); ++it)
    if (it->hit(px, py)) { best = *it; return true; }
  return false;
}

static void RefreshSelBounds()
{
  if (!g.sel.active) return;
  for (const PageObject& po : ListPageObjects(g.sel.page))
  {
    if (po.index == g.sel.index)
    {
      g.sel.type = po.type;
      g.sel.l = po.l; g.sel.b = po.b; g.sel.r = po.r; g.sel.t = po.t;
      return;
    }
  }
  g.sel.active = false;  // stale selection (page changed or object gone)
}

static void SetToolSelect(bool on)
{
  g.toolSelect = on;
  if (!on) g.sel.active = false;
  for (HWND h : g.ribbonBtns)
  {
    if (GetWindowLongPtrW(h, GWLP_ID) == ID_TOOL_SELECT)
    {
      Btn* b2 = reinterpret_cast<Btn*>(GetWindowLongPtrW(h, GWLP_USERDATA));
      if (b2) { b2->pressed = on; InvalidateRect(h, nullptr, TRUE); }
    }
  }
  InvalidateRect(g.status, nullptr, TRUE);
}

static void ApplyRecolor(int pi, int index, int r, int g_, int b);

// Persist object edits: serializes the (already GenerateContent'ed) doc and
// reloads it, exactly like Save-in-place. Selection is invalidated on reload.
static void CommitEdits()
{
  if (g.editPage) { FPDF_ClosePage(g.editPage); g.editPage = nullptr; }
  g.editObj = nullptr;
  g.selDrag = false;
  g.sel.active = false;
  SaveInPlace();
  RefreshSelBounds();
}

static void DeleteSelectedObject()
{
  if (!g.sel.active)
  {
    MessageBoxW(g.frame, L"Nothing selected. Use the Select tool to pick an object.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  FPDF_PAGE page = FPDF_LoadPage(g.doc, g.sel.page);
  FPDF_PAGEOBJECT o = page ? FPDFPage_GetObject(page, g.sel.index) : nullptr;
  if (o) FPDFPage_RemoveObject(page, o);
  if (page && o) FPDFPage_GenerateContent(page);
  if (page) FPDF_ClosePage(page);
  CommitEdits();
}

// ---------------------------------------------------------------------------
// Recolor palette dialog (SKClrWnd)
// ---------------------------------------------------------------------------
static std::vector<COLORREF> kPalette = {
  RGB(0x00, 0x00, 0x00), RGB(0xFF, 0xFF, 0xFF), RGB(0xFF, 0x00, 0x00),
  RGB(0x00, 0x80, 0x00), RGB(0x00, 0x00, 0xFF), RGB(0xFF, 0xE0, 0x00),
  RGB(0xFF, 0x80, 0x00), RGB(0x80, 0x80, 0x80),
};
static const wchar_t* kPaletteNames[] = {
  L"Black", L"White", L"Red", L"Green", L"Blue", L"Yellow", L"Orange", L"Gray",
};

static LRESULT CALLBACK ClrProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_CREATE:
    {
      const int cols = 4, bw = 52, bh = 30;
      for (int i = 0; i < (int)kPalette.size(); ++i)
      {
        int cx = 16 + (i % cols) * (bw + 12);
        int cy = 40 + (i / cols) * (bh + 12);
        CreateWindowExW(0, L"BUTTON", kPaletteNames[i],
                        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                        cx, cy, bw, bh, h, (HMENU)(INT_PTR)(i + 1),
                        g.inst, nullptr);
      }
      CreateWindowExW(0, L"STATIC", L"Pick a fill / stroke color:",
                      WS_CHILD | WS_VISIBLE, 16, 16, 240, 16, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      16, 124, 74, 28, h, (HMENU)99, g.inst, nullptr);
      return 0;
    }
    case WM_COMMAND:
    {
      const int id = LOWORD(w);
      if (id >= 1 && id <= (int)kPalette.size())
      {
        std::vector<unsigned char> col(3);
        col[0] = (kPalette[id - 1] >> 16) & 0xFF;
        col[1] = (kPalette[id - 1] >> 8) & 0xFF;
        col[2] = kPalette[id - 1] & 0xFF;
        ApplyRecolor(g.sel.page, g.sel.index, col[0], col[1], col[2]);
        DestroyWindow(h);
        return 0;
      }
      if (id == 99 || id == 2) { DestroyWindow(h); return 0; }
      break;
    }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

static void ApplyRecolorSelected()
{
  if (!g.sel.active)
  {
    MessageBoxW(g.frame, L"Nothing selected. Use the Select tool to pick an object.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  FPDF_PAGE page = FPDF_LoadPage(g.doc, g.sel.page);
  FPDF_PAGEOBJECT o = page ? FPDFPage_GetObject(page, g.sel.index) : nullptr;
  if (!o || !page) { if (page) FPDF_ClosePage(page); return; }
  const int t = FPDFPageObj_GetType(o);
  if (t != FPDF_PAGEOBJ_TEXT && t != FPDF_PAGEOBJ_PATH)
  {
    FPDF_ClosePage(page);
    MessageBoxW(g.frame, L"Recolor works on text and vector objects, not images.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  FPDF_ClosePage(page);

  const wchar_t cls[] = L"SKClrWnd";
  static bool reg = false;
  if (!reg)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = ClrProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    reg = true;
  }
  HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Recolor object",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 292, 190,
                            g.frame, nullptr, g.inst, nullptr);
  if (!hw) return;
  RECT fr, rc;
  GetWindowRect(g.frame, &fr);
  GetWindowRect(hw, &rc);
  SetWindowPos(hw, nullptr,
               fr.left + (fr.right - fr.left - (rc.right - rc.left)) / 2,
               fr.top + (fr.bottom - fr.top - (rc.bottom - rc.top)) / 2,
               0, 0, SWP_NOSIZE | SWP_NOZORDER);
  EnableWindow(g.frame, FALSE);
  ShowWindow(hw, SW_SHOW);
  UpdateWindow(hw);
  MSG msg;
  while (IsWindow(hw))
  {
    const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
    if (r <= 0) break;
    if (!IsDialogMessageW(hw, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  }
  EnableWindow(g.frame, TRUE);
  SetActiveWindow(g.frame);
  SetFocus(g.frame);
}

// ---------------------------------------------------------------------------
// Text edit dialog (SKTxtWnd) - double-click a text object to replace it
// ---------------------------------------------------------------------------
static HWND g_tedEdit = nullptr;
static bool g_tedOk = false;
static std::wstring g_tedValue;

static LRESULT CALLBACK TedProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_CREATE:
      CreateWindowExW(0, L"STATIC", L"Text:",
                      WS_CHILD | WS_VISIBLE, 16, 14, 60, 16, h, nullptr, g.inst, nullptr);
      g_tedEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                  WS_CHILD | WS_VISIBLE | WS_TABSTOP |
                                  ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
                                  66, 12, 262, 80, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Ok", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                      110, 104, 74, 28, h, (HMENU)1, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      196, 104, 74, 28, h, (HMENU)2, g.inst, nullptr);
      SetWindowTextW(g_tedEdit, g_tedValue.c_str());
      SetFocus(g_tedEdit);
      return 0;
    case WM_COMMAND:
      if (LOWORD(w) == 1 || LOWORD(w) == 2)
      {
        if (LOWORD(w) == 1 && g_tedEdit)
        {
          wchar_t buf[1024];
          GetWindowTextW(g_tedEdit, buf, 1024);
          g_tedValue = buf;
        }
        g_tedOk = (LOWORD(w) == 1);
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_CLOSE:
      g_tedOk = false;
      DestroyWindow(h);
      return 0;
    case WM_CTLCOLORSTATIC:
      return (LRESULT)(HBRUSH)(COLOR_BTNFACE + 1);
  }
  return DefWindowProcW(h, m, w, l);
}

static bool PromptEditText(const std::wstring& initial, std::wstring& out)
{
  const wchar_t cls[] = L"SKTxtWnd";
  static bool reg = false;
  if (!reg)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = TedProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    reg = true;
  }
  g_tedEdit = nullptr;
  g_tedOk = false;
  g_tedValue = initial;
  HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, L"Edit text",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 348, 170,
                            g.frame, nullptr, g.inst, nullptr);
  if (!hw) return false;
  RECT fr, rc;
  GetWindowRect(g.frame, &fr);
  GetWindowRect(hw, &rc);
  SetWindowPos(hw, nullptr,
               fr.left + (fr.right - fr.left - (rc.right - rc.left)) / 2,
               fr.top + (fr.bottom - fr.top - (rc.bottom - rc.top)) / 2,
               0, 0, SWP_NOSIZE | SWP_NOZORDER);
  EnableWindow(g.frame, FALSE);
  ShowWindow(hw, SW_SHOW);
  UpdateWindow(hw);
  MSG msg;
  while (IsWindow(hw))
  {
    const BOOL r = GetMessageW(&msg, nullptr, 0, 0);
    if (r <= 0) break;
    if (!IsDialogMessageW(hw, &msg)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
  }
  EnableWindow(g.frame, TRUE);
  SetActiveWindow(g.frame);
  SetFocus(g.frame);
  out = g_tedValue;
  return g_tedOk;
}

static void EditTextObject(int pi, int index, const std::wstring& newText)
{
  if (pi < 0 || pi >= g.pageCount) return;
  FPDF_PAGE page = FPDF_LoadPage(g.doc, pi);
  FPDF_PAGEOBJECT oldObj = page ? FPDFPage_GetObject(page, index) : nullptr;
  if (!oldObj || !page) { if (page) FPDF_ClosePage(page); return; }
  float ol = 0, ob = 0, or_ = 0, ot = 0;
  FPDFPageObj_GetBounds(oldObj, &ol, &ob, &or_, &ot);
  float size = 12.0f;
  if (FPDFTextObj_GetFontSize(oldObj, &size) == 0) size = 12.0f;

  // Build the replacement first so a failure never destroys the original.
  const PageObject foundHere = [&]() {
    const std::vector<PageObject> objs = ListPageObjects(pi);
    for (const PageObject& po : objs)
      if (po.index == index) return po;
    return PageObject{};
  }();
  std::string fn = foundHere.fontName.empty() ? "Helvetica" : foundHere.fontName;
  FPDF_PAGEOBJECT no = FPDFPageObj_NewTextObj(g.doc, fn.c_str(), size);
  if (!no) { FPDF_ClosePage(page); return; }
  const unsigned short* u16t = reinterpret_cast<const unsigned short*>(newText.c_str());
  if (!FPDFText_SetText(no, u16t)) { FPDFPageObj_Destroy(no); FPDF_ClosePage(page); return; }
  FS_MATRIX m{};
  m.a = 1; m.d = 1;
  FPDFPageObj_SetMatrix(no, &m);
  float nl = 0, nb = 0, nr = 0, nt = 0;
  FPDFPageObj_GetBounds(no, &nl, &nb, &nr, &nt);
  m.e = ol - nl;   // anchor bottom-left of the original run
  m.f = ob - nb;
  FPDFPageObj_SetMatrix(no, &m);
  FPDFPage_RemoveObject(page, oldObj);
  FPDFPage_InsertObjectAtIndex(page, no, index);
  FPDFPage_GenerateContent(page);
  FPDF_ClosePage(page);
  CommitEdits();
}

static void ApplyRecolor(int pi, int index, int r, int g_, int b)
{
  if (pi < 0 || pi >= g.pageCount) return;
  FPDF_PAGE page = FPDF_LoadPage(g.doc, pi);
  FPDF_PAGEOBJECT o = page ? FPDFPage_GetObject(page, index) : nullptr;
  if (o)
  {
    FPDFPageObj_SetFillColor(o, (unsigned int)r, (unsigned int)g_, (unsigned int)b, 255);
    FPDFPageObj_SetStrokeColor(o, (unsigned int)r, (unsigned int)g_, (unsigned int)b, 255);
    FPDFPage_GenerateContent(page);
  }
  if (page) FPDF_ClosePage(page);
  CommitEdits();
}

static void EditSelectedText()
{
  if (!g.sel.active)
  {
    MessageBoxW(g.frame, L"Nothing selected. Double-click a text object to edit it.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  if (g.sel.type != FPDF_PAGEOBJ_TEXT)
  {
    MessageBoxW(g.frame, L"Only text objects can be edited this way.",
                L"Stitchup", MB_OK | MB_ICONINFORMATION);
    return;
  }
  std::wstring cur;
  for (const PageObject& po : ListPageObjects(g.sel.page))
    if (po.index == g.sel.index) { cur = po.text; break; }
  std::wstring out;
  if (PromptEditText(cur, out))
    EditTextObject(g.sel.page, g.sel.index, out);
}

static void CanvasPaint(HDC dc, int cw, int ch)
{
  const Theme& th = ThemeNow();
  HBRUSH bg = CreateSolidBrush(th.canvasBg);
  RECT rc{0, 0, cw, ch};
  FillRect(dc, &rc, bg);
  DeleteObject(bg);

  if (!g.doc || g.pageCount == 0) return;
  double s = g.zoom;
  double span = std::max(1.0, MaxPageW()) * s;
  int workW = (int)std::ceil(span) + 32;
  int xBase = (workW - (int)std::ceil(span)) / 2 - g.scrollX;
  int yc = 12 - g.scrollY;

  for (int i = 0; i < g.pageCount; ++i)
  {
    float pw = PageW(i), ph = PageH(i);
    if (pw < 1 || ph < 1) continue;
    int w = (int)std::ceil(pw * s);
    int h = (int)std::ceil(ph * s);
    int x = xBase + (int)((std::ceil(span) - w) / 2.0);
    RECT r{x, yc, x + w, yc + h};
    if (r.bottom < 0 || r.top > ch) { yc += h + 14; continue; }
    if (r.right >= 0 && r.left <= cw)
    {
      int key;
      int rx = x, ry = yc, rw = w, rh = h;
      // shadow (canvas dimmed ~30%)
      COLORREF shC = RGB((GetRValue(th.canvasBg) * 7) / 10,
                         (GetGValue(th.canvasBg) * 7) / 10,
                         (GetBValue(th.canvasBg) * 7) / 10);
      HBRUSH sh = CreateSolidBrush(shC);
      RECT sr{x + 4, yc + 4, x + w + 4, yc + h + 4};
      FillRect(dc, &sr, sh);
      DeleteObject(sh);

      auto it = g.canvasCache.find(i);
      HBITMAP hb = nullptr;
      bool liveBmp = false;
      key = ZoomKey();
      if (g.toolSelect && g.selDrag && g.editPage && g.sel.page == i)
      {
        // Live drag: always re-render from the open page being transformed.
        hb = RenderPageBitmapInto(g.editPage, w, h);
        liveBmp = true;
      }
      else if (it != g.canvasCache.end() && it->second.key == key)
        hb = it->second.bmp;
      if (!hb)
      {
        hb = RenderPageBitmap(i, w, h);
        if (hb)
        {
          if (g.canvasCache.size() > 64)
          {
            auto first = g.canvasCache.begin();
            DeleteObject(first->second.bmp);
            g.canvasCache.erase(first);
          }
          g.canvasCache[i] = PageCache{key, hb, w, h};
        }
      }
      if (hb)
      {
        HDC mem = CreateCompatibleDC(dc);
        SelectObject(mem, hb);
        if (w > 0 && h > 0)
          BitBlt(dc, rx, ry, rw, rh, mem, 0, 0, SRCCOPY);
        DeleteDC(mem);
        if (liveBmp) DeleteObject(hb);
      }
      HBRUSH fb = CreateSolidBrush(g.selected == i ? th.accent
                                                   : th.pageFrame);
      FrameRect(dc, &r, fb);
      DeleteObject(fb);

      // Content-object selection overlay (Select tool)
      if (g.sel.active && g.sel.page == i && g.sel.type > 0)
      {
        float slx = g.sel.l, sbx = g.sel.b, srx = g.sel.r, stx = g.sel.t;
        if (srx > slx && stx > sbx)
        {
          int ax = x + (int)std::lround(slx * s);
          int ay = yc + (int)std::lround((ph - stx) * s);
          int aw = (int)std::lround((srx - slx) * s);
          int ah = (int)std::lround((stx - sbx) * s);
          HPEN pen = CreatePen(PS_SOLID, 2, th.accent);
          HPEN old = (HPEN)SelectObject(dc, pen);
          HBRUSH oldb = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
          Rectangle(dc, ax, ay, ax + aw, ay + ah);
          SelectObject(dc, oldb);
          SelectObject(dc, old);
          DeleteObject(pen);
          HBRUSH wh = CreateSolidBrush(th.canvasBg);
          HBRUSH acc = CreateSolidBrush(th.accent);
          for (int hx = 0; hx < 2; ++hx)
          {
            for (int hy = 0; hy < 2; ++hy)
            {
              int hcx = hx ? ax + aw - 4 : ax - 4;
              int hcy = hy ? ay + ah - 4 : ay - 4;
              RECT hr{hcx, hcy, hcx + 8, hcy + 8};
              FillRect(dc, &hr, wh);
              FrameRect(dc, &hr, acc);
            }
          }
          DeleteObject(acc);
          DeleteObject(wh);
        }
      }
    }
    yc += h + 14;
  }
}

struct HitInfo
{
  int page, x, y, w, h;
};
static bool HitPage(POINT pt, HitInfo& out);
static bool GetLinkAtDevice(FPDF_PAGE page, POINT pt, FPDF_LINK& link);
static bool FollowLink(FPDF_PAGE page, POINT pt);

static LRESULT CALLBACK CanvasProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      RECT rc;
      GetClientRect(hw, &rc);
      CanvasPaint(dc, rc.right - rc.left, rc.bottom - rc.top);
      EndPaint(hw, &ps);
      return 0;
    }
    case WM_SIZE:
      UpdateScrollbars();
      return 0;
    case WM_VSCROLL:
    case WM_HSCROLL:
    {
      int bar = (msg == WM_VSCROLL) ? SB_VERT : SB_HORZ;
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_ALL;
      GetScrollInfo(hw, bar, &si);
      int pos = si.nPos;
      switch (LOWORD(wp))
      {
        case SB_LINEUP:  pos -= 40; break;
        case SB_LINEDOWN:pos += 40; break;
        case SB_PAGEUP:  pos -= si.nPage; break;
        case SB_PAGEDOWN:pos += si.nPage; break;
        case SB_THUMBTRACK: pos = HIWORD(wp); break;
        case SB_TOP:  pos = si.nMin; break;
        case SB_BOTTOM: pos = si.nMax; break;
      }
      pos = std::max(si.nMin, std::min((int)si.nMax, pos));
      si.nPos = pos;
      si.fMask = SIF_POS;
      SetScrollInfo(hw, bar, &si, TRUE);
      if (bar == SB_VERT) g.scrollY = pos;
      else g.scrollX = pos;
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL:
    {
      short d = GET_WHEEL_DELTA_WPARAM(wp);
      if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL)
      {
        double f = (d > 0) ? 1.25 : 0.8;
        if (f > 0.0)
        {
          g.zoom = std::max(0.1, std::min(8.0, g.zoom * f));
          ClearCanvasCache();
          UpdateScrollbars();
          InvalidateRect(hw, nullptr, TRUE);
          InvalidateRect(g.status, nullptr, TRUE);
        }
      }
      else
      {
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_POS;
        GetScrollInfo(hw, SB_VERT, &si);
        g.scrollY = std::max(0, g.scrollY + (int)(-(d / WHEEL_DELTA) * 60));
        UpdateScrollbars();
        InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
    }
    case WM_SETCURSOR:
    {
      POINT cur;
      GetCursorPos(&cur);
      ScreenToClient(hw, &cur);
      if (g.doc && g.pageCount > 0)
      {
        HitInfo hi;
        if (HitPage(cur, hi))
        {
          if (g.toolSelect)
          {
            const double s = g.zoom;
            const float ph = PageH(hi.page);
            double px = (cur.x - hi.x) / s;
            double py = ph - (cur.y - hi.y) / s;
            PageObject best;
            if (HitTestObject(hi.page, px, py, best))
            {
              SetCursor(LoadCursorW(nullptr, IDC_SIZEALL));
              return TRUE;
            }
            return DefWindowProcW(hw, msg, wp, lp);
          }
          FPDF_PAGE page = FPDF_LoadPage(g.doc, hi.page);
          if (page)
          {
            FPDF_LINK link = nullptr;
            bool over = GetLinkAtDevice(page, cur, link);
            FPDF_ClosePage(page);
            if (over)
            {
              SetCursor(LoadCursorW(nullptr, IDC_HAND));
              return TRUE;
            }
          }
        }
      }
      return DefWindowProcW(hw, msg, wp, lp);
    }
    case WM_LBUTTONDOWN:
    {
      SetFocus(g.frame);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      HitInfo hi;
      if (HitPage(pt, hi))
      {
        if (g.toolSelect)
        {
          const double s = g.zoom;
          const float ph = PageH(hi.page);
          double px = (pt.x - hi.x) / s;
          double py = ph - (pt.y - hi.y) / s;
          PageObject best;
          if (HitTestObject(hi.page, px, py, best))
          {
            g.sel.active = true;
            g.sel.page = hi.page;
            g.sel.index = best.index;
            g.sel.type = best.type;
            g.sel.l = best.l; g.sel.b = best.b; g.sel.r = best.r; g.sel.t = best.t;
            g.editPage = FPDF_LoadPage(g.doc, g.sel.page);
            g.editObj = g.editPage ? FPDFPage_GetObject(g.editPage, g.sel.index) : nullptr;
            if (g.editObj)
            {
              g.selDrag = true;
              g.selDragMoved = false;
              g.dragLastX = px;
              g.dragLastY = py;
              SetCapture(hw);
            }
          }
          else
          {
            g.sel.active = false;
            g.selDrag = false;
          }
          g.selected = hi.page;
          ClearCanvasCache();
          InvalidateRect(hw, nullptr, TRUE);
          InvalidateRect(g.thumbs, nullptr, TRUE);
          InvalidateRect(g.status, nullptr, TRUE);
          return 0;
        }
        FPDF_PAGE page = FPDF_LoadPage(g.doc, hi.page);
        bool viaLink = false;
        if (page)
        {
          viaLink = FollowLink(page, pt);
          FPDF_ClosePage(page);
        }
        if (!viaLink && g.selected != hi.page)
        {
          g.selected = hi.page;
          InvalidateRect(hw, nullptr, TRUE);
          InvalidateRect(g.thumbs, nullptr, TRUE);
          InvalidateRect(g.status, nullptr, TRUE);
        }
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
      if (!g.toolSelect) return 0;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      HitInfo hi;
      if (!HitPage(pt, hi)) return 0;
      const double s = g.zoom;
      const float ph = PageH(hi.page);
      double px = (pt.x - hi.x) / s;
      double py = ph - (pt.y - hi.y) / s;
      PageObject best;
      if (HitTestObject(hi.page, px, py, best) && best.isText())
      {
        g.selDrag = false;
        if (g.editPage) { FPDF_ClosePage(g.editPage); g.editPage = nullptr; }
        g.editObj = nullptr;
        g.sel.active = true;
        g.sel.page = hi.page;
        g.sel.index = best.index;
        g.sel.type = best.type;
        g.sel.l = best.l; g.sel.b = best.b; g.sel.r = best.r; g.sel.t = best.t;
        std::wstring out;
        if (PromptEditText(best.text, out))
          EditTextObject(hi.page, best.index, out);
        else
          RefreshSelBounds();
      }
      return 0;
    }
    case WM_MOUSEMOVE:
    {
      if (!(g.toolSelect && g.selDrag && GetCapture() == hw)) break;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      HitInfo hi;
      if (!HitPage(pt, hi)) return 0;
      const double s = g.zoom;
      const float ph = PageH(hi.page);
      double px = (pt.x - hi.x) / s;
      double py = ph - (pt.y - hi.y) / s;
      double dx = px - g.dragLastX;
      double dy = py - g.dragLastY;
      if (dx != 0 || dy != 0)
      {
        if (g.editPage && g.editObj)
        {
          FPDFPageObj_Transform(g.editObj, 1, 0, 0, 1, (float)dx, (float)dy);
          g.selDragMoved = true;
          g.sel.l += (float)dx; g.sel.r += (float)dx;
          g.sel.b += (float)dy; g.sel.t += (float)dy;
        }
        g.dragLastX = px;
        g.dragLastY = py;
        ClearCanvasCache();
        InvalidateRect(hw, nullptr, TRUE);
      }
      return 0;
    }
    case WM_LBUTTONUP:
    {
      if (!(g.toolSelect && g.selDrag)) break;
      ReleaseCapture();
      const bool moved = g.selDragMoved;
      if (g.editPage && g.editObj)
      {
        if (moved)
        {
          FPDFPage_GenerateContent(g.editPage);
        }
        FPDF_ClosePage(g.editPage);
      }
      g.editPage = nullptr;
      g.editObj = nullptr;
      g.selDrag = false;
      g.selDragMoved = false;
      if (moved)
      {
        CommitEdits();
      }
      return 0;
    }
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Split bar
// ---------------------------------------------------------------------------
static LRESULT CALLBACK SplitProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      RECT rc;
      GetClientRect(hw, &rc);
      HBRUSH cl = CreateSolidBrush(ThemeNow().split);
      FillRect(dc, &rc, cl);
      DeleteObject(cl);
      EndPaint(hw, &ps);
      return 0;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
      return TRUE;
    case WM_LBUTTONDOWN:
      SetCapture(hw);
      return 0;
    case WM_MOUSEMOVE:
      if (GetCapture() == hw)
      {
        POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        ClientToScreen(hw, &pt);
        RECT fr;
        GetWindowRect(g.frame, &fr);
        int nw = pt.x - fr.left;
        nw = std::max(130, std::min(440, nw));
        if (nw != g.thumbsW)
        {
          g.thumbsW = nw;
          RECT cr;
          GetClientRect(g.frame, &cr);
          SetWindowPos(g.thumbs, nullptr, 0, 0, g.thumbsW, cr.bottom - 28,
                       SWP_NOMOVE | SWP_NOZORDER);
          SetWindowPos(hw, nullptr, g.thumbsW, 0, 6, cr.bottom - 28,
                       SWP_NOZORDER);
          SetWindowPos(g.canvas, nullptr, g.thumbsW + 6, 44,
                       std::max(100, (int)(cr.right - (g.thumbsW + 6))),
                       cr.bottom - 72, SWP_NOZORDER);
          InvalidateRect(g.thumbs, nullptr, TRUE);
        }
      }
      return 0;
    case WM_LBUTTONUP:
    case WM_CANCELMODE:
      ReleaseCapture();
      return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Toolbar custom buttons
// ---------------------------------------------------------------------------
static void BtnPaint(HWND hw)
{
  HWND parent = GetParent(hw);
  if (parent != g.toolbar && parent != g.paneTabs) return;
  Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
  if (!b) return;
  PAINTSTRUCT ps;
  HDC dc = BeginPaint(hw, &ps);
  RECT rc;
  GetClientRect(hw, &rc);
  const Theme& th = ThemeNow();
  const int R = 6;  // corner diameter for rounded 3px radius
  if (b->tab)
  {
    if (b->hover)
    {
      HBRUSH hb = CreateSolidBrush(th.btnHover);
      HPEN nopen = (HPEN)GetStockObject(NULL_PEN);
      HBRUSH wasb = (HBRUSH)SelectObject(dc, hb);
      HPEN wasp = (HPEN)SelectObject(dc, nopen);
      RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, R, R);
      SelectObject(dc, wasb);
      SelectObject(dc, wasp);
      DeleteObject(hb);
    }
    if (b->pressed)
    {
      HBRUSH acc = CreateSolidBrush(th.accent);
      HPEN unob = (HPEN)GetStockObject(NULL_PEN);
      HBRUSH wasb2 = (HBRUSH)SelectObject(dc, acc);
      HPEN waspp = (HPEN)SelectObject(dc, unob);
      RECT ar{rc.left + 10, rc.bottom - 3, rc.right - 10, rc.bottom - 1};
      RoundRect(dc, ar.left, ar.top, ar.right, ar.bottom, 2, 2);
      SelectObject(dc, wasb2);
      SelectObject(dc, waspp);
      DeleteObject(acc);
    }
    SetBkMode(dc, TRANSPARENT);
    HFONT was = (HFONT)SelectObject(dc, g.font);
    SetTextColor(dc, b->pressed ? th.accent : (b->hover ? th.text : th.textDim));
    DrawTextW(dc, b->label.c_str(), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, was);
  }
  else
  {
    BOOL active = b->hover || b->down;
    if (!b->hover && !b->down)
    {
      HBRUSH bg = CreateSolidBrush(th.card);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
    }
    if (active)
    {
      HBRUSH fill = CreateSolidBrush(b->down ? th.btnDown : th.btnHover);
      HPEN pen = CreatePen(PS_SOLID, 1, b->down ? th.accent : th.btnBorder);
      HBRUSH wasb = (HBRUSH)SelectObject(dc, fill);
      HPEN wasp = (HPEN)SelectObject(dc, pen);
      RoundRect(dc, rc.left, rc.top, rc.right, rc.bottom, R, R);
      SelectObject(dc, wasb);
      SelectObject(dc, wasp);
      DeleteObject(fill);
      DeleteObject(pen);
    }
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, b->down ? th.accent : th.text);
    DrawTextW(dc, b->label.c_str(), -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  }
  EndPaint(hw, &ps);
}

static void BtnTrack(HWND hw, bool leave)
{
  if (leave)
  {
    Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (b && b->hover) { b->hover = false; InvalidateRect(hw, nullptr, TRUE); }
  }
  POINT pt;
  GetCursorPos(&pt);
  RECT rc;
  GetWindowRect(hw, &rc);
  Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
  if (b && ((bool)PtInRect(&rc, pt)) != b->hover)
  {
    b->hover = PtInRect(&rc, pt) != FALSE;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hw, 0};
    TrackMouseEvent(&tme);
    InvalidateRect(hw, nullptr, TRUE);
  }
}

static LRESULT CALLBACK ToolBtnProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_PAINT:
      BtnPaint(hw);
      return 0;
    case WM_MOUSEMOVE:
      BtnTrack(hw, false);
      return 0;
    case WM_MOUSELEAVE:
      BtnTrack(hw, true);
      return 0;
    case WM_LBUTTONDOWN:
    {
      Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
      if (b) { b->down = true; InvalidateRect(hw, nullptr, TRUE); }
      SetCapture(hw);
      return 0;
    }
    case WM_LBUTTONUP:
    {
      ReleaseCapture();
      Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
      if (b) { b->down = false; InvalidateRect(hw, nullptr, TRUE); }
      RECT rc;
      GetWindowRect(hw, &rc);
      POINT pt;
      GetCursorPos(&pt);
      if (PtInRect(&rc, pt))
        SendMessageW(g.frame, WM_COMMAND, MAKEWPARAM(GetWindowLongPtrW(hw, GWLP_ID), 0),
                     reinterpret_cast<LPARAM>(hw));
      return 0;
    }
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

static HWND MakeBtn(HWND parent, int id, const wchar_t* label, int x, int y,
                    int w, int h, bool tabStyle)
{
  HWND hw = CreateWindowExW(0, L"SKToolBtn", label, WS_CHILD | WS_VISIBLE,
                            x, y, w, h, parent, nullptr,
                            g.inst, nullptr);
  Btn* b = new Btn();
  b->label = label;
  b->tab = tabStyle;
  SetWindowLongPtrW(hw, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
  SetWindowLongPtrW(hw, GWLP_ID, static_cast<LONG_PTR>(id));
  return hw;
}

static HWND MakeBtn(int id, const wchar_t* label, int x, int y, int w, int h)
{
  return MakeBtn(g.toolbar, id, label, x, y, w, h, false);
}

// ---------------------------------------------------------------------------
// Ribbon (Nitro-style tab strip + groups)
// ---------------------------------------------------------------------------
struct RibbonSpec
{
  int id;
  const wchar_t* label;
  int w;
  int tab;
  int grp;
};

static void BuildToolbar(HWND)
{
  static const RibbonSpec specs[] = {
    {ID_NEW,          L"New",        54, 0, 0},
    {ID_OPEN,         L"Open",       58, 0, 0},
    {ID_SAVE,         L"Save",       56, 0, 0},
    {ID_SAVEAS,       L"Save As",    74, 0, 0},
    {ID_IMPORT,       L"Import",     68, 0, 0},
    {ID_ROTL,         L"Rotate CCW", 88, 0, 1},
    {ID_ROTR,         L"Rotate CW",  86, 0, 1},
    {ID_DELETE,       L"Delete",     66, 0, 1},
    {ID_ADD,          L"Add Page",   80, 0, 1},
    {ID_PAGE_EXTRACT, L"Extract",    70, 0, 1},
    {ID_PAGE_SPLIT,   L"Split",      60, 0, 1},
    {ID_PAGE_CROP,    L"Auto-Crop",  84, 0, 1},
    {ID_ZOOM_OUT,     L"Zoom -",     62, 1, 0},
    {ID_ZOOM_IN,      L"Zoom +",     62, 1, 0},
    {ID_ZOOM100,      L"100%",       54, 1, 0},
    {ID_FITW,         L"Fit Width",  80, 1, 0},
    {ID_FITP,         L"Fit Page",   76, 1, 0},
    {ID_PREV,         L"Previous",   78, 1, 1},
    {ID_NEXT,         L"Next",       62, 1, 1},
    {ID_PANE_THUMBS,  L"Thumbnails", 92, 1, 2},
    {ID_PANE_BOOKMARKS, L"Bookmarks", 90, 1, 2},
    {ID_ANN_HL,       L"Highlight",  80, 0, 2},
    {ID_ANN_UL,       L"Underline",  80, 0, 2},
    {ID_ANN_NOTE,     L"Note",       56, 0, 2},
    {ID_ANN_TEXT,     L"Text Box",   78, 0, 2},
    {ID_ANN_SHAPE,    L"Shape",      62, 0, 2},
    {ID_ANN_STAMP,    L"Stamp",      62, 0, 2},
    {ID_TOOL_SELECT,  L"Select",     62, 0, 3},
    {ID_OBJ_EDIT,     L"Edit Text",  78, 0, 3},
    {ID_OBJ_DELETE,   L"Delete",     62, 0, 3},
    {ID_OBJ_RECOLOR,  L"Recolor",    66, 0, 3},
  };
  g.tabBtns[0] = MakeBtn(g.toolbar, ID_TAB_HOME, L"Home", 4, 2, 66, 20, true);
  g.tabBtns[1] = MakeBtn(g.toolbar, ID_TAB_VIEW, L"View", 74, 2, 66, 20, true);
  g.tabBtns[2] = MakeBtn(g.toolbar, ID_TAB_TOOLS, L"Tools", 144, 2, 66, 20, true);
  for (const RibbonSpec& s : specs)
  {
    HWND hw = MakeBtn(s.id, s.label, 0, 0, s.w, RIB_BTN_H);
    Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (b) { b->rtab = s.tab; b->rgroup = s.grp; }
    g.ribbonBtns.push_back(hw);
  }
}

static const wchar_t* GroupName(int tab, int grp)
{
  if (tab == 0)
    return grp == 0 ? L"Document" : (grp == 1 ? L"Pages" : (grp == 2 ? L"Annotate" : L"Content"));
  if (tab == 1)
    return grp == 0 ? L"Zoom" : (grp == 1 ? L"Navigate" : L"Panes");
  return nullptr;
}

static int GroupCount(int tab)
{
  return tab == 0 ? 4 : (tab == 1 ? 3 : 0);
}

static void SetTabPressed()
{
  for (int i = 0; i < 3 && g.tabBtns[i]; ++i)
  {
    Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(g.tabBtns[i], GWLP_USERDATA));
    if (b)
    {
      b->pressed = (g.ribbonTab == i);
      InvalidateRect(g.tabBtns[i], nullptr, TRUE);
    }
  }
}

static void LayoutRibbon()
{
  g.groups.clear();
  int x = 6;
  int nc = GroupCount(g.ribbonTab);
  for (int gi = 0; gi < nc; ++gi)
  {
    int gx = x;
    for (HWND hw : g.ribbonBtns)
    {
      Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
      if (!b || b->rtab != g.ribbonTab || b->rgroup != gi) continue;
      RECT rc2{};
      GetWindowRect(hw, &rc2);
      int w = rc2.right - rc2.left;
      SetWindowPos(hw, nullptr, x, RIB_BTN_Y, w, RIB_BTN_H, SWP_NOZORDER);
      ShowWindow(hw, SW_SHOW);
      x += w + 6;
    }
    int groupW = x - gx;
    App::GroupBox gb;
    gb.tab = g.ribbonTab;
    gb.name = GroupName(g.ribbonTab, gi) ? GroupName(g.ribbonTab, gi) : L"";
    gb.rc = {gx - 4, RIB_CAP_Y, gx + groupW - 2, RIB_H};
    g.groups.push_back(gb);
    x += 14;
  }
  for (HWND hw : g.ribbonBtns)
  {
    Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (b && (b->rtab != g.ribbonTab || b->rgroup >= GroupCount(g.ribbonTab)))
      ShowWindow(hw, SW_HIDE);
  }
  SetTabPressed();
  InvalidateRect(g.toolbar, nullptr, TRUE);
}

static void SwitchRibbonTab(int tab)
{
  if (tab < 0 || tab > 2) return;
  if (g.ribbonTab == tab) return;
  g.ribbonTab = tab;
  LayoutRibbon();
}

// ---------------------------------------------------------------------------
// Main frame / keyboard / commands
// ---------------------------------------------------------------------------
static void ZoomTo(double z, bool)
{
  g.zoom = std::max(0.1, std::min(8.0, z));
  ClearCanvasCache();
  UpdateScrollbars();
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.status, nullptr, TRUE);
}

static void FitPage()
{
  RECT rc;
  GetClientRect(g.canvas, &rc);
  int cw = std::max(120, (int)(rc.right - rc.left));
  int ch = std::max(160, (int)(rc.bottom - rc.top));
  double mw = std::max(1.0, MaxPageW());
  double mh = std::max(1.0, MaxPageH());
  ZoomTo(std::min((cw - 40.0) / mw, (ch - 60.0) / mh), true);
}

static void FitWidth()
{
  RECT rc;
  GetClientRect(g.canvas, &rc);
  int cw = std::max(120, (int)(rc.right - rc.left));
  double mw = std::max(1.0, MaxPageW());
  ZoomTo(std::max(0.1, (cw - 40.0) / mw), true);
}

static void GotoPageIndex(int idx)
{
  if (g.pageCount == 0) return;
  g.selected = std::max(0, std::min(g.pageCount - 1, idx));
  double s = g.zoom;
  float ph = PageH(g.selected);
  g.scrollY = (int)((g.selected < 2) ? 0 : (g.selected - 1) * (ph * s + 14.0));
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
  InvalidateRect(g.status, nullptr, TRUE);
  UpdateScrollbars();
  InvalidateRect(g.canvas, nullptr, TRUE);
}

static void GoPage(int d)
{
  if (g.pageCount == 0) return;
  GotoPageIndex(g.selected + d);
}

// ---------------------------------------------------------------------------
// Links (click-to-activate + hover cursor)
// ---------------------------------------------------------------------------
static bool HitPage(POINT pt, HitInfo& out)
{
  double s = g.zoom;
  double span = std::max(1.0, MaxPageW()) * s;
  int workW = (int)std::ceil(span) + 32;
  int xBase = (workW - (int)std::ceil(span)) / 2 - g.scrollX;
  int yc = 12 - g.scrollY;
  for (int i = 0; i < g.pageCount; ++i)
  {
    float pw = PageW(i), ph = PageH(i);
    if (pw < 1 || ph < 1) continue;
    int w = (int)std::ceil(pw * s);
    int h = (int)std::ceil(ph * s);
    int x = xBase + (int)((std::ceil(span) - w) / 2.0);
    if (pt.x >= x && pt.x <= x + w && pt.y >= yc && pt.y <= yc + h)
    {
      out = {i, x, yc, w, h};
      return true;
    }
    yc += h + 14;
  }
  return false;
}

static bool GetLinkAtDevice(FPDF_PAGE page, POINT pt, FPDF_LINK& link)
{
  HitInfo h;
  if (!HitPage(pt, h)) return false;
  float ph = PageH(h.page);
  double s = g.zoom;
  // PDF page coordinates have the origin at the bottom-left (y up).
  link = FPDFLink_GetLinkAtPoint(page, (pt.x - h.x) / s, ph - (pt.y - h.y) / s);
  return link != nullptr;
}

static bool FollowLink(FPDF_PAGE page, POINT pt)
{
  FPDF_LINK link = nullptr;
  if (!GetLinkAtDevice(page, pt, link)) return false;
  FPDF_DEST dest = FPDFLink_GetDest(g.doc, link);
  if (dest)
  {
    int pi = FPDFDest_GetDestPageIndex(g.doc, dest);
    if (pi >= 0 && pi < g.pageCount)
    {
      GotoPageIndex(pi);
      return true;
    }
    return false;
  }
  FPDF_ACTION act = FPDFLink_GetAction(link);
  if (!act) return false;
  unsigned long nbytes = FPDFAction_GetURIPath(g.doc, act, nullptr, 0);
  if (nbytes <= 3) return false;
  std::vector<char> raw(nbytes + 1, 0);
  FPDFAction_GetURIPath(g.doc, act, raw.data(), (unsigned long)raw.size());
  std::string uri8 = raw.data();
  int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, uri8.c_str(),
                               (int)uri8.size(), nullptr, 0);
  UINT cp = CP_UTF8;
  if (wn <= 0)
  {
    cp = CP_ACP;
    wn = MultiByteToWideChar(cp, 0, uri8.c_str(), (int)uri8.size(), nullptr, 0);
  }
  if (wn <= 0) return false;
  std::wstring uri((size_t)wn, L'\0');
  MultiByteToWideChar(cp, 0, uri8.c_str(), (int)uri8.size(), &uri[0], wn);
  return (intptr_t)ShellExecuteW(nullptr, L"open", uri.c_str(), nullptr,
                                 nullptr, SW_SHOWNORMAL) > 32;
}

// ---------------------------------------------------------------------------
// Navigation pane (thumbnails / bookmarks)
// ---------------------------------------------------------------------------
static bool GetBookmarkTitle(FPDF_BOOKMARK bm, std::wstring& out)
{
  unsigned long bytes = FPDFBookmark_GetTitle(bm, nullptr, 0);
  if (bytes < 2) { out.clear(); return false; }
  std::vector<unsigned char> raw(bytes);
  if (!raw.empty())
    FPDFBookmark_GetTitle(bm, raw.data(), (unsigned long)raw.size());
  else
    raw.resize(2);
  int chars = (int)(bytes / 2) - 1;
  if (chars < 0) chars = 0;
  out.assign(reinterpret_cast<const wchar_t*>(raw.data()), size_t(chars));
  return true;
}

static void AddBookmarkTree(FPDF_BOOKMARK parent, HTREEITEM parentItem, int depth)
{
  if (depth > 48 || !g.doc) return;
  for (FPDF_BOOKMARK bm = FPDFBookmark_GetFirstChild(g.doc, parent);
       bm; bm = FPDFBookmark_GetNextSibling(g.doc, bm))
  {
    std::wstring title;
    if (!GetBookmarkTitle(bm, title) || title.empty()) continue;
    std::vector<wchar_t> tmp(title.size() + 1);
    std::copy(title.begin(), title.end(), tmp.begin());
    TVINSERTSTRUCTW ti{};
    ti.hParent = parentItem ? parentItem : TVI_ROOT;
    ti.hInsertAfter = TVI_LAST;
    ti.item.mask = TVIF_TEXT | TVIF_PARAM;
    ti.item.pszText = tmp.data();
    int page = -1;
    FPDF_DEST dest = FPDFBookmark_GetDest(g.doc, bm);
    if (dest) page = FPDFDest_GetDestPageIndex(g.doc, dest);
    ti.item.lParam = (LPARAM)(page + 1);  // 0 = no destination
    HTREEITEM item = TreeView_InsertItem(g.bookmarks, &ti);
    int kids = FPDFBookmark_GetCount(bm);
    if (item && kids != 0)
      TreeView_Expand(g.bookmarks, item, kids > 0 ? TVE_EXPAND : TVE_COLLAPSE);
    AddBookmarkTree(bm, item, depth + 1);
  }
}

static void EnsureBookmarks()
{
  if (!g.bookmarks || g.doc == nullptr) return;
  TreeView_DeleteAllItems(g.bookmarks);
  if (g.pageCount > 0) AddBookmarkTree(nullptr, nullptr, 0);
  g.bmDirty = false;
}

static void SetAnnotText(FPDF_ANNOTATION a, const char* key, const wchar_t* value)
{
  FPDFAnnot_SetStringValue(a, key,
      reinterpret_cast<const FPDF_WCHAR*>(value));
}

// ---------------------------------------------------------------------------
// Annotations (PDFium FPDF_ANNOT* API)
// ---------------------------------------------------------------------------
// Creates a fresh annotation of the requested kind on |pageIdx| of |doc|.
// Parametrised on the document so the same code runs in the GUI and in the
// headless self-test.
static bool InsertAnnot(FPDF_DOCUMENT doc, int pageIdx, int kind)
{
  if (!doc) return false;
  FPDF_PAGE page = FPDF_LoadPage(doc, pageIdx);
  if (!page) return false;
  float pw = FPDF_GetPageWidthF(page);
  float ph = FPDF_GetPageHeightF(page);
  FPDF_ANNOTATION a = nullptr;
  bool ok = false;

  if (kind == ID_ANN_HL || kind == ID_ANN_UL)
  {
    int sub = (kind == ID_ANN_HL) ? FPDF_ANNOT_HIGHLIGHT : FPDF_ANNOT_UNDERLINE;
    float x = pw * 0.10f, w = pw * 0.55f;
    float y = (kind == ID_ANN_HL) ? ph * 0.85f : ph * 0.80f;
    float h = (kind == ID_ANN_HL) ? 18.0f : 6.0f;
    a = FPDFPage_CreateAnnot(page, sub);
    if (a)
    {
      FS_QUADPOINTSF q{};
      q.x1 = x; q.y1 = y + h; q.x2 = x + w; q.y2 = y + h;
      q.x3 = x + w; q.y3 = y; q.x4 = x; q.y4 = y;
      ok = FPDFAnnot_AppendAttachmentPoints(a, &q) != 0;
      FS_RECTF rc{x - 4.0f, y + h + 4.0f, x + w + 4.0f, y - 4.0f};
      ok = ok && (FPDFAnnot_SetRect(a, &rc) != 0);
      unsigned fr = (kind == ID_ANN_HL) ? 255 : 0, fg = (kind == ID_ANN_HL) ? 242 : 0;
      unsigned fb = (kind == ID_ANN_HL) ? 0 : 255;
      ok = ok && (FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, fr, fg, fb, 255) != 0);
      SetAnnotText(a, "Contents",
          (kind == ID_ANN_HL) ? L"Highlight added by Stitchup"
                              : L"Underline added by Stitchup");
    }
  }
  else if (kind == ID_ANN_NOTE)
  {
    a = FPDFPage_CreateAnnot(page, FPDF_ANNOT_TEXT);
    if (a)
    {
      FS_RECTF rc{pw - 90.0f, ph - 36.0f, pw - 30.0f, ph - 90.0f};
      ok = FPDFAnnot_SetRect(a, &rc) != 0;
      ok = ok && (FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, 255, 230, 0, 255) != 0);
      SetAnnotText(a, "Contents", L"Sticky note added by Stitchup.");
    }
  }
  else if (kind == ID_ANN_TEXT)
  {
    a = FPDFPage_CreateAnnot(page, FPDF_ANNOT_FREETEXT);
    if (a)
    {
      FS_RECTF rc{pw * 0.12f, ph * 0.55f, pw * 0.52f, ph * 0.42f};
      ok = FPDFAnnot_SetRect(a, &rc) != 0;
      ok = ok && (FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, 0, 0, 0, 255) != 0);
      ok = ok && (FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_InteriorColor,
                                     255, 255, 180, 255) != 0);
      SetAnnotText(a, "Contents", L"Free text added by Stitchup.");
    }
  }
  else if (kind == ID_ANN_SHAPE)
  {
    a = FPDFPage_CreateAnnot(page, FPDF_ANNOT_SQUARE);
    if (a)
    {
      float s = 70.0f;
      FS_RECTF rc{pw * 0.12f, ph * 0.35f, pw * 0.12f + s, ph * 0.35f - s};
      ok = FPDFAnnot_SetRect(a, &rc) != 0;
      ok = ok && (FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_Color, 0x1F, 0x6F, 0xEB, 255) != 0);
      ok = ok && (FPDFAnnot_SetColor(a, FPDFANNOT_COLORTYPE_InteriorColor,
                                     220, 235, 255, 180) != 0);
    }
  }
  else if (kind == ID_ANN_STAMP)
  {
    a = FPDFPage_CreateAnnot(page, FPDF_ANNOT_STAMP);
    if (a)
    {
      FS_RECTF rc{pw * 0.55f, ph * 0.18f, pw * 0.55f + 150.0f, ph * 0.18f - 52.0f};
      ok = FPDFAnnot_SetRect(a, &rc) != 0;
      SetAnnotText(a, "Name", L"Draft");
      SetAnnotText(a, "Contents", L"DRAFT");
    }
  }

  if (a)
  {
    if (ok) FPDFAnnot_SetFlags(a, FPDF_ANNOT_FLAG_PRINT);
    FPDFPage_CloseAnnot(a);
  }
  FPDF_ClosePage(page);
  return ok;
}

static void InsertAnnotCurrent(int kind)
{
  if (!g.doc || g.pageCount == 0) return;
  if (InsertAnnot(g.doc, g.selected, kind))
  {
    InvalidateRect(g.canvas, nullptr, TRUE);
    InvalidateRect(g.thumbs, nullptr, TRUE);
    InvalidateRect(g.status, nullptr, TRUE);
  }
}

static BOOL CALLBACK PaneTabEnum(HWND h, LPARAM)
{
  Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(h, GWLP_USERDATA));
  if (b && b->tab)
  {
    LONG_PTR id = GetWindowLongPtrW(h, GWLP_ID);
    b->pressed = (id == ID_PANE_THUMBS) ? (g.pane == 0)
                 : (id == ID_PANE_BOOKMARKS) ? (g.pane == 1) : false;
    InvalidateRect(h, nullptr, TRUE);
  }
  return TRUE;
}

static void SetPane(int p)
{
  g.pane = p;
  if (p == 1 && g.doc && g.bmDirty) EnsureBookmarks();
  ShowWindow(g.thumbs, p == 0 ? SW_SHOW : SW_HIDE);
  ShowWindow(g.bookmarks, p == 1 ? SW_SHOW : SW_HIDE);
  EnumChildWindows(g.paneTabs, PaneTabEnum, 0);
  if (p == 1 && g.doc && g.pageCount)
  {
    g.selected = 0;
    GotoPageIndex(0);
  }
}

static void DoCommand(int id)
{
  switch (id)
  {
    case ID_NEW:    NewDoc(); break;
    case ID_OPEN:
    {
      wchar_t file[MAX_PATH] = L"";
      OPENFILENAMEW ofn{};
      ofn.lStructSize = sizeof(ofn);
      ofn.hwndOwner = g.frame;
      ofn.lpstrFilter = L"PDF Files (*.pdf)\0*.pdf\0All Files\0*.*\0\0";
      ofn.lpstrFile = file;
      ofn.nMaxFile = MAX_PATH;
      ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY;
      if (GetOpenFileNameW(&ofn)) LoadDoc(file);
      break;
    }
    case ID_SAVE:   SaveInPlace(); break;
    case ID_SAVEAS: SaveAs(); break;
    case ID_SAVEENC: SaveAsEncrypted(); break;
    case ID_IMPORT: ImportPdf(); break;
    case ID_DELETE: DeletePage(); break;
    case ID_ADD:    AddPage(); break;
    case ID_ROTL:   RotatePage(3); break;
    case ID_ROTR:   RotatePage(1); break;
    case ID_TOOL_SELECT: SetToolSelect(!g.toolSelect); break;
    case ID_OBJ_EDIT:   EditSelectedText(); break;
    case ID_OBJ_DELETE: DeleteSelectedObject(); break;
    case ID_OBJ_RECOLOR: ApplyRecolorSelected(); break;
    case ID_ZOOM_OUT: ZoomTo(g.zoom / 1.25, false); break;
    case ID_ZOOM_IN:  ZoomTo(g.zoom * 1.25, false); break;
    case ID_ZOOM100:  ZoomTo(1.0, false); break;
    case ID_FITW:     FitWidth(); break;
    case ID_FITP:     FitPage(); break;
    case ID_PREV:     GoPage(-1); break;
    case ID_NEXT:     GoPage(1); break;
    case ID_TAB_HOME:    SwitchRibbonTab(0); break;
    case ID_TAB_VIEW:    SwitchRibbonTab(1); break;
    case ID_TAB_TOOLS:   SwitchRibbonTab(2); break;
    case ID_PANE_THUMBS: SetPane(0); break;
    case ID_PANE_BOOKMARKS: SetPane(1); break;
    case ID_ANN_HL:    InsertAnnotCurrent(ID_ANN_HL); break;
    case ID_ANN_UL:    InsertAnnotCurrent(ID_ANN_UL); break;
    case ID_ANN_NOTE:  InsertAnnotCurrent(ID_ANN_NOTE); break;
    case ID_ANN_TEXT:  InsertAnnotCurrent(ID_ANN_TEXT); break;
    case ID_ANN_SHAPE: InsertAnnotCurrent(ID_ANN_SHAPE); break;
    case ID_ANN_STAMP: InsertAnnotCurrent(ID_ANN_STAMP); break;
    case ID_PAGE_EXTRACT: ExtractCurrentPage(); break;
    case ID_PAGE_SPLIT:   SplitAllPages(); break;
    case ID_PAGE_CROP:    CropCurrentPageToContent(); break;
    case ID_EXPORT_TEXT:  ExportTextAll(); break;
    case ID_EXPORT_CSV:   ExportCsvAll(); break;
    case ID_WATERMARK:    WatermarkCurrentDoc(); break;
    case ID_THEME:        ToggleTheme(); break;
    case ID_ABOUT:
      MessageBoxW(g.frame,
        L"Stitchup PDF Editor\n\nPortable PDF viewer/editor\n"
        L"Engine: PDFium (BSD-3-Clause, Chromium project)\n"
        L"UI: native Win32 (zero runtime dependencies)\n\n"
        L"Shortcuts:\n"
        L"  Ctrl+O open   Ctrl+S save   Ctrl+Shift+S save as\n"
        L"  Ctrl+R rotate CW   Ctrl+Shift+R rotate CCW\n"
        L"  Ctrl+= / Ctrl+- zoom   Ctrl+0 fit page   Ctrl+1 100%\n"
        L"  Del delete page   Ctrl+W fit width   Ctrl+PgUp/PgDn page",
        L"About Stitchup", MB_OK | MB_ICONINFORMATION);
      break;
    case ID_EXIT:
      PostMessageW(g.frame, WM_CLOSE, 0, 0);
      break;
  }
}

static HMENU BuildMenu()
{
  HMENU bar = CreateMenu();
  auto addItem = [&](HMENU m, UINT id, const wchar_t* s)
  {
    AppendMenuW(m, MF_STRING, id, s);
  };
  HMENU file = CreatePopupMenu();
  addItem(file, ID_NEW, L"New\tCtrl+N");
  addItem(file, ID_OPEN, L"Open...\tCtrl+O");
  addItem(file, ID_SAVE, L"Save\tCtrl+S");
  addItem(file, ID_SAVEAS, L"Save As...\tCtrl+Shift+S");
  addItem(file, ID_SAVEENC, L"Save As Encrypted...");
  addItem(file, ID_IMPORT, L"Import PDF...");
  addItem(file, ID_EXPORT_TEXT, L"Export Text...");
  addItem(file, ID_EXPORT_CSV, L"Export CSV...");
  addItem(file, ID_WATERMARK, L"Watermark...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  addItem(file, ID_EXIT, L"Exit");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

  HMENU edit = CreatePopupMenu();
  addItem(edit, ID_ROTR, L"Rotate Right\tCtrl+R");
  addItem(edit, ID_ROTL, L"Rotate Left\tCtrl+Shift+R");
  addItem(edit, ID_DELETE, L"Delete Page\tDel");
  addItem(edit, ID_ADD, L"Add Page...");
  AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
  addItem(edit, ID_TOOL_SELECT, L"Select / Move Content Object");
  addItem(edit, ID_OBJ_EDIT, L"Edit Text...\tDouble-click");
  addItem(edit, ID_OBJ_DELETE, L"Delete Selected Object\tCtrl+Del");
  addItem(edit, ID_OBJ_RECOLOR, L"Recolor Selected Object...");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)edit, L"&Edit");

  HMENU pages = CreatePopupMenu();
  addItem(pages, ID_PAGE_EXTRACT, L"Extract Current Page...");
  addItem(pages, ID_PAGE_SPLIT, L"Split Document (one file per page)");
  addItem(pages, ID_PAGE_CROP, L"Auto-Crop Current Page");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)pages, L"Pa&ges");

  HMENU annotate = CreatePopupMenu();
  addItem(annotate, ID_ANN_HL, L"Highlight");
  addItem(annotate, ID_ANN_UL, L"Underline");
  addItem(annotate, ID_ANN_NOTE, L"Sticky Note");
  addItem(annotate, ID_ANN_TEXT, L"Text Box");
  addItem(annotate, ID_ANN_SHAPE, L"Shape");
  addItem(annotate, ID_ANN_STAMP, L"Stamp");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)annotate, L"Anno&tate");

  HMENU view = CreatePopupMenu();
  addItem(view, ID_ZOOM_IN, L"Zoom In\tCtrl+=");
  addItem(view, ID_ZOOM_OUT, L"Zoom Out\tCtrl+-");
  addItem(view, ID_ZOOM100, L"100%\tCtrl+1");
  addItem(view, ID_FITW, L"Fit Width\tCtrl+W");
  addItem(view, ID_FITP, L"Fit Page\tCtrl+0");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  addItem(view, ID_PREV, L"Previous Page\tCtrl+PgUp");
  addItem(view, ID_NEXT, L"Next Page\tCtrl+PgDn");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  addItem(view, ID_PANE_THUMBS, L"Page Thumbnails");
  addItem(view, ID_PANE_BOOKMARKS, L"Bookmarks");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
  addItem(view, ID_THEME, L"Dark Mode\tCtrl+D");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");

  HMENU help = CreatePopupMenu();
  addItem(help, ID_ABOUT, L"About");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
  return bar;
}

static LRESULT CALLBACK FrameProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_GETICON:
      return (LRESULT)LoadIconW(g.inst, MAKEINTRESOURCEW(101));
    case WM_CREATE:
    {
      g.toolbar = CreateWindowExW(0, L"SKToolbar", nullptr, WS_CHILD | WS_VISIBLE,
                                  0, 0, 600, RIB_H, hw, nullptr, g.inst, nullptr);
      BuildToolbar(g.toolbar);
      LayoutRibbon();

      g.paneTabs = CreateWindowExW(0, L"SKToolbar", nullptr,
                                   WS_CHILD | WS_VISIBLE,
                                   0, RIB_H, g.thumbsW, PANE_TAB_H,
                                   hw, nullptr, g.inst, nullptr);
      int ha = std::max(30, g.thumbsW / 2 - 4);
      MakeBtn(g.paneTabs, ID_PANE_THUMBS, L"Page Thumbnails",
              2, 3, ha, 20, true);
      MakeBtn(g.paneTabs, ID_PANE_BOOKMARKS, L"Bookmarks",
              g.thumbsW - ha - 2, 3, ha, 20, true);

      g.thumbs = CreateWindowExW(0, L"SKThumbs", nullptr, WS_CHILD | WS_VISIBLE |
                                 WS_VSCROLL, 0, RIB_H + PANE_TAB_H,
                                 g.thumbsW, 300, hw, nullptr, g.inst, nullptr);
      g.bookmarks = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                    TVS_HASBUTTONS | TVS_HASLINES |
                                    TVS_LINESATROOT,
                                    0, RIB_H + PANE_TAB_H,
                                    g.thumbsW, 300, hw, nullptr, g.inst, nullptr);
      SendMessageW(g.bookmarks, WM_SETFONT, (WPARAM)g.font, TRUE);
      ShowWindow(g.bookmarks, SW_HIDE);
      SetPane(0);

      g.split = CreateWindowExW(0, L"SKSplit", nullptr, WS_CHILD | WS_VISIBLE,
                                g.thumbsW, RIB_H, 6, 300, hw, nullptr, g.inst,
                                nullptr);
      g.canvas = CreateWindowExW(0, L"SKCanvas", nullptr,
                                 WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL,
                                 g.thumbsW + 6, RIB_H, 600, 300,
                                 hw, nullptr, g.inst, nullptr);
      g.status = CreateWindowExW(0, L"SKStatus", nullptr, WS_CHILD | WS_VISIBLE,
                                 0, 0, 800, 28, hw, nullptr, g.inst, nullptr);
      SetWindowSubclass(g.status, StatusProc, 1, 0);
      DragAcceptFiles(hw, TRUE);
      SetFocus(hw);
      break;
    }
    case WM_COMMAND:
      if (HIWORD(wp) == 0 || HIWORD(wp) == 1)
        DoCommand(LOWORD(wp));
      return 0;
    case WM_DROPFILES:
    {
      HDROP hd = reinterpret_cast<HDROP>(wp);
      wchar_t file[MAX_PATH];
      if (DragQueryFileW(hd, 0, file, MAX_PATH) > 0)
        LoadDoc(file);
      DragFinish(hd);
      return 0;
    }
    case WM_NOTIFY:
    {
      NMHDR* nm = reinterpret_cast<NMHDR*>(lp);
      if (nm && nm->hwndFrom == g.bookmarks && nm->code == TVN_SELCHANGED)
      {
        NMTREEVIEWW* tv = reinterpret_cast<NMTREEVIEWW*>(lp);
        if (tv)
        {
          LRESULT page = tv->itemNew.lParam - 1;
          if (page >= 0 && page < g.pageCount)
            GotoPageIndex((int)page);
        }
        return 0;
      }
      break;
    }
    case WM_GETMINMAXINFO:
    {
      MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lp);
      mmi->ptMinTrackSize.x = 780;
      mmi->ptMinTrackSize.y = 500;
      return 0;
    }
    case WM_SIZE:
    {
      int w = LOWORD(lp), h = HIWORD(lp);
      if (g.toolbar)
      {
        int paneY = RIB_H + PANE_TAB_H;
        int paneH = std::max(10, h - paneY - 28);
        int canvasH = std::max(10, h - RIB_H - 28);
        SetWindowPos(g.toolbar, nullptr, 0, 0, w, RIB_H, SWP_NOZORDER);
        SetWindowPos(g.paneTabs, nullptr, 0, RIB_H, g.thumbsW, PANE_TAB_H,
                     SWP_NOZORDER);
        SetWindowPos(g.status, nullptr, 0, h - 28, w, 28, SWP_NOZORDER);
        SetWindowPos(g.thumbs, nullptr, 0, paneY, g.thumbsW, paneH,
                     SWP_NOZORDER);
        SetWindowPos(g.bookmarks, nullptr, 0, paneY, g.thumbsW, paneH,
                     SWP_NOZORDER);
        SetWindowPos(g.split, nullptr, g.thumbsW, RIB_H, 6,
                     PANE_TAB_H + paneH, SWP_NOZORDER);
        SetWindowPos(g.canvas, nullptr, g.thumbsW + 6, RIB_H,
                     std::max(100, w - g.thumbsW - 6), canvasH, SWP_NOZORDER);
        InvalidateRect(g.thumbs, nullptr, TRUE);
        InvalidateRect(g.canvas, nullptr, TRUE);
      }
      return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
      return 0;
    case WM_KEYDOWN:
    {
      bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
      bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      int vk = (int)wp;
      if (ctrl)
      {
        switch (vk)
        {
          case 'N': DoCommand(ID_NEW); return 0;
          case 'O': DoCommand(ID_OPEN); return 0;
          case 'S': DoCommand(shift ? ID_SAVEAS : ID_SAVE); return 0;
          case 'R': DoCommand(shift ? ID_ROTL : ID_ROTR); return 0;
          case VK_OEM_PLUS: case VK_ADD: DoCommand(ID_ZOOM_IN); return 0;
          case VK_OEM_MINUS: case VK_SUBTRACT: DoCommand(ID_ZOOM_OUT); return 0;
          case '0': DoCommand(ID_FITP); return 0;
          case '1': DoCommand(ID_ZOOM100); return 0;
          case 'W': DoCommand(ID_FITW); return 0;
          case VK_PRIOR: GoPage(-1); return 0;
          case VK_NEXT: GoPage(1); return 0;
          case VK_DELETE: DoCommand(ID_OBJ_DELETE); return 0;
        }
      }
      else
      {
        switch (vk)
        {
          case VK_DELETE: DoCommand(g.toolSelect ? ID_OBJ_DELETE : ID_DELETE); return 0;
        }
      }
      return 0;
    }
    case WM_CLOSE:
    {
      if (g.dirty)
      {
        int r = MessageBoxW(hw, L"Save changes?", L"Stitchup",
                            MB_YESNOCANCEL | MB_ICONQUESTION);
        if (r == IDCANCEL) return 0;
        if (r == IDYES) DoCommand(ID_SAVE);
      }
      break;
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Sample PDF (self-test fixture)
// ---------------------------------------------------------------------------
static std::string MakeSamplePdf()
{
  static const std::string content =
    "BT /F1 24 Tf 72 720 Td (Stitchup PDF Editor) Tj ET\n"
    "0 0 1 rg 72 672 168 16 re f\n";
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offs;
  auto emit = [&](const std::string& obj) {
    offs.push_back(out.size());
    out += obj;
    out += "\n";
  };
  emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj");
  emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj");
  emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>\nendobj");
  emit("4 0 obj\n<< /Length " + std::to_string(content.size()) +
       " >>\nstream\n" + content + "endstream\nendobj");
  emit("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj");

  size_t xref = out.size();
  out += "xref\n0 6\n";
  out += "0000000000 65535 f \n";
  for (size_t o : offs)
  {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", o);
    out += line;
  }
  out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
  out += std::to_string(xref);
  out += "\n%%EOF\n";
  return out;
}

static std::string MakeOutlinePdf()
{
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offs;
  auto emit = [&](const std::string& obj) {
    offs.push_back(out.size());
    out += obj;
    out += "\n";
  };
  emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R >>\nendobj");
  emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj");
  emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj");
  emit("4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj");
  emit("5 0 obj\n<< /Type /Outlines /First 6 0 R /Last 7 0 R /Count 2 >>\nendobj");
  emit("6 0 obj\n<< /Title (Cover) /Parent 5 0 R /Next 7 0 R "
       "/Dest [3 0 R /Fit] >>\nendobj");
  emit("7 0 obj\n<< /Title (Details) /Parent 5 0 R /Prev 6 0 R "
       "/First 8 0 R /Last 8 0 R /Count 1 /Dest [4 0 R /Fit] >>\nendobj");
  emit("8 0 obj\n<< /Title (Details - Sub) /Parent 7 0 R "
       "/Dest [4 0 R /Fit] >>\nendobj");
  size_t xref = out.size();
  out += "xref\n0 9\n";
  out += "0000000000 65535 f \n";
  for (size_t o : offs)
  {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", o);
    out += line;
  }
  out += "trailer\n<< /Size 9 /Root 1 0 R >>\nstartxref\n";
  out += std::to_string(xref);
  out += "\n%%EOF\n";
  return out;
}

static std::string MakeLinkedPdf()
{
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offs;
  auto emit = [&](const std::string& obj) {
    offs.push_back(out.size());
    out += obj;
    out += "\n";
  };
  emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj");
  emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>\nendobj");
  emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       "/Annots [5 0 R 6 0 R] >>\nendobj");
  emit("4 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>\nendobj");
  emit("5 0 obj\n<< /Type /Annot /Subtype /Link /Rect [100 500 300 700] "
       "/Border [0 0 0] /Dest [4 0 R /XYZ 0 792 0] >>\nendobj");
  emit("6 0 obj\n<< /Type /Annot /Subtype /Link /Rect [360 200 560 300] "
       "/Border [0 0 0] /A << /S /URI /URI (https://example.com/stitchup) >> "
       ">>\nendobj");
  size_t xref = out.size();
  out += "xref\n0 7\n";
  out += "0000000000 65535 f \n";
  for (size_t o : offs)
  {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", o);
    out += line;
  }
  out += "trailer\n<< /Size 7 /Root 1 0 R >>\nstartxref\n";
  out += std::to_string(xref);
  out += "\n%%EOF\n";
  return out;
}

static std::string MakeTextPdf()
{
  const std::string txt = "BT /F1 12 Tf 72 700 Td (Hello, World! Export me.) Tj ET";
  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offs;
  auto emit = [&](const std::string& obj) {
    offs.push_back(out.size());
    out += obj;
    out += "\n";
  };
  emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj");
  emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj");
  emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj");
  emit("4 0 obj\n<< /Length " + std::to_string(txt.size()) +
       " >>\nstream\n" + txt + "\nendstream\nendobj");
  emit("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj");
  size_t xref = out.size();
  out += "xref\n0 6\n";
  out += "0000000000 65535 f \n";
  for (size_t o : offs)
  {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", o);
    out += line;
  }
  out += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n";
  out += std::to_string(xref);
  out += "\n%%EOF\n";
  return out;
}

// ---------------------------------------------------------------------------
// Minimal MD5 + RC4 (used only to build the encrypted selftest fixture)
// ---------------------------------------------------------------------------
struct Md5Ctx
{
  unsigned int state[4];
  unsigned long long bits;
  unsigned char in[64];
  int inlen;
};

static void Md5Transform(unsigned int state[4], const unsigned char block[64])
{
  static const unsigned int K[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391 };
  unsigned int a = state[0], b = state[1], c = state[2], d = state[3];
  unsigned int x[16];
  for (int i = 0; i < 16; i++)
    x[i] = (unsigned int)block[i * 4] |
           ((unsigned int)block[i * 4 + 1] << 8) |
           ((unsigned int)block[i * 4 + 2] << 16) |
           ((unsigned int)block[i * 4 + 3] << 24);
#define F_(x,y,z) (((x)&(y)) | (~(x)&(z)))
#define G_(x,y,z) (((x)&(z)) | ((y)&~(z)))
#define H_(x,y,z) ((x) ^ (y) ^ (z))
#define I_(x,y,z) ((y) ^ ((x) | ~(z)))
#define ROT_(x,s) (((x)<<(s)) | ((x)>>(32-(s))))
#define STEP_(fn,k,s,i) { unsigned int _w = b + ROT_(a + fn(b,c,d) + x[i] + K[k], s); a = d; d = c; c = b; b = _w; }
  STEP_(F_,0,7,0) STEP_(F_,1,12,1) STEP_(F_,2,17,2) STEP_(F_,3,22,3)
  STEP_(F_,4,7,4) STEP_(F_,5,12,5) STEP_(F_,6,17,6) STEP_(F_,7,22,7)
  STEP_(F_,8,7,8) STEP_(F_,9,12,9) STEP_(F_,10,17,10) STEP_(F_,11,22,11)
  STEP_(F_,12,7,12) STEP_(F_,13,12,13) STEP_(F_,14,17,14) STEP_(F_,15,22,15)
  STEP_(G_,16,5,1) STEP_(G_,17,9,6) STEP_(G_,18,14,11) STEP_(G_,19,20,0)
  STEP_(G_,20,5,5) STEP_(G_,21,9,10) STEP_(G_,22,14,15) STEP_(G_,23,20,4)
  STEP_(G_,24,5,9) STEP_(G_,25,9,14) STEP_(G_,26,14,3) STEP_(G_,27,20,8)
  STEP_(G_,28,5,13) STEP_(G_,29,9,2) STEP_(G_,30,14,7) STEP_(G_,31,20,12)
  STEP_(H_,32,4,5) STEP_(H_,33,11,8) STEP_(H_,34,16,11) STEP_(H_,35,23,14)
  STEP_(H_,36,4,1) STEP_(H_,37,11,4) STEP_(H_,38,16,7) STEP_(H_,39,23,10)
  STEP_(H_,40,4,13) STEP_(H_,41,11,0) STEP_(H_,42,16,3) STEP_(H_,43,23,6)
  STEP_(H_,44,4,9) STEP_(H_,45,11,12) STEP_(H_,46,16,15) STEP_(H_,47,23,2)
  STEP_(I_,48,6,0) STEP_(I_,49,10,7) STEP_(I_,50,15,14) STEP_(I_,51,21,5)
  STEP_(I_,52,6,12) STEP_(I_,53,10,3) STEP_(I_,54,15,10) STEP_(I_,55,21,1)
  STEP_(I_,56,6,8) STEP_(I_,57,10,15) STEP_(I_,58,15,6) STEP_(I_,59,21,13)
  STEP_(I_,60,6,4) STEP_(I_,61,10,11) STEP_(I_,62,15,2) STEP_(I_,63,21,9)
#undef STEP_
#undef ROT_
#undef F_
#undef G_
#undef H_
#undef I_
  state[0] += a; state[1] += b; state[2] += c; state[3] += d;
}

static void Md5Init(Md5Ctx* c)
{
  c->state[0] = 0x67452301; c->state[1] = 0xefcdab89;
  c->state[2] = 0x98badcfe; c->state[3] = 0x10325476;
  c->bits = 0; c->inlen = 0;
}

static void Md5Update(Md5Ctx* c, const unsigned char* data, size_t len)
{
  c->bits += (unsigned long long)len * 8;
  while (len)
  {
    unsigned int n = 64 - c->inlen;
    if (n > len) n = (unsigned int)len;
    memcpy(c->in + c->inlen, data, n);
    c->inlen += (int)n;
    data += n;
    len -= n;
    if (c->inlen == 64) { Md5Transform(c->state, c->in); c->inlen = 0; }
  }
}

static void Md5Final(Md5Ctx* c, unsigned char out[16])
{
  const unsigned long long bits = c->bits;
  unsigned char pad[128] = {};
  pad[0] = 0x80;
  const int need = (c->inlen < 56) ? (56 - c->inlen) : (120 - c->inlen);
  Md5Update(c, pad, (size_t)need);
  unsigned char lenb[8];
  for (int i = 0; i < 8; i++) lenb[i] = (unsigned char)((bits >> (8 * i)) & 0xff);
  Md5Update(c, lenb, 8);
  for (int i = 0; i < 4; i++)
  {
    out[i * 4 + 0] = (unsigned char)(c->state[i] & 0xff);
    out[i * 4 + 1] = (unsigned char)((c->state[i] >> 8) & 0xff);
    out[i * 4 + 2] = (unsigned char)((c->state[i] >> 16) & 0xff);
    out[i * 4 + 3] = (unsigned char)((c->state[i] >> 24) & 0xff);
  }
}

struct Rc4Ctx
{
  unsigned char s[256];
  int i, j;
};

static void Rc4Init(Rc4Ctx* r, const unsigned char* key, int keylen)
{
  for (int k = 0; k < 256; k++) r->s[k] = (unsigned char)k;
  int j = 0;
  for (int k = 0; k < 256; k++)
  {
    j = (j + r->s[k] + key[k % keylen]) & 0xff;
    unsigned char t = r->s[k]; r->s[k] = r->s[j]; r->s[j] = t;
  }
  r->i = r->j = 0;
}

static void Rc4Crypt(Rc4Ctx* r, const unsigned char* in, unsigned char* out, int len)
{
  int a = r->i, b = r->j;
  for (int k = 0; k < len; k++)
  {
    a = (a + 1) & 0xff;
    b = (b + r->s[a]) & 0xff;
    unsigned char t = r->s[a]; r->s[a] = r->s[b]; r->s[b] = t;
    out[k] = in[k] ^ r->s[(r->s[a] + r->s[b]) & 0xff];
  }
  r->i = a;
  r->j = b;
}

// The 32-byte padding the Standard security handler appends to passwords
// (ISO 32000-1, 7.6.3.3).
static const unsigned char kPadding[32] = {
  0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
  0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A };

// Builds an encrypted single-page PDF (Standard security handler V1) whose
// user password is "stitchup". Variants: 1 = R2/40-bit, 2 = R3/128-bit,
// 3 = R2/128-bit. The selftest probes each until this pdfium accepts one.
static std::string MakeEncryptedPdf(int variant)
{
  const std::string user_pw = "stitchup";
  const std::string owner_pw = "clockwork";
  auto pad32 = [&](const std::string& pw, unsigned char out[32])
  {
    const size_t n = pw.size();
    for (size_t i = 0; i < 32; i++)
      out[i] = i < n ? (unsigned char)pw[i] : kPadding[i - n];
  };
  unsigned char pad_o[32], pad_u[32];
  pad32(owner_pw, pad_o);
  pad32(user_pw, pad_u);

  const int revision = (variant == 2) ? 3 : 2;
  const int keylen = (variant == 2) ? 16 : (variant == 3 ? 16 : 5);
  const unsigned char P_le[4] = { 0xFC, 0xFF, 0xFF, 0xFF };
  const unsigned char* ID1 = reinterpret_cast<const unsigned char*>("STITCHUPENCID012");  // 16 bytes

  unsigned char okey[16];
  { Md5Ctx m; Md5Init(&m); Md5Update(&m, pad_o, 32); Md5Final(&m, okey); }
  unsigned char O[32];
  { Rc4Ctx r; Rc4Init(&r, okey, keylen); Rc4Crypt(&r, kPadding, O, 32); }

  unsigned char key[16];
  { Md5Ctx m; Md5Init(&m); Md5Update(&m, pad_u, 32); Md5Update(&m, O, 32);
    Md5Update(&m, P_le, 4); Md5Update(&m, ID1, 16); Md5Final(&m, key); }
  if (revision >= 3)
  {
    for (int i = 0; i < 50; i++)
    {
      Md5Ctx m; Md5Init(&m); Md5Update(&m, key, 16); Md5Final(&m, key);
    }
  }
  unsigned char U[32];
  if (revision == 2)
  {
    Rc4Ctx r; Rc4Init(&r, key, keylen); Rc4Crypt(&r, kPadding, U, 32);
  }
  else
  {
    unsigned char ubase[16];
    { Md5Ctx m; Md5Init(&m); Md5Update(&m, kPadding, 32); Md5Update(&m, ID1, 16); Md5Final(&m, ubase); }
    Rc4Ctx r; Rc4Init(&r, key, keylen); Rc4Crypt(&r, ubase, U, 16);
  }

  const std::string txt = "BT /F1 12 Tf 72 700 Td (Hello, World! Export me.) Tj ET";
  std::string enc(txt.size(), '\0');
  {
    // Per-object crypt key = MD5(K || objnum(3 LE) || gen(2 LE)), truncated
    // to min(keylen+5, 16) for RC4.
    unsigned char mat[21] = {};
    memcpy(mat, key, (size_t)keylen);
    mat[keylen + 0] = 0x04;  // contents object number=4
    mat[keylen + 1] = 0x00;
    mat[keylen + 2] = 0x00;
    mat[keylen + 3] = 0x00;
    mat[keylen + 4] = 0x00;
    const int streamKeyLen = std::min(keylen + 5, 16);
    unsigned char streamKey[16];
    { Md5Ctx m; Md5Init(&m); Md5Update(&m, mat, (size_t)keylen + 5); Md5Final(&m, streamKey); }
    Rc4Ctx r; Rc4Init(&r, streamKey, streamKeyLen);
    Rc4Crypt(&r, reinterpret_cast<const unsigned char*>(txt.data()),
             reinterpret_cast<unsigned char*>(&enc[0]), (int)txt.size());
  }

  auto hexof = [](const unsigned char* b, int n)
  {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    for (int i = 0; i < n; i++) { s += hx[b[i] >> 4]; s += hx[b[i] & 15]; }
    return s;
  };
  const std::string encHexO = hexof(O, 32);
  const std::string encHexU = hexof(U, 32);

  std::string out = "%PDF-1.4\n";
  std::vector<size_t> offs;
  auto emit = [&](const std::string& obj) {
    offs.push_back(out.size());
    out += obj;
    out += "\n";
  };
  emit("1 0 obj\n<< /Type /Catalog /Pages 2 0 R >>\nendobj");
  emit("2 0 obj\n<< /Type /Pages /Kids [3 0 R] /Count 1 >>\nendobj");
  emit("3 0 obj\n<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
       "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>\nendobj");
  emit("4 0 obj\n<< /Length " + std::to_string(enc.size()) + " >>\nstream\n" +
       enc + "\nendstream\nendobj");
  emit("5 0 obj\n<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>\nendobj");
  emit("6 0 obj\n<< /Filter /Standard /V 1 /R " + std::to_string(revision) +
       " /Length " + std::to_string(keylen * 8) +
       " /O <" + encHexO + "> /U <" + encHexU + "> /P -4 >>\nendobj");
  const size_t xref = out.size();
  out += "xref\n0 7\n";
  out += "0000000000 65535 f \n";
  for (size_t o : offs)
  {
    char line[32];
    std::snprintf(line, sizeof(line), "%010zu 00000 n \n", o);
    out += line;
  }
  out += "trailer\n<< /Size 7 /Root 1 0 R /Encrypt 6 0 R ";
  out += "/ID [<" + hexof(ID1, 16) + "> <" + hexof(ID1, 16) + ">] >>\nstartxref\n";
  out += std::to_string(xref);
  out += "\n%%EOF\n";
  return out;
}

// ---------------------------------------------------------------------------
// Encrypt-on-save: rewrites a serialized PDF into a password-protected copy
// using the Standard security handler (V2/R3, 128-bit RC4). The byte layout
// mirrors what pdfium's own SetPassword produces, so this pdfium build can
// reopen the file with FPDF_LoadDocument(userPassword). RC4 preserves stream
// length, so existing /Length values remain valid after encryption.
// ---------------------------------------------------------------------------

static void Md5Once(const void* data, size_t len, unsigned char out[16])
{
  Md5Ctx m;
  Md5Init(&m);
  Md5Update(&m, static_cast<const unsigned char*>(data), len);
  Md5Final(&m, out);
}

static void Rc4One(const unsigned char key[16], unsigned char* data, size_t len)
{
  Rc4Ctx r;
  Rc4Init(&r, key, 16);
  Rc4Crypt(&r, data, data, (int)len);
}

static void PadPassword(const std::string& pw, unsigned char out[32])
{
  const size_t n = pw.size();
  for (size_t i = 0; i < 32; i++)
    out[i] = i < n ? (unsigned char)pw[i] : kPadding[i - n];
}

static void RandomFileId(unsigned char id[16])
{
  // xorshift64* seeded from timer/process state (nondeterministic enough for a
  // file identifier; the encryption itself does not rely on this value).
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  unsigned long long x =
      ((unsigned long long)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  x ^= (unsigned long long)GetTickCount64();
  x ^= (unsigned long long)GetCurrentProcessId() << 32;
  x ^= reinterpret_cast<unsigned long long>(&x) >> 3;
  if (!x) x = 1ULL;
  auto next = [&]() {
    x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
    return x * 0x2545F4914F6CDD1DULL;
  };
  for (int i = 0; i < 16; i++)
    id[i] = (unsigned char)(next() >> ((i & 7) * 8));
}

// Builds the R3 file key and the O/U entries. The U entry follows the chained
// RC4 algorithm (ISO 32000-1 7.6.3.4) that pdfium's CheckUserPassword expects.
static bool BuildR3Keys(const std::string& userPw, const std::string& ownerPw,
                        const unsigned char id[16], unsigned char fileKey[16],
                        unsigned char O[32], unsigned char U[32])
{
  const std::string owner = ownerPw.empty() ? userPw : ownerPw;
  unsigned char pad_o[32], pad_u[32];
  PadPassword(owner, pad_o);
  PadPassword(userPw, pad_u);

  // O (Algorithm 3.3): 50x MD5(pad_owner), then RC4(key, pad_owner).
  unsigned char F[16];
  Md5Once(pad_o, 32, F);
  for (int i = 0; i < 50; i++) Md5Once(F, 16, F);
  Rc4One(F, pad_o, 32);
  memcpy(O, pad_o, 32);

  // File key (Algorithm 3.4): MD5(pad_user || O || P_le || ID), then 50x MD5.
  const unsigned char P_le[4] = { 0xFC, 0xFF, 0xFF, 0xFF };
  {
    Md5Ctx m;
    Md5Init(&m);
    Md5Update(&m, pad_u, 32);
    Md5Update(&m, O, 32);
    Md5Update(&m, P_le, 4);
    Md5Update(&m, id, 16);
    Md5Final(&m, fileKey);
  }
  for (int i = 0; i < 50; i++) Md5Once(fileKey, 16, fileKey);

  // U (Algorithm 3.5): start with MD5(pad || ID), then RC4 passes over the
  // first 16 bytes: first with the file key, then with key XOR 1..19. The
  // trailing 16 bytes are MD5 of the transformed first 16 (pdfium style).
  {
    Md5Ctx m;
    Md5Init(&m);
    Md5Update(&m, kPadding, 32);
    Md5Update(&m, id, 16);
    Md5Final(&m, U);
  }
  Rc4One(fileKey, U, 16);
  for (int i = 1; i <= 19; i++)
  {
    unsigned char k2[16];
    for (int j = 0; j < 16; j++) k2[j] = fileKey[j] ^ (unsigned char)i;
    Rc4One(k2, U, 16);
  }
  Md5Once(U, 16, U + 16);
  return true;
}

static bool BytesAt(const std::vector<unsigned char>& b, size_t at, const char* w)
{
  for (size_t i = 0; w[i]; i++)
    if (at + i >= b.size() || b[at + i] != (unsigned char)w[i]) return false;
  return true;
}

static size_t SearchToken(const std::vector<unsigned char>& b, size_t from, const char* w)
{
  const size_t n = strlen(w);
  for (size_t i = from; i + n <= b.size(); i++)
    if (BytesAt(b, i, w)) return i;
  return (size_t)-1;
}

static bool ParseRef(const std::vector<unsigned char>& b, size_t from, const char* key,
                     unsigned int* numOut)
{
  size_t k = SearchToken(b, from, key);
  if (k == (size_t)-1) return false;
  size_t p = k + strlen(key);
  while (p < b.size() && (b[p] == ' ' || b[p] == '\t' || b[p] == '\r' || b[p] == '\n')) p++;
  unsigned int num = 0;
  size_t d = p;
  while (d < b.size() && b[d] >= '0' && b[d] <= '9') { num = num * 10 + (b[d] - '0'); d++; }
  if (d == p) return false;
  *numOut = num;
  return true;
}

// Rewrites the object body [body, end) into 'out', encrypting every literal
// string, hex string and stream with the object key (RC4, length preserving).
static void EmitEncryptedBody(const std::vector<unsigned char>& plain, size_t body,
                              size_t end, const unsigned char objKey[16],
                              std::vector<unsigned char>& out)
{
  size_t p = body;
  const size_t n = end;
  while (p < n)
  {
    const unsigned char c = plain[p];
    if (c == '(')
    {
      int depth = 1;
      size_t q = p + 1;
      for (; q < n && depth; q++)
      {
        if (plain[q] == '\\' && q + 1 < n) { q++; continue; }
        if (plain[q] == '(') depth++;
        else if (plain[q] == ')') depth--;
      }
      out.push_back('(');
      std::vector<unsigned char> tmp(plain.begin() + p + 1, plain.begin() + std::min(q, n));
      Rc4One(objKey, tmp.data(), tmp.size());
      out.insert(out.end(), tmp.begin(), tmp.end());
      out.push_back(')');
      p = q;
      continue;
    }
    if (c == '<' && p + 1 < n && plain[p + 1] == '<') { out.push_back('<'); out.push_back('<'); p += 2; continue; }
    if (c == '>' && p + 1 < n && plain[p + 1] == '>') { out.push_back('>'); out.push_back('>'); p += 2; continue; }
    if (c == '<')
    {
      size_t q = p + 1;
      while (q < n && plain[q] != '>') q++;
      if (q >= n) { out.push_back('<'); p++; continue; }
      std::vector<unsigned char> bytes;
      int hi = -1;
      for (size_t k2 = p + 1; k2 < q; k2++)
      {
        const unsigned char h = plain[k2];
        int v = -1;
        if (h >= '0' && h <= '9') v = h - '0';
        else if (h >= 'A' && h <= 'F') v = h - 'A' + 10;
        else if (h >= 'a' && h <= 'f') v = h - 'a' + 10;
        if (v < 0) continue;
        if (hi < 0) hi = v;
        else { bytes.push_back((unsigned char)((hi << 4) | v)); hi = -1; }
      }
      if (hi >= 0) bytes.push_back((unsigned char)(hi << 4));
      Rc4One(objKey, bytes.data(), bytes.size());
      out.push_back('<');
      static const char* hx = "0123456789ABCDEF";
      for (unsigned char b : bytes) { out.push_back((unsigned char)hx[b >> 4]); out.push_back((unsigned char)hx[b & 15]); }
      out.push_back('>');
      p = q + 1;
      continue;
    }
    if (c == 's' && BytesAt(plain, p, "stream"))
    {
      // A real stream keyword is a bare token: preceded by EOL / '>>' / space
      // (so names like /FileStream are not matched), followed optionally by an
      // EOL whose presence is not required (pdfium sometimes emits the data
      // directly after the keyword).
      const bool prevOk = p == body ||
          plain[p - 1] == '\n' || plain[p - 1] == '\r' ||
          plain[p - 1] == '>' || plain[p - 1] == ' ';
      if (!prevOk) { out.push_back('s'); p++; continue; }
      size_t q = p + 6;
      if (q < n && (plain[q] == '\r' || plain[q] == '\n'))
      {
        if (plain[q] == '\r') q++;
        if (q < n && plain[q] == '\n') q++;
      }
      size_t es = SearchToken(plain, q, "endstream");
      if (es == (size_t)-1) { out.insert(out.end(), plain.begin() + p, plain.begin() + n); break; }
      out.insert(out.end(), plain.begin() + p, plain.begin() + q);
      std::vector<unsigned char> tmp(plain.begin() + q, plain.begin() + es);
      Rc4One(objKey, tmp.data(), tmp.size());
      out.insert(out.end(), tmp.begin(), tmp.end());
      const size_t esEnd = std::min(n, es + 9);
      out.insert(out.end(), plain.begin() + es, plain.begin() + esEnd);
      p = esEnd;
      continue;
    }
    out.push_back(c);
    p++;
  }
}

// Returns an empty vector on failure.
static std::vector<unsigned char> EncryptPdfBytes(
    const std::vector<unsigned char>& plain,
    const std::string& userPw, const std::string& ownerPw)
{
  std::vector<unsigned char> out;
  const size_t n = plain.size();
  if (n < 16) return out;

  // File ID + encryption keys.
  unsigned char id[16];
  RandomFileId(id);
  unsigned char fileKey[16], O[32], U[32];
  if (!BuildR3Keys(userPw, ownerPw, id, fileKey, O, U)) return out;

  size_t hdr = 0;
  while (hdr < n && plain[hdr] != '\n') hdr++;
  if (hdr >= n) return out;
  out.insert(out.end(), plain.begin(), plain.begin() + std::min(n, hdr + 1));

  // Collect object spans: "N G obj\n ... \nendobj".
  struct ObjSpan { size_t num; size_t gen; size_t body; size_t end; };
  std::vector<ObjSpan> objs;
  std::map<size_t, size_t> offByNum;
  size_t pos = hdr + 1;
  size_t maxNum = 0;
  while (pos < n)
  {
    while (pos < n && (plain[pos] == ' ' || plain[pos] == '\t' || plain[pos] == '\r' || plain[pos] == '\n')) pos++;
    if (pos < n && plain[pos] == '%') { while (pos < n && plain[pos] != '\n') pos++; continue; }
    if (pos >= n || plain[pos] < '0' || plain[pos] > '9') break;
    size_t num = 0, q = pos;
    while (q < n && plain[q] >= '0' && plain[q] <= '9') { num = num * 10 + (plain[q] - '0'); q++; }
    while (q < n && (plain[q] == ' ' || plain[q] == '\t' || plain[q] == '\r' || plain[q] == '\n')) q++;
    size_t gen = 0;
    while (q < n && plain[q] >= '0' && plain[q] <= '9') { gen = gen * 10 + (plain[q] - '0'); q++; }
    while (q < n && (plain[q] == ' ' || plain[q] == '\t' || plain[q] == '\r' || plain[q] == '\n')) q++;
    if (!BytesAt(plain, q, "obj")) break;
    size_t body = q + 3;
    while (body < n && plain[body] == '\r') body++;
    if (body < n && plain[body] == '\n') body++;
    size_t e = SearchToken(plain, body, "endobj");
    if (e == (size_t)-1) break;
    objs.push_back({ num, gen, body, e });
    if (num > maxNum) maxNum = num;
    pos = e + 6;
  }
  if (objs.empty()) return out;
  const size_t tail = pos;

  // Rebuild root/info object references from the original trailer.
  unsigned int rootNum = 1, infoNum = 0;
  if (!ParseRef(plain, tail, "/Root", &rootNum)) return out;
  ParseRef(plain, tail, "/Info", &infoNum);

  const size_t encNum = maxNum + 1;
  const size_t objCount = encNum + 1;  // objects 0..encNum inclusive

  for (size_t i = 0; i < objs.size(); i++)
  {
    const ObjSpan& s = objs[i];
    const size_t objNo = s.num, genNo = s.gen;
    offByNum[objNo] = out.size();
    char head[40];
    std::snprintf(head, sizeof(head), "%zu %zu obj\n", objNo, genNo);
    const char* hh = head;
    out.insert(out.end(), hh, hh + strlen(head));

    unsigned char mat[21];
    memcpy(mat, fileKey, 16);
    mat[16] = (unsigned char)(objNo & 0xff);
    mat[17] = (unsigned char)((objNo >> 8) & 0xff);
    mat[18] = (unsigned char)((objNo >> 16) & 0xff);
    mat[19] = (unsigned char)(genNo & 0xff);
    mat[20] = (unsigned char)((genNo >> 8) & 0xff);
    unsigned char objKey[16];
    Md5Once(mat, 21, objKey);  // truncated to min(16+5, 16) = 16 bytes

    EmitEncryptedBody(plain, s.body, s.end, objKey, out);
    static const char kEndObj[] = "endobj\n";
    out.insert(out.end(), kEndObj, kEndObj + sizeof(kEndObj) - 1);
  }

  // The /Encrypt dictionary itself is never encrypted.
  offByNum[encNum] = out.size();
  char encHead[32];
  std::snprintf(encHead, sizeof(encHead), "%zu 0 obj\n", encNum);
  const char* eh = encHead;
  out.insert(out.end(), eh, eh + strlen(encHead));

  auto hexRow = [](const unsigned char* b, size_t len) {
    static const char* hx = "0123456789ABCDEF";
    std::string s;
    for (size_t i = 0; i < len; i++) { s += hx[b[i] >> 4]; s += hx[b[i] & 15]; }
    return s;
  };
  std::string encDict = "<< /Filter /Standard /V 2 /R 3 /Length 128 "
                        "/O <" + hexRow(O, 32) + "> /U <" + hexRow(U, 32) + "> /P -4 >>\nendobj\n";
  out.insert(out.end(), encDict.begin(), encDict.end());

  // Fresh xref + trailer.
  const size_t xrefOff = out.size();
  std::string xref = "xref\n0 " + std::to_string(objCount) + "\n0000000000 65535 f \n";
  for (size_t i = 1; i <= encNum; i++)
  {
    auto it2 = offByNum.find(i);
    char line[32];
    if (it2 != offByNum.end())
      std::snprintf(line, sizeof(line), "%010zu 00000 n \n", it2->second);
    else
      std::snprintf(line, sizeof(line), "%010zu 00000 n \n", (size_t)0);
    xref += line;
  }
  std::string trailer = "trailer\n<< /Size " + std::to_string(objCount) +
                        " /Root " + std::to_string(rootNum) + " 0 R";
  if (infoNum) trailer += " /Info " + std::to_string(infoNum) + " 0 R";
  trailer += " /Encrypt " + std::to_string(encNum) + " 0 R /ID [<" +
             hexRow(id, 16) + "> <" + hexRow(id, 16) + ">] >>\n";
  out.insert(out.end(), xref.begin(), xref.end());
  out.insert(out.end(), trailer.begin(), trailer.end());
  std::string tailText = "startxref\n" + std::to_string(xrefOff) + "\n%%EOF\n";
  out.insert(out.end(), tailText.begin(), tailText.end());
  return out;
}

static bool SaveAsString(FPDF_DOCUMENT d, std::vector<unsigned char>& out)
{
  FileWriter fw{};
  fw.base.version = 1;
  fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
  {
    FileWriter* f = reinterpret_cast<FileWriter*>(self);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    f->buf.insert(f->buf.end(), p, p + size);
    return 1;
  };
  bool ok = FPDF_SaveAsCopy(d, &fw.base, FPDF_NO_INCREMENTAL);
  out.swap(fw.buf);
  return ok;
}

static void SelfTest(const std::wstring& cwd)
{
  std::vector<std::string> lines;
  int pass = 0, fail = 0;
  std::wstring outPath = cwd + L"\\test_result.txt";
  FILE* of = nullptr;
  _wfopen_s(&of, outPath.c_str(), L"w");
  auto emit = [&](const std::string& l) {
    lines.push_back(l);
    if (of)
    {
      fwrite(l.data(), 1, l.size(), of);
      fputc('\n', of);
      fflush(of);
    }
  };
  auto check = [&](const std::string& name, bool ok) {
    emit(std::string(ok ? "PASS " : "FAIL ") + name);
    if (ok) ++pass;
    else ++fail;
  };
  auto checkEq = [&](const std::string& name, int got, int want) {
    check(name + " (got " + std::to_string(got) +
          ", want " + std::to_string(want) + ")", got == want);
  };

  FPDF_InitLibrary();
  check("library init", true);

  {
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    check("create new doc", d != nullptr);
    check("add blank page", FPDFPage_New(d, 0, 612.0, 792.0) != nullptr);
    std::vector<unsigned char> bytes;
    check("save new doc", SaveAsString(d, bytes));
check("saved %PDF header", bytes.size() > 8 &&
            std::string(bytes.begin(), bytes.begin() + 5) == "%PDF-");
    check("saved %%EOF trailer", bytes.size() >= 6 &&
            std::string(bytes.end() - std::min<size_t>(16, bytes.size()),
                        bytes.end())
                  .find("%%EOF") != std::string::npos);
    FPDF_DOCUMENT r = FPDF_LoadMemDocument(bytes.data(), (int)bytes.size(), nullptr);
    check("roundtrip reopen", r != nullptr);
    if (r) { checkEq("reopened has 1 page", FPDF_GetPageCount(r), 1); }
    if (r) FPDF_CloseDocument(r);
    FPDF_CloseDocument(d);
  }

  std::string sample = MakeSamplePdf();
  {
    check("sample pdf nonempty", sample.size() > 300);
  }
  FPDF_DOCUMENT s = nullptr;
  {
    s = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
    check("load sample pdf", s != nullptr);
    if (s)
    {
      checkEq("sample page count", FPDF_GetPageCount(s), 1);
      FPDF_PAGE p = FPDF_LoadPage(s, 0);
      check("load page", p != nullptr);
      if (p)
      {
        float w = FPDF_GetPageWidthF(p);
        float h = FPDF_GetPageHeightF(p);
        check("page size 612x792", std::abs(w - 612.0f) < 0.5f &&
                                   std::abs(h - 792.0f) < 0.5f);
        FPDF_ClosePage(p);
        int bw = 200, bh = 260, stride = ((bw * 32 + 31) / 32) * 4;
        std::vector<unsigned char> px((size_t)bh * stride, 0xFF);
        FPDF_BITMAP fb = FPDFBitmap_CreateEx(bw, bh, FPDFBitmap_BGRA,
                                             px.data(), stride);
        FPDFBitmap_FillRect(fb, 0, 0, bw, bh, 0xFFFFFFFF);
        FPDF_PAGE pp = FPDF_LoadPage(s, 0);
        FPDF_RenderPageBitmap(fb, pp, 0, 0, bw, bh, 0, FPDF_ANNOT);
        FPDF_ClosePage(pp);
        FPDFBitmap_Destroy(fb);
        long long ink = 0;
        for (int yy = 0; yy < bh; ++yy)
        {
          const unsigned char* row = px.data() + (size_t)yy * stride;
          for (int xx = 0; xx < bw; ++xx)
          {
            unsigned char b = row[xx * 4], gg = row[xx * 4 + 1],
                          rr = row[xx * 4 + 2];
            if (b < 250 || gg < 250 || rr < 250) ++ink;
          }
        }
        emit("render non-white pixels: " + std::to_string(ink));
        check("render produces ink", ink > 30);
      }

      std::vector<unsigned char> saved;
      check("save-as-copy sample", SaveAsString(s, saved));
      check("copy valid header", saved.size() > 200 &&
            std::string(saved.begin(), saved.begin() + 5) == "%PDF-");

      FPDF_PAGE p0 = FPDF_LoadPage(s, 0);
      int rot0 = p0 ? FPDFPage_GetRotation(p0) : -1;
      if (p0) FPDF_ClosePage(p0);
      FPDF_PAGE p1 = FPDF_LoadPage(s, 0);
      if (p1)
      {
        FPDFPage_SetRotation(p1, (rot0 + 1) & 3);
        FPDF_ClosePage(p1);
      }
      FPDF_PAGE p2 = FPDF_LoadPage(s, 0);
      check("rotate changes width/height", p2 != nullptr &&
            std::abs(FPDF_GetPageWidthF(p2) - 792.0f) < 0.5f &&
            std::abs(FPDF_GetPageHeightF(p2) - 612.0f) < 0.5f);
      if (p2) FPDF_ClosePage(p2);

      FPDF_DOCUMENT merged = FPDF_CreateNewDocument();
      check("merge import", FPDF_ImportPagesByIndex(merged, s, nullptr, 0, 0));
      checkEq("import page count", FPDF_GetPageCount(merged), 1);
      FPDF_CloseDocument(merged);

      FPDFPage_Delete(s, 0);
      checkEq("delete page -> 0", FPDF_GetPageCount(s), 0);
    }
  }

  {
    std::string outline = MakeOutlinePdf();
    FPDF_DOCUMENT od = FPDF_LoadMemDocument(outline.data(), (int)outline.size(), nullptr);
    check("load outline pdf", od != nullptr);
    FPDF_BOOKMARK b0 = od ? FPDFBookmark_GetFirstChild(od, nullptr) : nullptr;
    check("outline root present", b0 != nullptr);
    if (b0)
    {
      FPDF_BOOKMARK b1 = FPDFBookmark_GetNextSibling(od, b0);
      FPDF_BOOKMARK b2 = b1 ? FPDFBookmark_GetFirstChild(od, b1) : nullptr;
      check("outline top-level sibling", b1 != nullptr);
      check("outline nested child present", b2 != nullptr);
      checkEq("outline children of Details", b2 ? FPDFBookmark_GetCount(b1) : -1, 1);

      auto titleOf = [&](FPDF_BOOKMARK bm) -> std::wstring {
        unsigned long n = FPDFBookmark_GetTitle(bm, nullptr, 0);
        std::vector<unsigned char> raw(n + 2, 0);
        FPDFBookmark_GetTitle(bm, raw.data(), (unsigned long)raw.size());
        int c = (int)(n / 2) - 1;
        if (c < 0) c = 0;
        return std::wstring(reinterpret_cast<const wchar_t*>(raw.data()), (size_t)c);
      };
      check("outline title Cover", titleOf(b0) == L"Cover");
      check("outline title Details", titleOf(b1) == L"Details");
      check("outline title nested Sub", titleOf(b2) == L"Details - Sub");

      FPDF_DEST d0 = FPDFBookmark_GetDest(od, b0);
      FPDF_DEST d1 = b1 ? FPDFBookmark_GetDest(od, b1) : nullptr;
      checkEq("bookmark dest Cover page", d0 ? FPDFDest_GetDestPageIndex(od, d0) : -1, 0);
      checkEq("bookmark dest Details page", d1 ? FPDFDest_GetDestPageIndex(od, d1) : -1, 1);
    }
    if (od) FPDF_CloseDocument(od);
  }

  {
    FPDF_DOCUMENT an = FPDF_CreateNewDocument();
    FPDFPage_New(an, 0, 612.0, 792.0);
    check("annot: highlight created", InsertAnnot(an, 0, ID_ANN_HL));
    check("annot: underline created", InsertAnnot(an, 0, ID_ANN_UL));
    check("annot: note created", InsertAnnot(an, 0, ID_ANN_NOTE));
    check("annot: textbox created", InsertAnnot(an, 0, ID_ANN_TEXT));
    check("annot: shape created", InsertAnnot(an, 0, ID_ANN_SHAPE));
    check("annot: stamp created", InsertAnnot(an, 0, ID_ANN_STAMP));

    FPDF_PAGE ap9 = FPDF_LoadPage(an, 0);
    check("annot: page with annots loads", ap9 != nullptr);
    if (ap9) FPDF_ClosePage(ap9);

    std::vector<unsigned char> abytes;
    check("annot: save annotated doc", SaveAsString(an, abytes));
    FPDF_DOCUMENT an2 = FPDF_LoadMemDocument(abytes.data(), (int)abytes.size(), nullptr);
    check("annot: reload annotated doc", an2 != nullptr);

    if (an2)
    {
      FPDF_PAGE ap = FPDF_LoadPage(an2, 0);
      check("annot: reopened page loads", ap != nullptr);
      if (ap)
      {
        int n = FPDFPage_GetAnnotCount(ap);
        checkEq("annot: persistent count", n, 6);
        bool foundHL = false, foundUL = false, foundNote = false, foundFree = false,
             foundSq = false, foundSt = false;
        FS_RECTF noteR{};
        for (int i = 0; i < n && i < 16; ++i)
        {
          FPDF_ANNOTATION aa = FPDFPage_GetAnnot(ap, i);
          if (!aa) continue;
          int st = FPDFAnnot_GetSubtype(aa);
          foundHL |= (st == FPDF_ANNOT_HIGHLIGHT);
          foundUL |= (st == FPDF_ANNOT_UNDERLINE);
          foundNote |= (st == FPDF_ANNOT_TEXT);
          foundFree |= (st == FPDF_ANNOT_FREETEXT);
          foundSq |= (st == FPDF_ANNOT_SQUARE);
          foundSt |= (st == FPDF_ANNOT_STAMP);
          if (st == FPDF_ANNOT_TEXT) FPDFAnnot_GetRect(aa, &noteR);
          FPDFPage_CloseAnnot(aa);
        }
        check("annot: highlight persists", foundHL);
        check("annot: underline persists", foundUL);
        check("annot: note persists", foundNote);
        check("annot: textbox persists", foundFree);
        check("annot: shape persists", foundSq);
        check("annot: stamp persists", foundSt);
        check("annot: note rect sane", noteR.left > 0.0f && noteR.right > noteR.left &&
                                        noteR.top > noteR.bottom);

        auto annotInk = [&](int flags) -> long long {
          int bw = 220, bh = 285, stride = ((bw * 32 + 31) / 32) * 4;
          std::vector<unsigned char> px((size_t)bh * stride, 0xFF);
          FPDF_BITMAP fb = FPDFBitmap_CreateEx(bw, bh, FPDFBitmap_BGRA, px.data(), stride);
          FPDFBitmap_FillRect(fb, 0, 0, bw, bh, 0xFFFFFFFF);
          FPDF_RenderPageBitmap(fb, ap, 0, 0, bw, bh, 0, flags);
          FPDFBitmap_Destroy(fb);
          long long sum = 0;
          for (int yy = 0; yy < bh; yy += 2)
          {
            const unsigned char* row = px.data() + (size_t)yy * stride;
            for (int xx = 0; xx < bw; xx += 2)
            {
              unsigned char b = row[xx * 4], gg = row[xx * 4 + 1], rr = row[xx * 4 + 2];
              if (b < 250 || gg < 250 || rr < 250) ++sum;
            }
          }
          return sum;
        };
        long long plain = annotInk(0);
        long long shown = annotInk(FPDF_ANNOT);
        emit("annot render ink: plain=" + std::to_string(plain) +
             " annot=" + std::to_string(shown));
        check("annot: appearances render", shown > plain);
        FPDF_ClosePage(ap);
      }
      FPDF_CloseDocument(an2);
    }
    FPDF_CloseDocument(an);
  }

  {
    wchar_t tmpPath[MAX_PATH];
    GetTempPathW(MAX_PATH, tmpPath);
    std::wstring file = std::wstring(tmpPath) + L"stitchup_selftest.pdf";
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    FPDFPage_New(d, 0, 612.0, 792.0);
    FileWriter fw{};
    fw.base.version = 1;
    fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
    {
      FileWriter* f = reinterpret_cast<FileWriter*>(self);
      const unsigned char* p = static_cast<const unsigned char*>(data);
      f->buf.insert(f->buf.end(), p, p + size);
      return 1;
    };
    bool saved = FPDF_SaveAsCopy(d, &fw.base, FPDF_NO_INCREMENTAL);
    FILE* f = nullptr;
    if (saved && _wfopen_s(&f, file.c_str(), L"wb") == 0)
    {
      fwrite(fw.buf.data(), 1, fw.buf.size(), f);
      fclose(f);
    }
    FPDF_CloseDocument(d);
    FPDF_DOCUMENT r = FPDF_LoadDocument(Utf8(file).c_str(), nullptr);
    check("file save + reopen", r != nullptr);
    if (r) { checkEq("file reopen page count", FPDF_GetPageCount(r), 1); }
    if (r) FPDF_CloseDocument(r);
    DeleteFileW(file.c_str());
  }

  {
    std::string lpdf = MakeLinkedPdf();
    FPDF_DOCUMENT ld = FPDF_LoadMemDocument(lpdf.data(), (int)lpdf.size(), nullptr);
    check("load linked pdf", ld != nullptr);
    if (ld)
    {
      checkEq("linked pdf page count", FPDF_GetPageCount(ld), 2);
      FPDF_PAGE lp = FPDF_LoadPage(ld, 0);
      check("load page 0 (links)", lp != nullptr);
      if (lp)
      {
        check("link hit center (200,600)", FPDFLink_GetLinkAtPoint(lp, 200, 600) != nullptr);
        check("link hit near corner (150,550)",
              FPDFLink_GetLinkAtPoint(lp, 150, 550) != nullptr);
        check("no link outside rect (x)", FPDFLink_GetLinkAtPoint(lp, 50, 600) == nullptr);
        check("no link outside rect (y)", FPDFLink_GetLinkAtPoint(lp, 200, 50) == nullptr);
        check("uri link hit (460,250)", FPDFLink_GetLinkAtPoint(lp, 460, 250) != nullptr);

        int pos = 0;
        FPDF_LINK tmp = nullptr;
        int linked = 0;
        while (FPDFLink_Enumerate(lp, &pos, &tmp)) ++linked;
        checkEq("enumerate link count", linked, 2);

        FPDF_LINK dl = FPDFLink_GetLinkAtPoint(lp, 200, 600);
        FPDF_DEST dest = dl ? FPDFLink_GetDest(ld, dl) : nullptr;
        check("internal link has dest", dest != nullptr);
        checkEq("internal dest page", dest ? FPDFDest_GetDestPageIndex(ld, dest) : -1, 1);

        FPDF_LINK ul = FPDFLink_GetLinkAtPoint(lp, 460, 250);
        FPDF_ACTION act = ul ? FPDFLink_GetAction(ul) : nullptr;
        check("uri link has action", act != nullptr);
        std::string uri;
        if (act)
        {
          unsigned long nb = FPDFAction_GetURIPath(ld, act, nullptr, 0);
          if (nb > 0)
          {
            uri.assign((size_t)(nb - 1), '\0');
            FPDFAction_GetURIPath(ld, act, &uri[0], nb);
          }
          checkEq("uri path bytes", (int)nb - 1,
                  (int)std::string("https://example.com/stitchup").size());
        }
        check("uri path value", uri == "https://example.com/stitchup");
        FPDF_ClosePage(lp);
      }
      FPDF_CloseDocument(ld);
    }
  }

  {
    // page management: extract / split / crop
    FPDF_DOCUMENT d = FPDF_CreateNewDocument();
    check("pm: create doc", d != nullptr);
    if (d)
    {
      check("pm: add page 0", FPDFPage_New(d, 0, 612.0, 792.0) != nullptr);
      check("pm: add page 1", FPDFPage_New(d, 1, 700.0, 500.0) != nullptr);
      check("pm: add page 2", FPDFPage_New(d, 2, 612.0, 792.0) != nullptr);
      checkEq("pm: 3 pages", FPDF_GetPageCount(d), 3);

      std::wstring tmpPath;
      {
        wchar_t tp[MAX_PATH];
        GetTempPathW(MAX_PATH, tp);
        tmpPath = tp;
      }
      std::wstring pmFile = tmpPath + L"stitchup_pm_test.pdf";
      check("pm: DocToFile", DocToFile(d, pmFile));
      FPDF_DOCUMENT rl = FPDF_LoadDocument(Utf8(pmFile).c_str(), nullptr);
      check("pm: DocToFile reopen", rl != nullptr);
      if (rl) { checkEq("pm: reopen count", FPDF_GetPageCount(rl), 3); }
      if (rl) FPDF_CloseDocument(rl);
      DeleteFileW(pmFile.c_str());

      FPDF_DOCUMENT e = FPDF_CreateNewDocument();
      int one = 1;
      check("pm: import page index 1",
            e && FPDF_ImportPagesByIndex(e, d, &one, 1, 0) != 0);
      if (e)
      {
        checkEq("pm: extract count", FPDF_GetPageCount(e), 1);
        FPDF_PAGE ep = FPDF_LoadPage(e, 0);
        check("pm: extract page loads", ep != nullptr);
        if (ep)
        {
          float L, B, R2, T;
          FPDFPage_GetMediaBox(ep, &L, &B, &R2, &T);
          check("pm: extracted page keeps size 700x500",
                std::abs(L) < 0.1f && std::abs(B) < 0.1f &&
                std::abs(R2 - 700.0f) < 0.1f && std::abs(T - 500.0f) < 0.1f);
          FPDF_ClosePage(ep);
        }
      }
      if (e) FPDF_CloseDocument(e);

      bool splitOk = true;
      for (int i = 0; i < 3; ++i)
      {
        int idx = i;
        FPDF_DOCUMENT sp = FPDF_CreateNewDocument();
        if (!sp || !FPDF_ImportPagesByIndex(sp, d, &idx, 1, 0))
          splitOk = false;
        else if (FPDF_GetPageCount(sp) != 1) splitOk = false;
        if (sp) FPDF_CloseDocument(sp);
      }
      check("pm: split produces one page per file", splitOk);

      FPDF_DOCUMENT sd = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
      check("pm: crop load sample", sd != nullptr);
      if (sd)
      {
        FPDF_PAGE sp2 = FPDF_LoadPage(sd, 0);
        check("pm: crop load page", sp2 != nullptr);
        if (sp2)
        {
          float cL, cB, cR, cT;
          bool ib = InkBounds(sp2, cL, cB, cR, cT);
          check("pm: content bounds found", ib);
          if (ib)
          {
            emit("pm: ink bounds L=" + std::to_string((double)cL) +
                 " B=" + std::to_string((double)cB) +
                 " R=" + std::to_string((double)cR) +
                 " T=" + std::to_string((double)cT));
            FPDFPage_SetMediaBox(sp2, cL, cB, cR, cT);
            float mL, mB, mR, mT;
            FPDFPage_GetMediaBox(sp2, &mL, &mB, &mR, &mT);
            check("pm: crop left tight (71..73)", mL >= 71.0f && mL <= 73.0f);
            check("pm: crop bottom to ink (669..674)", mB >= 669.0f && mB <= 674.0f);
            check("pm: crop right to text (282..287)", mR >= 282.0f && mR <= 287.0f);
            check("pm: crop top trimmed (730..744)", mT >= 730.0f && mT <= 744.0f);
          }
          FPDF_ClosePage(sp2);
        }
        FPDF_CloseDocument(sd);
      }
      FPDF_CloseDocument(d);
    }
  }

  {
    // text export
    std::string tpdf = MakeTextPdf();
    FPDF_DOCUMENT td = FPDF_LoadMemDocument(tpdf.data(), (int)tpdf.size(), nullptr);
    check("tx: load text pdf", td != nullptr);
    if (td)
    {
      checkEq("tx: text page count", FPDF_GetPageCount(td), 1);
      std::wstring raw = DocTextRaw(td);
      check("tx: raw text contains run",
            raw.find(L"Hello, World! Export me.") != std::wstring::npos);
      wchar_t tp2[MAX_PATH];
      GetTempPathW(MAX_PATH, tp2);
      std::wstring f = std::wstring(tp2) + L"stitchup_export_test.txt";
      check("tx: export to file", ExportTextToFile(td, f));
      bool bom = false, text = false;
      FILE* fx = nullptr;
      if (_wfopen_s(&fx, f.c_str(), L"rb") == 0 && fx)
      {
        unsigned char hdr[3] = {0, 0, 0};
        if (fread(hdr, 1, 3, fx) == 3)
          bom = hdr[0] == 0xEF && hdr[1] == 0xBB && hdr[2] == 0xBF;
        std::string rest;
        int c;
        while ((c = fgetc(fx)) != EOF) rest.push_back((char)c);
        text = rest.find("Hello, World! Export me.") != std::string::npos;
        fclose(fx);
      }
      check("tx: UTF-8 BOM written", bom);
      check("tx: exported text present", text);
      DeleteFileW(f.c_str());
      FPDF_CloseDocument(td);
    }
    FPDF_DOCUMENT blank = FPDF_CreateNewDocument();
    if (blank)
    {
      FPDF_PAGE bp = FPDFPage_New(blank, 0, 612.0, 792.0);
      if (bp) FPDF_ClosePage(bp);
      wchar_t tp3[MAX_PATH];
      GetTempPathW(MAX_PATH, tp3);
      std::wstring bf = std::wstring(tp3) + L"stitchup_export_blank.txt";
      check("tx: blank doc export is false", !ExportTextToFile(blank, bf));
      check("tx: blank doc writes no file", GetFileAttributesW(bf.c_str()) == INVALID_FILE_ATTRIBUTES);
      FPDF_CloseDocument(blank);
    }
  }

  {
    // CSV export
    std::string cpdf = MakeTextPdf();
    FPDF_DOCUMENT cd = FPDF_LoadMemDocument(cpdf.data(), (int)cpdf.size(), nullptr);
    check("csv: load text pdf", cd != nullptr);
    if (cd)
    {
      wchar_t tp5[MAX_PATH];
      GetTempPathW(MAX_PATH, tp5);
      std::wstring cf = std::wstring(tp5) + L"stitchup_export_test.csv";
      check("csv: export to file", ExportCsvToFile(cd, cf));
      bool cBom = false;
      bool cHdr = false, cRow = false;
      FILE* fc = nullptr;
      if (_wfopen_s(&fc, cf.c_str(), L"rb") == 0 && fc)
      {
        unsigned char hdr[3] = {0, 0, 0};
        if (fread(hdr, 1, 3, fc) == 3)
          cBom = hdr[0] == 0xEF && hdr[1] == 0xBB && hdr[2] == 0xBF;
        std::string all;
        int c;
        while ((c = fgetc(fc)) != EOF) all.push_back((char)c);
        cHdr = all.find("Page,Width (pt),Height (pt),Text chars,Annotations") != std::string::npos;
        cRow = all.find("1,612.0,792.0,25,0") != std::string::npos;
        fclose(fc);
      }
      check("csv: UTF-8 BOM written", cBom);
      check("csv: header line present", cHdr);
      check("csv: per-page row (page,size,text,anns)", cRow);
      DeleteFileW(cf.c_str());
      FPDF_CloseDocument(cd);
    }
    {
      // two-page blank doc: one row per page; empty doc: no file
      FPDF_DOCUMENT two = FPDF_CreateNewDocument();
      check("csv: create 2-page doc", two != nullptr);
      if (two)
      {
        FPDF_PAGE p0 = FPDFPage_New(two, 0, 612.0, 792.0);
        if (p0) FPDF_ClosePage(p0);
        FPDF_PAGE p1 = FPDFPage_New(two, 1, 700.0, 500.0);
        if (p1) FPDF_ClosePage(p1);
        std::wstring csv = DocCsv(two);
        check("csv: one row per page", csv.find(L"1,612.0,792.0,0,0") != std::wstring::npos &&
                                      csv.find(L"2,700.0,500.0,0,0") != std::wstring::npos);
        FPDF_CloseDocument(two);
      }
      FPDF_DOCUMENT zero = FPDF_CreateNewDocument();
      wchar_t tp6[MAX_PATH];
      GetTempPathW(MAX_PATH, tp6);
      std::wstring zf = std::wstring(tp6) + L"stitchup_export_empty.csv";
      if (zero)
      {
        check("csv: no pages -> export false", !ExportCsvToFile(zero, zf));
        check("csv: no pages -> no file",
              GetFileAttributesW(zf.c_str()) == INVALID_FILE_ATTRIBUTES);
        FPDF_CloseDocument(zero);
      }
      {
        FPDF_DOCUMENT ann = FPDF_CreateNewDocument();
        check("csv: create annot doc", ann != nullptr);
        if (ann)
        {
          FPDF_PAGE ap2 = FPDFPage_New(ann, 0, 612.0, 792.0);
          if (ap2)
          {
            FPDF_ANNOTATION ha = FPDFPage_CreateAnnot(ap2, FPDF_ANNOT_HIGHLIGHT);
            check("csv: create annot for count", ha != nullptr);
            if (ha) FPDFPage_CloseAnnot(ha);
            std::wstring csvA = DocCsv(ann);
            check("csv: annotation count in row",
                  csvA.find(L"1,612.0,792.0,0,1") != std::wstring::npos);
            FPDF_ClosePage(ap2);
          }
          FPDF_CloseDocument(ann);
        }
      }
    }
  }

  {
    // encrypted PDF (Standard V1/R2, user "stitchup")
    unsigned char md5abc[16];
    { Md5Ctx m; Md5Init(&m); Md5Update(&m, (const unsigned char*)"abc", 3); Md5Final(&m, md5abc); }
    const char* hx = "0123456789abcdef";
    std::string md5s;
    for (int i = 0; i < 16; i++) { md5s += hx[md5abc[i] >> 4]; md5s += hx[md5abc[i] & 15]; }
    check("crypto: MD5(abc) vector", md5s == "900150983cd24fb0d6963f7d28e17f72");
    unsigned char rc4out[10];
    { Rc4Ctx r; Rc4Init(&r, (const unsigned char*)"Key", 3);
      Rc4Crypt(&r, (const unsigned char*)"Plaintext", rc4out, 9); }
    std::string rc4s;
    for (int i = 0; i < 9; i++) { rc4s += hx[rc4out[i] >> 4]; rc4s += hx[rc4out[i] & 15]; }
    check("crypto: RC4 vector", rc4s == "bbf316e8d940af0ad3");

    std::string epdf;
    int accepted = 0;
    for (int v = 1; v <= 3; v++)
    {
      std::string e = MakeEncryptedPdf(v);
      FPDF_DOCUMENT p = FPDF_LoadMemDocument(e.data(), (int)e.size(), "stitchup");
      if (p)
      {
        if (accepted == 0) epdf = e;
        ++accepted;
        emit("enc: variant " + std::to_string(v) + " accepted");
        FPDF_CloseDocument(p);
      }
    }
    check("enc: at least one variant accepted", accepted > 0);
    if (epdf.empty()) epdf = MakeEncryptedPdf(1);
    FPDF_DOCUMENT ok = FPDF_LoadMemDocument(epdf.data(), (int)epdf.size(), "stitchup");
    check("enc: opens with correct password", ok != nullptr);
    if (ok)
    {
      checkEq("enc: page count", FPDF_GetPageCount(ok), 1);
      check("enc: stream decrypted (text run)",
            DocTextRaw(ok).find(L"Hello, World! Export me.") != std::wstring::npos);
      FPDF_CloseDocument(ok);
    }
    FPDF_DOCUMENT bad = FPDF_LoadMemDocument(epdf.data(), (int)epdf.size(), "wrongpass");
    check("enc: wrong password rejected", bad == nullptr);
    check("enc: error is PASSWORD",
          bad == nullptr && FPDF_GetLastError() == FPDF_ERR_PASSWORD);
    if (bad) FPDF_CloseDocument(bad);
    FPDF_DOCUMENT none = FPDF_LoadMemDocument(epdf.data(), (int)epdf.size(), nullptr);
    check("enc: empty password rejected", none == nullptr);
    check("enc: no-password error is PASSWORD",
          none == nullptr && FPDF_GetLastError() == FPDF_ERR_PASSWORD);
    if (none) FPDF_CloseDocument(none);
    wchar_t tp4[MAX_PATH];
    GetTempPathW(MAX_PATH, tp4);
    std::wstring ef = std::wstring(tp4) + L"stitchup_enc_test.pdf";
    FILE* fe = nullptr;
    if (_wfopen_s(&fe, ef.c_str(), L"wb") == 0 && fe)
    {
      fwrite(epdf.data(), 1, epdf.size(), fe);
      fclose(fe);
    }
    check("enc: fixture written", GetFileAttributesW(ef.c_str()) != INVALID_FILE_ATTRIBUTES);
    {
      FPDF_DOCUMENT disk = FPDF_LoadDocument(Utf8(ef).c_str(), "stitchup");
      check("enc: disk fixture opens with correct password", disk != nullptr);
      if (disk) FPDF_CloseDocument(disk);
    }
  }

  {
    // Encrypt-on-save writer: a pdfium-serialized document is encrypted and
    // must reopen with the user password (proves per-object RC4 keys, the
    // chained U entry and the rebuilt xref/trailer).
    FPDF_DOCUMENT esrc = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
    check("epw: source loads", esrc != nullptr);
    std::vector<unsigned char> eplain;
    bool eser = esrc && SaveAsString(esrc, eplain);
    check("epw: source serializes", eser);
    if (esrc) FPDF_CloseDocument(esrc);
    std::vector<unsigned char> eenc = EncryptPdfBytes(eplain, "s3cret", "ownerpw");
    check("epw: writer produced output", !eenc.empty());
    if (!eplain.empty() && !eenc.empty())
      check("epw: bytes differ from plaintext", eplain != eenc);
    const std::string estr(eenc.begin(), eenc.end());
    check("epw: /Encrypt in output", estr.find("/Encrypt") != std::string::npos);
    check("epw: /Filter /Standard present",
          estr.find("/Filter /Standard") != std::string::npos);
    check("epw: V2 R3 128 present",
          estr.find("/V 2 /R 3 /Length 128") != std::string::npos);
    check("epw: user password not stored in clear",
          estr.find("s3cret") == std::string::npos);
    FPDF_DOCUMENT eok = FPDF_LoadMemDocument(eenc.data(), (int)eenc.size(), "s3cret");
    check("epw: opens with correct password", eok != nullptr);
    if (eok)
    {
      checkEq("epw: page count", FPDF_GetPageCount(eok), 1);
      check("epw: stream decrypted (text run)",
            DocTextRaw(eok).find(L"Stitchup PDF Editor") != std::wstring::npos);
      FPDF_CloseDocument(eok);
    }
    FPDF_DOCUMENT ebad = FPDF_LoadMemDocument(eenc.data(), (int)eenc.size(), "wrongpass");
    check("epw: wrong password rejected", ebad == nullptr);
    check("epw: wrong password is PASSWORD error",
          ebad == nullptr && FPDF_GetLastError() == FPDF_ERR_PASSWORD);
    if (ebad) FPDF_CloseDocument(ebad);
    FPDF_DOCUMENT enone = FPDF_LoadMemDocument(eenc.data(), (int)eenc.size(), nullptr);
    check("epw: empty password rejected", enone == nullptr);
    if (enone) FPDF_CloseDocument(enone);
    wchar_t tp5[MAX_PATH];
    GetTempPathW(MAX_PATH, tp5);
    std::wstring epwf = std::wstring(tp5) + L"stitchup_encwrite_test.pdf";
    FILE* fe2 = nullptr;
    bool ewrote = false;
    if (_wfopen_s(&fe2, epwf.c_str(), L"wb") == 0 && fe2)
    {
      ewrote = fwrite(eenc.data(), 1, eenc.size(), fe2) == eenc.size();
      fclose(fe2);
    }
    check("epw: disk fixture written",
          ewrote && GetFileAttributesW(epwf.c_str()) != INVALID_FILE_ATTRIBUTES);
    if (ewrote)
    {
      FPDF_DOCUMENT edge = FPDF_LoadDocument(Utf8(epwf).c_str(), "s3cret");
      check("epw: disk fixture opens with correct password", edge != nullptr);
      if (edge) FPDF_CloseDocument(edge);
    }
  }

  {
    FPDF_DOCUMENT wd = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
    check("wat: sample loads", wd != nullptr);
    if (wd)
    {
      check("wat: empty text rejected",
            !ApplyWatermarkDoc(wd, L"", 36.0f, WMDP_CENTER));
      check("wat: center watermark applies",
            ApplyWatermarkDoc(wd, L"TRIAL", 36.0f, WMDP_CENTER));
      checkEq("wat: page count unchanged", FPDF_GetPageCount(wd), 1);
      FPDF_PAGE wp = FPDF_LoadPage(wd, 0);
      if (wp)
      {
        std::wstring wt = PageTextRaw(wp);
        check("wat: original text preserved",
              wt.find(L"Stitchup PDF Editor") != std::wstring::npos);
        check("wat: watermark text extractable",
              wt.find(L"TRIAL") != std::wstring::npos);
        FPDF_ClosePage(wp);
      }
      std::vector<unsigned char> wbytes;
      check("wat: watermarked doc saves",
            SaveAsString(wd, wbytes) && wbytes.size() > 8);
      FPDF_DOCUMENT wr = wbytes.empty() ? nullptr
        : FPDF_LoadMemDocument(wbytes.data(), (int)wbytes.size(), nullptr);
      check("wat: watermarked doc roundtrips", wr != nullptr);
      if (wr)
      {
        checkEq("wat: roundtrip page count", FPDF_GetPageCount(wr), 1);
        FPDF_PAGE rp = FPDF_LoadPage(wr, 0);
        if (rp)
        {
          check("wat: roundtrip keeps watermark",
                PageTextRaw(rp).find(L"TRIAL") != std::wstring::npos);
          FPDF_ClosePage(rp);
        }
        FPDF_CloseDocument(wr);
      }
      FPDF_CloseDocument(wd);
    }
  }
  {
    FPDF_DOCUMENT wd = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
    check("wat: top mode applies", wd &&
          ApplyWatermarkDoc(wd, L"TRIAL", 24.0f, WMDP_TOP));
    if (wd)
    {
      FPDF_PAGE wp = FPDF_LoadPage(wd, 0);
      if (wp)
      {
        check("wat: top mode extractable",
              PageTextRaw(wp).find(L"TRIAL") != std::wstring::npos);
        FPDF_ClosePage(wp);
      }
      FPDF_CloseDocument(wd);
    }
  }
  {
    FPDF_DOCUMENT wd = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
    check("wat: tiled mode applies", wd &&
          ApplyWatermarkDoc(wd, L"TRIAL", 16.0f, WMDP_TILED));
    if (wd)
    {
      FPDF_PAGE wp = FPDF_LoadPage(wd, 0);
      if (wp)
      {
        check("wat: tiled mode extractable",
              PageTextRaw(wp).find(L"TRIAL") != std::wstring::npos);
        FPDF_ClosePage(wp);
      }
      FPDF_CloseDocument(wd);
    }
  }
  {
    std::string outline = MakeOutlinePdf();
    FPDF_DOCUMENT wd = FPDF_LoadMemDocument(outline.data(), (int)outline.size(), nullptr);
    check("wat: multipage sample loads", wd != nullptr);
    if (wd)
    {
      checkEq("wat: multipage count", FPDF_GetPageCount(wd), 2);
      check("wat: multipage watermark applies",
            ApplyWatermarkDoc(wd, L"TRIAL", 36.0f, WMDP_CENTER));
      bool both = true;
      for (int i = 0; i < FPDF_GetPageCount(wd); ++i)
      {
        FPDF_PAGE wp = FPDF_LoadPage(wd, i);
        if (!wp) { both = false; continue; }
        if (PageTextRaw(wp).find(L"TRIAL") == std::wstring::npos) both = false;
        FPDF_ClosePage(wp);
      }
      check("wat: watermark on all pages", both);
      FPDF_CloseDocument(wd);
    }
  }
  {
    // disk-loaded document, mirroring the GUI path (file -> apply -> save)
    wchar_t dwt[MAX_PATH];
    GetTempPathW(MAX_PATH, dwt);
    std::wstring dp = std::wstring(dwt) + L"stitchup_wm_disk.pdf";
    FILE* dwf = nullptr;
    if (_wfopen_s(&dwf, dp.c_str(), L"wb") == 0 && dwf)
    {
      fwrite(sample.data(), 1, sample.size(), dwf);
      fclose(dwf);
    }
    check("wat: disk fixture written",
          GetFileAttributesW(dp.c_str()) != INVALID_FILE_ATTRIBUTES);
    FPDF_DOCUMENT dd = FPDF_LoadDocument(Utf8(dp).c_str(), nullptr);
    check("wat: disk load", dd != nullptr);
    check("wat: disk watermark applies",
          dd && ApplyWatermarkDoc(dd, L"TRIAL", 36.0f, WMDP_CENTER));
    std::vector<unsigned char> dbytes;
    bool ds = dd && SaveAsString(dd, dbytes);
    check("wat: disk doc saves after watermark", ds && dbytes.size() > 8);
    FPDF_DOCUMENT dr = dbytes.empty() ? nullptr
      : FPDF_LoadMemDocument(dbytes.data(), (int)dbytes.size(), nullptr);
    check("wat: disk doc roundtrip", dr != nullptr);
    if (dr)
    {
      FPDF_PAGE dp2 = FPDF_LoadPage(dr, 0);
      if (dp2)
      {
        check("wat: disk doc watermark persisted",
              PageTextRaw(dp2).find(L"TRIAL") != std::wstring::npos);
        FPDF_ClosePage(dp2);
      }
      FPDF_CloseDocument(dr);
    }
    if (dd) FPDF_CloseDocument(dd);
    DeleteFileW(dp.c_str());
  }

  {
    // --- Content-object editing (FPDFEdit API) ---
    FPDF_DOCUMENT ed = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
    check("obj: edit sample loads", ed != nullptr);
    if (ed)
    {
      FPDF_PAGE ep = FPDF_LoadPage(ed, 0);
      check("obj: edit page loads", ep != nullptr);
      if (ep)
      {
        const int n0 = FPDFPage_CountObjects(ep);
        checkEq("obj: two objects in sample", n0, 2);
        if (n0 == 2)
        {
          FPDF_PAGEOBJECT o0 = FPDFPage_GetObject(ep, 0);
          FPDF_PAGEOBJECT o1 = FPDFPage_GetObject(ep, 1);
          check("obj: first is text", FPDFPageObj_GetType(o0) == FPDF_PAGEOBJ_TEXT);
          check("obj: second is vector", FPDFPageObj_GetType(o1) == FPDF_PAGEOBJ_PATH);
          FPDF_TEXTPAGE et = FPDFText_LoadPage(ep);
          if (et)
          {
            FPDF_WCHAR wb[512] = {};
            unsigned long wl = FPDFTextObj_GetText(o0, et, wb, sizeof(wb));
            check("obj: text object has content",
                  wl > 2 &&
                  wcsncmp(reinterpret_cast<const wchar_t*>(wb), L"Stitchup", 8) == 0);
            FPDFText_ClosePage(et);
          }
          else check("obj: textpage", false);

          float ol = 0, ob = 0, or_ = 0, ot = 0;
          if (FPDFPageObj_GetBounds(o0, &ol, &ob, &or_, &ot) && ot > 700)
          {
            FPDFPageObj_Transform(o0, 1, 0, 0, 1, 10, 5);
            FPDFPage_GenerateContent(ep);
            FPDF_ClosePage(ep);
            ep = nullptr;
            std::vector<unsigned char> ebytes;
            bool eSaved = SaveAsString(ed, ebytes) && !ebytes.empty();
            check("obj: doc saves after transform", eSaved);
            FPDF_DOCUMENT er = eSaved ? FPDF_LoadMemDocument(ebytes.data(), (int)ebytes.size(), nullptr) : nullptr;
            check("obj: transformed doc reloads", er != nullptr);
            if (er)
            {
              FPDF_PAGE erp = FPDF_LoadPage(er, 0);
              check("obj: transformed page loads", erp != nullptr);
              if (erp)
              {
                FPDF_PAGEOBJECT eo0 = FPDFPage_GetObject(erp, 0);
                float nl = 0, nb = 0, nr = 0, nt = 0;
                if (FPDFPageObj_GetBounds(eo0, &nl, &nb, &nr, &nt))
                {
                  check("obj: moved object persisted x", std::fabs(nl - (ol + 10)) < 1.0f);
                  check("obj: moved object persisted y", std::fabs(nt - (ot + 5)) < 1.0f);
                }
                else check("obj: reloaded bounds query", false);
                FPDF_ClosePage(erp);
              }
              FPDF_CloseDocument(er);
            }
          }
          else check("obj: text bounds readable", false);
        }
        if (ep) FPDF_ClosePage(ep);
      }
      FPDF_CloseDocument(ed);
    }

    // Remove an object -> serialize -> reload -> one object (the path) remains.
    {
      FPDF_DOCUMENT rd = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
      check("obj: remove doc loads", rd != nullptr);
      if (rd)
      {
        FPDF_PAGE rp = FPDF_LoadPage(rd, 0);
        check("obj: remove page loads", rp != nullptr);
        if (rp)
        {
          FPDF_PAGEOBJECT r0 = FPDFPage_GetObject(rp, 0);
          if (r0)
          {
            FPDFPage_RemoveObject(rp, r0);
            FPDFPage_GenerateContent(rp);
          }
          FPDF_ClosePage(rp);
        }
        std::vector<unsigned char> rbytes;
        bool rSaved = SaveAsString(rd, rbytes) && !rbytes.empty();
        check("obj: doc saves after removal", rSaved);
        FPDF_DOCUMENT rr = rSaved ? FPDF_LoadMemDocument(rbytes.data(), (int)rbytes.size(), nullptr) : nullptr;
        check("obj: removed doc reloads", rr != nullptr);
        if (rr)
        {
          FPDF_PAGE rrp = FPDF_LoadPage(rr, 0);
          if (rrp)
          {
            checkEq("obj: one object remains", FPDFPage_CountObjects(rrp), 1);
            check("obj: remaining is the path",
                  FPDFPageObj_GetType(FPDFPage_GetObject(rrp, 0)) == FPDF_PAGEOBJ_PATH);
            FPDF_ClosePage(rrp);
          }
          FPDF_CloseDocument(rr);
        }
        FPDF_CloseDocument(rd);
      }
    }

    // Recolor: blue -> red, fill color round-trips through serialize/reload.
    {
      FPDF_DOCUMENT cd = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
      check("obj: recolor doc loads", cd != nullptr);
      if (cd)
      {
        FPDF_PAGE cp = FPDF_LoadPage(cd, 0);
        if (cp)
        {
          FPDF_PAGEOBJECT co = FPDFPage_GetObject(cp, 1);
          FPDFPageObj_SetFillColor(co, 255, 0, 0, 255);
          FPDFPageObj_SetStrokeColor(co, 255, 0, 0, 255);
          FPDFPage_GenerateContent(cp);
          FPDF_ClosePage(cp);
        }
        std::vector<unsigned char> cbytes;
        bool cSaved = SaveAsString(cd, cbytes) && !cbytes.empty();
        check("obj: recolor saved", cSaved);
        FPDF_DOCUMENT cr = cSaved ? FPDF_LoadMemDocument(cbytes.data(), (int)cbytes.size(), nullptr) : nullptr;
        check("obj: recolored doc reloads", cr != nullptr);
        if (cr)
        {
          FPDF_PAGE crp = FPDF_LoadPage(cr, 0);
          if (crp)
          {
            unsigned int R = 0, G = 0, B = 0, A = 0;
            FPDF_PAGEOBJECT co2 = crp ? FPDFPage_GetObject(crp, 1) : nullptr;
            if (co2 && FPDFPageObj_GetFillColor(co2, &R, &G, &B, &A))
              check("obj: fill is red now", R == 255 && G == 0 && B == 0 && A == 255);
            else
              check("obj: fill color query", false);
            FPDF_ClosePage(crp);
          }
          FPDF_CloseDocument(cr);
        }
        FPDF_CloseDocument(cd);
      }
    }

    // Text edit: replace the only run, serialize, reload -> new text present.
    {
      FPDF_DOCUMENT td = FPDF_LoadMemDocument(sample.data(), (int)sample.size(), nullptr);
      check("obj: edit-text doc loads", td != nullptr);
      if (td)
      {
        FPDF_PAGE tp = FPDF_LoadPage(td, 0);
        check("obj: edit-text page loads", tp != nullptr);
        bool textReplaced = false;
        if (tp)
        {
          FPDF_PAGEOBJECT to = FPDFPage_GetObject(tp, 0);
          float ol = 0, ob = 0, or_ = 0, ot = 0;
          FPDFPageObj_GetBounds(to, &ol, &ob, &or_, &ot);
          FPDF_PAGEOBJECT no2 = FPDFPageObj_NewTextObj(td, "Helvetica", 24.0f);
          if (no2)
          {
            const unsigned short* u16 = reinterpret_cast<const unsigned short*>(L"Edited Text");
            if (FPDFText_SetText(no2, u16))
            {
              FS_MATRIX tm{};
              tm.a = 1; tm.d = 1;
              FPDFPageObj_SetMatrix(no2, &tm);
              float nl = 0, nb = 0, nr = 0, nt = 0;
              FPDFPageObj_GetBounds(no2, &nl, &nb, &nr, &nt);
              tm.e = ol - nl; tm.f = ob - nb;
              FPDFPageObj_SetMatrix(no2, &tm);
              FPDFPage_RemoveObject(tp, to);
              FPDFPage_InsertObjectAtIndex(tp, no2, 0);
              textReplaced = true;
            }
            else FPDFPageObj_Destroy(no2);
          }
          FPDFPage_GenerateContent(tp);
          FPDF_ClosePage(tp);
        }
        check("obj: new text object installed", textReplaced);
        std::vector<unsigned char> tbytes;
        bool tSaved = SaveAsString(td, tbytes) && !tbytes.empty();
        check("obj: edited text saved", tSaved);
        FPDF_DOCUMENT tr = tSaved ? FPDF_LoadMemDocument(tbytes.data(), (int)tbytes.size(), nullptr) : nullptr;
        check("obj: edited text doc reloads", tr != nullptr);
        if (tr)
        {
          FPDF_PAGE trp = FPDF_LoadPage(tr, 0);
          if (trp)
          {
            const std::wstring tx = PageTextRaw(trp);
            check("obj: new text present", tx.find(L"Edited Text") != std::wstring::npos);
            check("obj: original text gone", tx.find(L"Stitchup") == std::wstring::npos);
            FPDF_ClosePage(trp);
          }
          FPDF_CloseDocument(tr);
        }
        FPDF_CloseDocument(td);
      }
    }
  }

  if (s) FPDF_CloseDocument(s);
  FPDF_DestroyLibrary();

  emit(std::string("SUMMARY ") + std::to_string(pass) +
       " passed, " + std::to_string(fail) + " failed");
  if (of) fclose(of);
}

static bool DemoMode(const std::wstring& outPath, int pages)
{
  if (pages < 1) pages = 1;
  if (pages > 100) pages = 100;
  FPDF_InitLibrary();
  FPDF_DOCUMENT d = FPDF_CreateNewDocument();
  if (!d) return false;
  const double sizes[][2] = {{612, 792}, {595, 842}, {612, 612}, {792, 612}};
  const int rots[] = {0, 1, 2, 3};
  for (int i = 0; i < pages; ++i)
  {
    FPDF_PAGE p = FPDFPage_New(d, i, sizes[i % 4][0], sizes[i % 4][1]);
    if (p)
    {
      FPDFPage_SetRotation(p, rots[i % 4]);
      FPDF_ClosePage(p);
    }
  }
  FileWriter fw{};
  fw.base.version = 1;
  fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
  {
    FileWriter* f = reinterpret_cast<FileWriter*>(self);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    f->buf.insert(f->buf.end(), p, p + size);
    return 1;
  };
  bool ok = FPDF_SaveAsCopy(d, &fw.base, FPDF_NO_INCREMENTAL);
  FPDF_CloseDocument(d);
  FPDF_DestroyLibrary();
  if (!ok) return false;
  FILE* f = nullptr;
  if (_wfopen_s(&f, outPath.c_str(), L"wb") != 0) return false;
  bool written = fwrite(fw.buf.data(), 1, fw.buf.size(), f) == fw.buf.size();
  fclose(f);
  if (!written) return false;
  return true;
}

static std::vector<std::wstring> GetArgs()
{
  int n = 0;
  LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &n);
  std::vector<std::wstring> out;
  if (raw)
  {
    for (int i = 0; i < n; ++i) out.push_back(raw[i]);
    LocalFree(raw);
  }
  return out;
}

static bool HasArg(const char* arg)
{
  return std::strstr(GetCommandLineA(), arg) != nullptr;
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------
static LRESULT CALLBACK ToolbarProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      RECT rc;
      GetClientRect(hw, &rc);
      const Theme& th = ThemeNow();
      HBRUSH bg = CreateSolidBrush(th.ribbonBg);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);

      // brand accent strip across the very top
      HBRUSH strip = CreateSolidBrush(th.accent);
      RECT sr{0, 0, rc.right, 2};
      FillRect(dc, &sr, strip);
      DeleteObject(strip);

      for (const App::GroupBox& gb : g.groups)
      {
        RECT card{gb.rc.left + 2, RIB_BTN_Y - 6, gb.rc.right - 2, RIB_H - 2};
        HBRUSH fill = CreateSolidBrush(th.card);
        HPEN pen = CreatePen(PS_SOLID, 1, th.cardBorder);
        HBRUSH wasb = (HBRUSH)SelectObject(dc, fill);
        HPEN wasp = (HPEN)SelectObject(dc, pen);
        RoundRect(dc, card.left, card.top, card.right, card.bottom, 10, 10);
        SelectObject(dc, wasb);
        SelectObject(dc, wasp);
        DeleteObject(fill);
        DeleteObject(pen);

        RECT cap{card.left + 4, RIB_CAP_Y, card.right - 4, RIB_H - 2};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, th.textDim);
        HFONT small = CreateFontW(-MulDiv(8, g.dpi, 72), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Segoe UI");
        HFONT was = (HFONT)SelectObject(dc, small);
        DrawTextW(dc, gb.name.c_str(), -1, &cap,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, was);
        DeleteObject(small);
      }

      if (GroupCount(g.ribbonTab) == 0)
      {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, th.textDim);
        RECT trc{8, RIB_BTN_Y, rc.right - 8, RIB_CAP_Y};
        DrawTextW(dc,
                  L"Annotations, forms, security and advanced tools arrive in "
                  L"later milestones.",
                  -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      }

      HPEN pen = CreatePen(PS_SOLID, 1, th.cardBorder);
      SelectObject(dc, pen);
      MoveToEx(dc, 0, rc.bottom - 1, nullptr);
      LineTo(dc, rc.right, rc.bottom - 1);
      DeleteObject(pen);
      EndPaint(hw, &ps);
      return 0;
    }
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
  g.inst = inst;

  std::vector<std::wstring> args = GetArgs();
  if (args.size() >= 2 && args[1] == L"--demo")
  {
    if (args.size() < 3) return 2;
    int pages = (args.size() >= 4) ? _wtoi(args[3].c_str()) : 4;
    bool ok = DemoMode(args[2], pages);
    return ok ? 0 : 1;
  }
  if (HasArg("--self-test"))
  {
    std::wstring cwd(MAX_PATH, L'\0');
    DWORD n = GetCurrentDirectoryW((DWORD)cwd.size(), &cwd[0]);
    cwd.resize(n);
    SelfTest(cwd);
    return 0;
  }

  std::wstring openFile;
  if (args.size() >= 2 && args[1].compare(0, 2, L"--") != 0)
    openFile = args[1];

  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  INITCOMMONCONTROLSEX iccex{sizeof(iccex), ICC_TREEVIEW_CLASSES};
  InitCommonControlsEx(&iccex);

  ApplyInitialThemePref();

  HWND hidden = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 0, 0,
                                nullptr, nullptr, inst, nullptr);
  HDC hdc = GetDC(hidden);
  g.dpi = GetDeviceCaps(hdc, LOGPIXELSX);
  ReleaseDC(hidden, hdc);
  DestroyWindow(hidden);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.hbrBackground = nullptr;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpfnWndProc = FrameProc;
  wc.hInstance = inst;
  wc.lpszClassName = L"SKFrame";
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
  wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(101));
  RegisterClassExW(&wc);

  wc.lpfnWndProc = ToolbarProc;
  wc.lpszClassName = L"SKToolbar";
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  wc.lpfnWndProc = ToolBtnProc;
  wc.lpszClassName = L"SKToolBtn";
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  wc.lpfnWndProc = ThumbsProc;
  wc.lpszClassName = L"SKThumbs";
  RegisterClassExW(&wc);

  wc.lpfnWndProc = SplitProc;
  wc.lpszClassName = L"SKSplit";
  wc.hCursor = LoadCursorW(nullptr, IDC_SIZEWE);
  RegisterClassExW(&wc);

  wc.lpfnWndProc = CanvasProc;
  wc.lpszClassName = L"SKCanvas";
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  RegisterClassExW(&wc);

  g.font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  FPDF_InitLibrary();

  HWND frame = CreateWindowExW(0, L"SKFrame", L"Stitchup PDF Editor",
                               WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                               1240, 820, nullptr, BuildMenu(), inst, nullptr);
  if (!frame)
  {
    MessageBoxW(nullptr, L"Window creation failed.", L"Stitchup",
                MB_OK | MB_ICONERROR);
    FPDF_DestroyLibrary();
    return 1;
  }
  g.frame = frame;

  ApplyTreeTheme();
  CheckMenuItem(GetMenu(g.frame), ID_THEME,
                MF_BYCOMMAND | (g_dark ? MF_CHECKED : MF_UNCHECKED));

  if (openFile.empty()) NewDoc();
  else LoadDoc(openFile);
  ShowWindow(frame, SW_SHOW);
  UpdateWindow(frame);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CloseDoc();
  FPDF_DestroyLibrary();
  DeleteObject(g.font);
  return (int)msg.wParam;
}