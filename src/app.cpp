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
};

static App g;

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

static HBITMAP RenderPageBitmap(int index, int w, int h)
{
  if (g.pageCount == 0 || w < 1 || h < 1) return nullptr;
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

  FPDF_PAGE p = FPDF_LoadPage(g.doc, index);
  if (p)
  {
    FPDF_RenderPageBitmap(fb, p, 0, 0, w, h, 0,
                          FPDF_ANNOT | FPDF_LCD_TEXT);
    FPDF_ClosePage(p);
  }
  FPDFBitmap_Destroy(fb);
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
      HBRUSH bg = CreateSolidBrush(RGB(0x2B, 0x2B, 0x2B));
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(0xE8, 0xE8, 0xE8));
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
      HBRUSH bgb = CreateSolidBrush(RGB(0xEC, 0xEC, 0xEC));
      FillRect(dc, &rc, bgb);
      DeleteObject(bgb);

      RECT hr{rc.left + 10, 6, rc.right - 10, 26};
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, RGB(0x33, 0x33, 0x33));
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
        HPEN pn = CreatePen(PS_SOLID, 2, RGB(0x0B, 0x6C, 0xE0));
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
            HBRUSH dim = CreateSolidBrush(RGB(0xD8, 0xE4, 0xF5));
            RECT dr{x + 2, yc - 2, x + 4 + tw, yc + th + 6};
            FillRect(dc, &dr, dim);
            DeleteObject(dim);
          }
          RECT pr{x + 2, yc - 2, x + 4 + tw, yc + th + 6};
          HBRUSH phb = CreateSolidBrush(sel ? RGB(0x0B, 0x6C, 0xE0)
                                            : RGB(0xAA, 0xAA, 0xAA));
          FrameRect(dc, &pr, phb);
          DeleteObject(phb);
          std::wstring num = std::to_wstring(i + 1);
          RECT nr{x + 4, yc + th + 4, x + thumbW, yc + th + 14};
          SetTextColor(dc, sel ? RGB(0x0B, 0x6C, 0xE0) : RGB(0x55, 0x55, 0x55));
          DrawTextW(dc, num.c_str(), -1, &nr, DT_SINGLELINE);
        }
        yc += th + 16;
        if (g.dragPage >= 0 && i + 1 == g.dragCursor)
        {
          HPEN pn = CreatePen(PS_SOLID, 2, RGB(0x0B, 0x6C, 0xE0));
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
static void CanvasPaint(HDC dc, int cw, int ch)
{
  HBRUSH bg = CreateSolidBrush(RGB(0xE2, 0xE2, 0xE2));
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
      // shadow
      HBRUSH sh = CreateSolidBrush(RGB(0xBF, 0xBF, 0xBF));
      RECT sr{x + 4, yc + 4, x + w + 4, yc + h + 4};
      FillRect(dc, &sr, sh);
      DeleteObject(sh);

      auto it = g.canvasCache.find(i);
      HBITMAP hb = nullptr;
      key = ZoomKey();
      if (it != g.canvasCache.end() && it->second.key == key)
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
      }
      HBRUSH fb = CreateSolidBrush(g.selected == i ? RGB(0x0B, 0x6C, 0xE0)
                                                   : RGB(0x99, 0x99, 0x99));
      FrameRect(dc, &r, fb);
      DeleteObject(fb);
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
  if (b->tab)
  {
    HBRUSH bg = CreateSolidBrush(b->pressed ? RGB(0xFF, 0xFF, 0xFF)
                                 : b->hover   ? RGB(0xE9, 0xEF, 0xFA)
                                              : RGB(0xF0, 0xF1, 0xF4));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    if (b->pressed)
    {
      HBRUSH acc = CreateSolidBrush(RGB(0x0B, 0x6C, 0xE0));
      RECT ar{rc.left + 4, rc.bottom - 3, rc.right - 4, rc.bottom};
      FillRect(dc, &ar, acc);
      DeleteObject(acc);
    }
    HFONT was = (HFONT)SelectObject(dc, g.font);
    SetTextColor(dc, b->pressed ? RGB(0x0B, 0x3E, 0x77) : RGB(0x40, 0x40, 0x40));
    DrawTextW(dc, b->label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, was);
  }
  else
  {
    HBRUSH bg = CreateSolidBrush(b->down   ? RGB(0xC8, 0xDC, 0xF2)
                                 : b->hover ? RGB(0xE6, 0xEF, 0xFB)
                                            : RGB(0xF7, 0xF8, 0xFA));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD5, 0xD5, 0xD5));
    SelectObject(dc, pen);
    MoveToEx(dc, 0, rc.bottom - 1, nullptr);
    LineTo(dc, rc.right, rc.bottom - 1);
    DeleteObject(pen);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(0x20, 0x20, 0x20));
    DrawTextW(dc, b->label.c_str(), -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
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
  if (tab == 0) return grp == 0 ? L"Document" : (grp == 1 ? L"Pages" : L"Annotate");
  if (tab == 1)
    return grp == 0 ? L"Zoom" : (grp == 1 ? L"Navigate" : L"Panes");
  return nullptr;
}

static int GroupCount(int tab)
{
  return tab == 0 ? 3 : (tab == 1 ? 3 : 0);
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
    case ID_IMPORT: ImportPdf(); break;
    case ID_DELETE: DeletePage(); break;
    case ID_ADD:    AddPage(); break;
    case ID_ROTL:   RotatePage(3); break;
    case ID_ROTR:   RotatePage(1); break;
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
  addItem(file, ID_IMPORT, L"Import PDF...");
  addItem(file, ID_EXPORT_TEXT, L"Export Text...");
  addItem(file, ID_EXPORT_CSV, L"Export CSV...");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
  addItem(file, ID_EXIT, L"Exit");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, L"&File");

  HMENU edit = CreatePopupMenu();
  addItem(edit, ID_ROTR, L"Rotate Right\tCtrl+R");
  addItem(edit, ID_ROTL, L"Rotate Left\tCtrl+Shift+R");
  addItem(edit, ID_DELETE, L"Delete Page\tDel");
  addItem(edit, ID_ADD, L"Add Page...");
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
        }
      }
      else
      {
        switch (vk)
        {
          case VK_DELETE: DoCommand(ID_DELETE); return 0;
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

// Builds an encrypted single-page PDF (Standard security handler V1) whose
// user password is "stitchup". Variants: 1 = R2/40-bit, 2 = R3/128-bit,
// 3 = R2/128-bit. The selftest probes each until this pdfium accepts one.
static std::string MakeEncryptedPdf(int variant)
{
  static const unsigned char kPadding[32] = {
    0x28,0xBF,0x4E,0x5E,0x4E,0x75,0x8A,0x41,0x64,0x00,0x4E,0x56,0xFF,0xFA,0x01,0x08,
    0x2E,0x2E,0x00,0xB6,0xD0,0x68,0x3E,0x80,0x2F,0x0C,0xA9,0xFE,0x64,0x53,0x69,0x7A };
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
      HBRUSH bg = CreateSolidBrush(RGB(0xF7, 0xF8, 0xFA));
      FillRect(dc, &rc, bg);
      DeleteObject(bg);

      for (const App::GroupBox& gb : g.groups)
      {
        RECT gr{gb.rc.left, RIB_BTN_Y - 8, gb.rc.right, gb.rc.bottom};
        HBRUSH gbbr = CreateSolidBrush(RGB(0xEF, 0xF0, 0xF3));
        FillRect(dc, &gr, gbbr);
        DeleteObject(gbbr);
        RECT cap{gb.rc.left + 2, gb.rc.top, gb.rc.right - 2, gb.rc.bottom};
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x40, 0x40, 0x40));
        HFONT small = CreateFontW(-MulDiv(8, g.dpi, 72), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
                                  L"Segoe UI");
        HFONT was = (HFONT)SelectObject(dc, small);
        DrawTextW(dc, gb.name.c_str(), -1, &cap,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(dc, was);
        DeleteObject(small);
      }

      if (GroupCount(g.ribbonTab) == 0)
      {
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(0x80, 0x80, 0x80));
        RECT trc{8, RIB_BTN_Y, rc.right - 8, RIB_CAP_Y};
        DrawTextW(dc,
                  L"Annotations, forms, security and advanced tools arrive in "
                  L"later milestones.",
                  -1, &trc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
      }

      HPEN pen = CreatePen(PS_SOLID, 1, RGB(0xD0, 0xD0, 0xD0));
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
  wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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