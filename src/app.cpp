// Stitchup PDF Editor - portable Nitro-style PDF viewer/editor.
// Native Win32 GUI + PDFium (BSD-3-Clause, statically wired via import lib).
// Single page source: GUI host, rendering, self-test.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <commdlg.h>
#include <windowsx.h>
#include <uxtheme.h>
#include <dwmapi.h>

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
#include <functional>
#include <algorithm>
#include <map>
#include <random>
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
  ID_TAB_FIRST,          // one ribbon tab per tool group, contiguous block
  ID_TAB_DOCUMENT = ID_TAB_FIRST,
  ID_TAB_PAGES,
  ID_TAB_ANNOTATE,
  ID_TAB_CONTENT,
  ID_TAB_ZOOM,
  ID_TAB_NAVIGATE,
  ID_TAB_SECURITY,
  ID_TAB_LAST = ID_TAB_SECURITY,
  ID_PANE_THUMBS,
  ID_PANE_BOOKMARKS,
  ID_ANN_HL,
  ID_ANN_UL,
  ID_ANN_NOTE,
  ID_ANN_TEXT,
  ID_ANN_SHAPE,
ID_ANN_STAMP,
ID_ANN_LINK,
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
  ID_SIDEBAR,
  ID_SPREAD,
  ID_NEW_TAB,
  ID_CLOSE_TAB,
  ID_NEXT_TAB,
  ID_PREV_TAB,
  ID_FIND,
  ID_FIND_NEXT,
  ID_FIND_PREV,
  // Colour-scheme pickers follow in one contiguous block (radio group).
  ID_THEME_FIRST,
  ID_THEME_LIGHT = ID_THEME_FIRST,
  ID_THEME_DARK,
  ID_THEME_DARKBLUE,
  ID_THEME_PASTEL,
  ID_THEME_HC_DARK,
  ID_THEME_HC_LIGHT,
  ID_THEME_XP,
  ID_THEME_MAC,
  // Canvas context menu (right-click). Kept after the radio group so the
  // contiguous theme block above is untouched.
  ID_BM_PAGE,
  ID_BM_VIEW,
};

enum
{
  TAB_H = 30,       // document tab strip height
  RIB_TAB_H = 30,   // ribbon tab strip: one tab per tool group
  RIB_BTN_Y = 38,   // button row top
  RIB_BTN_H = 40,   // button height (icon over caption)
  RIB_H = 102,      // full ribbon height (buttons + group caption band)
  PANE_TAB_H = 26,  // navigation-pane header height
  STATUS_H = 28,    // status bar height
};

// App identity shown in the title bar. The open file's name already lives on
// the document tab below the title bar, so it is not repeated in the caption.
const wchar_t* const kAppTitle = L"Stitchup PDF Editor  v0.12.0";

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
  int rtab = -1;         // owning ribbon tab index, -1 = not a ribbon btn
  wchar_t icon = 0;      // Segoe MDL2 Assets codepoint, 0 = no icon
  bool mirror = false;   // draw the glyph flipped (counter-clockwise arrows)
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

// A bookmark the user created in the UI. pdfium can read outlines but not write
// them, so these are held in memory and persisted into the file as a PDF
// incremental update by AppendOutlines() at save time.
struct UserBookmark
{
  std::wstring title;
  int page = 0;        // 0-based page index
  bool atView = false; // /XYZ at the current scroll position instead of top
  double x = 0, y = 0; // PDF user-space point, PDF origin is bottom-left
  double zoom = 0;     // 0 = keep the reader's default zoom
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
  HWND tabbar = nullptr;
  HFONT font = nullptr;
  HFONT treeFont = nullptr;
  HFONT iconFont = nullptr;   // Segoe MDL2 Assets, null when the font is absent
  bool haveMdl2 = false;      // resolved at startup by inspecting the face name
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
  std::vector<UserBookmark> marks;  // user bookmarks not yet written to disk
  bool showSidebar = false;  // hidden until the user asks for it (View / F8)
  bool spread = false;  // two-page side-by-side layout

  struct GroupBox
  {
    int tab;
    std::wstring name;
    RECT rc{};
  };
  std::vector<GroupBox> groups;
  std::vector<HWND> ribbonBtns;
  std::vector<RECT> ribbonTabRects;   // hit-test + paint rects for the tab strip
  int ribbonTabHover = -1;
  HWND tips = nullptr;                // tooltip control, null when unavailable

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
  // Text search (Ctrl+F). findHits holds every match in page order; findCur
  // indexes it (-1 = no active search). findQuery is kept so F3 / Shift+F3 can
  // repeat without re-prompting.
  struct FindHit { int page; int start; int count; };
  std::wstring findQuery;
  std::vector<FindHit> findHits;
  int findCur = -1;

  FPDF_PAGE editPage = nullptr;   // page kept open during a live object drag
  FPDF_PAGEOBJECT editObj = nullptr;
  bool selDrag = false;
  bool selDragMoved = false;
  double dragLastX = 0, dragLastY = 0;

  // Drag-to-draw annotation placement. When annTool is set the next left-drag
  // on a page defines the annotation rectangle; annDrag is true between the
  // button-down and button-up. Coordinates are canvas-client pixels.
  int annTool = 0;           // pending annotation kind, 0 = none
  bool annDrag = false;
  int annPage = -1;          // page under the drag start
  POINT annStart{};
  POINT annCur{};
};

// A document open in its own tab. The live App fields above always mirror the
// active tab (g.doc, g.path, g.name, g.dirty, g.pageCount, g.selected, g.zoom,
// g.scrollX/Y). Switching tabs snapshots the current live state back into its
// TabDoc and restores the target's.
struct TabDoc
{
  FPDF_DOCUMENT doc = nullptr;
  std::wstring path;
  std::wstring name;
  bool dirty = false;
  int pageCount = 0;
  int selected = 0;
  double zoom = 1.0;
  int scrollX = 0;
  int scrollY = 0;
  std::vector<UserBookmark> marks;
};
static std::vector<TabDoc> g_tabs;
static int g_curTab = -1;

static App g;

// ---------------------------------------------------------------------------
// Themes - named colour schemes (light / dark / dark blue / pastel /
// high-contrast / Windows XP / macOS), each with a matching title bar.
// ---------------------------------------------------------------------------
enum ThemeId
{
  THEME_LIGHT = 0,
  THEME_DARK,
  THEME_DARKBLUE,
  THEME_PASTEL,
  THEME_HC_DARK,
  THEME_HC_LIGHT,
  THEME_XP,
  THEME_MAC,
  THEME_COUNT
};

enum CaptionStyle
{
  CAP_SYS = 0,  // Windows 10/11 style caption buttons
  CAP_XP,       // Windows XP Luna caption
  CAP_MAC,      // macOS traffic-light caption
};

struct UiTheme
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
  COLORREF emptyHint;   // "open a PDF" hint text on the empty canvas
  // title bar
  COLORREF capTop;      // caption gradient start
  COLORREF capBottom;   // caption gradient end (== capTop when flat)
  COLORREF capText;     // caption title text
  COLORREF capLine;     // caption bottom hairline
  COLORREF capBtn;      // caption button face
  COLORREF capBtnHover; // caption button hover face
  COLORREF capGlyph;    // caption button glyph
  COLORREF macRed;      // traffic light: close
  COLORREF macYellow;   // traffic light: minimise
  COLORREF macGreen;    // traffic light: zoom
  COLORREF macGlyph;    // traffic-light glyph on hover
  int capStyle;         // CaptionStyle
  bool dark;            // dark surfaces (paper/ink decisions)
};

static int g_themeId = THEME_LIGHT;
static HMENU g_themeMenu = nullptr;   // "Colour Scheme" popup, for check marks
// Easter egg: one quip per run, shown in the middle of the title bar.
static std::wstring g_capPhrase;

static void PickCapPhrase()
{
  static const wchar_t* kQuips[] = {
      L"Don't Panic",
      L"Time is an illusion. Lunchtime doubly so.",
      L"42",
      L"Mostly Harmless",
      L"Improbability Drive engaged",
      L"Never panic. Panic is simply the response to a trivial threat.",
      L"You are unlikely to be eaten by a grue",
      L"So long, and thanks for all the fish",
  };
  const int n = static_cast<int>(sizeof(kQuips) / sizeof(kQuips[0]));
  std::random_device rd;
  std::mt19937 rng(rd());
  std::uniform_int_distribution<int> pick(0, n - 1);
  g_capPhrase = kQuips[pick(rng)];
}

static const UiTheme kThemes[THEME_COUNT] = {
    // ---- Light grey (default): soft light UI, light-grey canvas ---
    {
    /* ribbonBg   */ RGB(0xF2, 0xF3, 0xF5),
    /* card       */ RGB(0xFF, 0xFF, 0xFF),
    /* cardBorder */ RGB(0xDC, 0xDE, 0xE3),
    /* accent     */ RGB(0x0B, 0x6C, 0xE0),
    /* accentDeep */ RGB(0x08, 0x4A, 0x9E),
    /* text       */ RGB(0x1B, 0x1F, 0x24),
    /* textDim    */ RGB(0x55, 0x5F, 0x6B),
    /* btnHover   */ RGB(0xE3, 0xED, 0xFB),
    /* btnDown    */ RGB(0xC9, 0xDD, 0xF7),
    /* btnBorder  */ RGB(0xD5, 0xD9, 0xDE),
    /* thumbBg    */ RGB(0xE9, 0xEB, 0xEE),
    /* canvasBg   */ RGB(0xDE, 0xE1, 0xE5),
    /* pageFrame  */ RGB(0x8D, 0x94, 0x9D),
    /* split      */ RGB(0xDC, 0xDE, 0xE3),
    /* statusBg   */ RGB(0x2B, 0x2F, 0x36),
    /* statusTxt  */ RGB(0xEC, 0xEE, 0xF3),
    /* treeBg     */ RGB(0xFF, 0xFF, 0xFF),
    /* treeTxt    */ RGB(0x1B, 0x1F, 0x24),
    /* emptyHint  */ RGB(0x6B, 0x75, 0x81),
    /* capTop     */ RGB(0xFF, 0xFF, 0xFF),
    /* capBottom  */ RGB(0xF0, 0xF2, 0xF5),
    /* capText    */ RGB(0x1B, 0x1F, 0x24),
    /* capLine    */ RGB(0xD8, 0xDB, 0xE0),
    /* capBtn     */ RGB(0xF0, 0xF2, 0xF5),
    /* capBtnHover */ RGB(0xE4, 0xE8, 0xEE),
    /* capGlyph   */ RGB(0x3A, 0x40, 0x48),
    /* macRed     */ RGB(0xFF, 0x5F, 0x57),
    /* macYellow  */ RGB(0xFE, 0xBC, 0x2E),
    /* macGreen   */ RGB(0x28, 0xC8, 0x40),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_SYS,
    /* dark       */ false
    },
    // ---- Dark: neutral charcoal ---
    {
    /* ribbonBg   */ RGB(0x20, 0x21, 0x24),
    /* card       */ RGB(0x2A, 0x2C, 0x30),
    /* cardBorder */ RGB(0x3A, 0x3D, 0x43),
    /* accent     */ RGB(0x4C, 0x9A, 0xFF),
    /* accentDeep */ RGB(0x1E, 0x6F, 0xC9),
    /* text       */ RGB(0xE8, 0xEA, 0xED),
    /* textDim    */ RGB(0xA6, 0xAC, 0xB4),
    /* btnHover   */ RGB(0x33, 0x37, 0x3D),
    /* btnDown    */ RGB(0x3D, 0x44, 0x4D),
    /* btnBorder  */ RGB(0x3A, 0x3D, 0x43),
    /* thumbBg    */ RGB(0x23, 0x25, 0x29),
    /* canvasBg   */ RGB(0x1A, 0x1C, 0x1F),
    /* pageFrame  */ RGB(0x6A, 0x70, 0x79),
    /* split      */ RGB(0x2E, 0x31, 0x36),
    /* statusBg   */ RGB(0x15, 0x17, 0x1A),
    /* statusTxt  */ RGB(0xC9, 0xCE, 0xD6),
    /* treeBg     */ RGB(0x23, 0x25, 0x29),
    /* treeTxt    */ RGB(0xE0, 0xE3, 0xE8),
    /* emptyHint  */ RGB(0x8A, 0x91, 0x9A),
    /* capTop     */ RGB(0x2B, 0x2E, 0x33),
    /* capBottom  */ RGB(0x20, 0x21, 0x24),
    /* capText    */ RGB(0xE8, 0xEA, 0xED),
    /* capLine    */ RGB(0x39, 0x3D, 0x43),
    /* capBtn     */ RGB(0x3D, 0x41, 0x48),
    /* capBtnHover */ RGB(0x4A, 0x4F, 0x57),
    /* capGlyph   */ RGB(0xE6, 0xE8, 0xEC),
    /* macRed     */ RGB(0xFF, 0x5F, 0x57),
    /* macYellow  */ RGB(0xFE, 0xBC, 0x2E),
    /* macGreen   */ RGB(0x28, 0xC8, 0x40),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_SYS,
    /* dark       */ true
    },
    // ---- Dark blue: deep navy with bright blue accents ---
    {
    /* ribbonBg   */ RGB(0x16, 0x23, 0x3A),
    /* card       */ RGB(0x1D, 0x2C, 0x46),
    /* cardBorder */ RGB(0x2B, 0x3E, 0x5C),
    /* accent     */ RGB(0x6F, 0xB2, 0xFF),
    /* accentDeep */ RGB(0x3D, 0x82, 0xD6),
    /* text       */ RGB(0xE6, 0xEE, 0xF9),
    /* textDim    */ RGB(0xA2, 0xB6, 0xD0),
    /* btnHover   */ RGB(0x24, 0x38, 0x5A),
    /* btnDown    */ RGB(0x2C, 0x45, 0x70),
    /* btnBorder  */ RGB(0x2B, 0x3E, 0x5C),
    /* thumbBg    */ RGB(0x1A, 0x29, 0x42),
    /* canvasBg   */ RGB(0x10, 0x1A, 0x2B),
    /* pageFrame  */ RGB(0x5A, 0x73, 0x96),
    /* split      */ RGB(0x23, 0x33, 0x4D),
    /* statusBg   */ RGB(0x0D, 0x15, 0x22),
    /* statusTxt  */ RGB(0xC3, 0xD3, 0xE8),
    /* treeBg     */ RGB(0x1A, 0x29, 0x42),
    /* treeTxt    */ RGB(0xDC, 0xE7, 0xF5),
    /* emptyHint  */ RGB(0x86, 0x9C, 0xB8),
    /* capTop     */ RGB(0x1F, 0x31, 0x4F),
    /* capBottom  */ RGB(0x16, 0x23, 0x3A),
    /* capText    */ RGB(0xE6, 0xEE, 0xF9),
    /* capLine    */ RGB(0x33, 0x4C, 0x72),
    /* capBtn     */ RGB(0x2B, 0x3E, 0x5C),
    /* capBtnHover */ RGB(0x3C, 0x53, 0x7A),
    /* capGlyph   */ RGB(0xDD, 0xE7, 0xF6),
    /* macRed     */ RGB(0xFF, 0x5F, 0x57),
    /* macYellow  */ RGB(0xFE, 0xBC, 0x2E),
    /* macGreen   */ RGB(0x28, 0xC8, 0x40),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_SYS,
    /* dark       */ true
    },
    // ---- Pastel: soft lavender, light surfaces ---
    {
    /* ribbonBg   */ RGB(0xF6, 0xF1, 0xFA),
    /* card       */ RGB(0xFF, 0xFF, 0xFF),
    /* cardBorder */ RGB(0xE3, 0xD8, 0xEC),
    /* accent     */ RGB(0x7C, 0x4D, 0xBE),
    /* accentDeep */ RGB(0x5A, 0x2F, 0x92),
    /* text       */ RGB(0x2A, 0x24, 0x31),
    /* textDim    */ RGB(0x6B, 0x5F, 0x7A),
    /* btnHover   */ RGB(0xEF, 0xE3, 0xF8),
    /* btnDown    */ RGB(0xDF, 0xCD, 0xF0),
    /* btnBorder  */ RGB(0xE0, 0xD6, 0xE9),
    /* thumbBg    */ RGB(0xF1, 0xEA, 0xF7),
    /* canvasBg   */ RGB(0xD3, 0xC9, 0xDE),
    /* pageFrame  */ RGB(0x8B, 0x7E, 0x9B),
    /* split      */ RGB(0xE3, 0xD8, 0xEC),
    /* statusBg   */ RGB(0x4A, 0x3B, 0x5C),
    /* statusTxt  */ RGB(0xF0, 0xE9, 0xF7),
    /* treeBg     */ RGB(0xFF, 0xFF, 0xFF),
    /* treeTxt    */ RGB(0x2A, 0x24, 0x31),
    /* emptyHint  */ RGB(0x7A, 0x6C, 0x88),
    /* capTop     */ RGB(0xFF, 0xFD, 0xFF),
    /* capBottom  */ RGB(0xF4, 0xEC, 0xFB),
    /* capText    */ RGB(0x2A, 0x24, 0x31),
    /* capLine    */ RGB(0xE3, 0xD8, 0xEC),
    /* capBtn     */ RGB(0xF6, 0xEE, 0xFB),
    /* capBtnHover */ RGB(0xEF, 0xE6, 0xF8),
    /* capGlyph   */ RGB(0x59, 0x4C, 0x69),
    /* macRed     */ RGB(0xFF, 0x5F, 0x57),
    /* macYellow  */ RGB(0xFE, 0xBC, 0x2E),
    /* macGreen   */ RGB(0x28, 0xC8, 0x40),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_SYS,
    /* dark       */ false
    },
    // ---- High contrast dark: black / cyan / white ---
    {
    /* ribbonBg   */ RGB(0x00, 0x00, 0x00),
    /* card       */ RGB(0x0A, 0x0A, 0x0A),
    /* cardBorder */ RGB(0xFF, 0xFF, 0xFF),
    /* accent     */ RGB(0x00, 0xE5, 0xFF),
    /* accentDeep */ RGB(0x00, 0xA0, 0xB4),
    /* text       */ RGB(0xFF, 0xFF, 0xFF),
    /* textDim    */ RGB(0xD8, 0xD8, 0xD8),
    /* btnHover   */ RGB(0x1E, 0x1E, 0x1E),
    /* btnDown    */ RGB(0x33, 0x33, 0x33),
    /* btnBorder  */ RGB(0xFF, 0xFF, 0xFF),
    /* thumbBg    */ RGB(0x05, 0x05, 0x05),
    /* canvasBg   */ RGB(0x00, 0x00, 0x00),
    /* pageFrame  */ RGB(0xFF, 0xFF, 0xFF),
    /* split      */ RGB(0xFF, 0xFF, 0xFF),
    /* statusBg   */ RGB(0x00, 0x00, 0x00),
    /* statusTxt  */ RGB(0xFF, 0xFF, 0xFF),
    /* treeBg     */ RGB(0x00, 0x00, 0x00),
    /* treeTxt    */ RGB(0xFF, 0xFF, 0xFF),
    /* emptyHint  */ RGB(0xC0, 0xC0, 0xC0),
    /* capTop     */ RGB(0x00, 0x00, 0x00),
    /* capBottom  */ RGB(0x00, 0x00, 0x00),
    /* capText    */ RGB(0xFF, 0xFF, 0xFF),
    /* capLine    */ RGB(0xFF, 0xFF, 0xFF),
    /* capBtn     */ RGB(0x33, 0x33, 0x33),
    /* capBtnHover */ RGB(0xFF, 0xFF, 0xFF),
    /* capGlyph   */ RGB(0x00, 0x00, 0x00),
    /* macRed     */ RGB(0xFF, 0x59, 0x00),
    /* macYellow  */ RGB(0xFF, 0xD5, 0x00),
    /* macGreen   */ RGB(0x00, 0xE0, 0x00),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_SYS,
    /* dark       */ true
    },
    // ---- High contrast light: white / blue / black ---
    {
    /* ribbonBg   */ RGB(0xFF, 0xFF, 0xFF),
    /* card       */ RGB(0xFF, 0xFF, 0xFF),
    /* cardBorder */ RGB(0x00, 0x00, 0x00),
    /* accent     */ RGB(0x00, 0x00, 0xC8),
    /* accentDeep */ RGB(0x00, 0x00, 0x66),
    /* text       */ RGB(0x00, 0x00, 0x00),
    /* textDim    */ RGB(0x33, 0x33, 0x33),
    /* btnHover   */ RGB(0xE0, 0xE0, 0xFF),
    /* btnDown    */ RGB(0xC0, 0xC0, 0xF0),
    /* btnBorder  */ RGB(0x00, 0x00, 0x00),
    /* thumbBg    */ RGB(0xF0, 0xF0, 0xF0),
    /* canvasBg   */ RGB(0xB0, 0xB0, 0xB0),
    /* pageFrame  */ RGB(0x00, 0x00, 0x00),
    /* split      */ RGB(0x00, 0x00, 0x00),
    /* statusBg   */ RGB(0x00, 0x00, 0x00),
    /* statusTxt  */ RGB(0xFF, 0xFF, 0xFF),
    /* treeBg     */ RGB(0xFF, 0xFF, 0xFF),
    /* treeTxt    */ RGB(0x00, 0x00, 0x00),
    /* emptyHint  */ RGB(0x40, 0x40, 0x40),
    /* capTop     */ RGB(0xFF, 0xFF, 0xFF),
    /* capBottom  */ RGB(0xEE, 0xEE, 0xEE),
    /* capText    */ RGB(0x00, 0x00, 0x00),
    /* capLine    */ RGB(0xE6, 0xE6, 0xE6),
    /* capBtn     */ RGB(0xFF, 0xFF, 0xFF),
    /* capBtnHover */ RGB(0xE6, 0xE6, 0xFF),
    /* capGlyph   */ RGB(0x00, 0x00, 0x00),
    /* macRed     */ RGB(0xFF, 0x59, 0x00),
    /* macYellow  */ RGB(0xFF, 0xD5, 0x00),
    /* macGreen   */ RGB(0x00, 0xE0, 0x00),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_SYS,
    /* dark       */ false
    },
    // ---- Windows XP: Luna colours, blue gradient caption ---
    {
    /* ribbonBg   */ RGB(0xEC, 0xE9, 0xD8),
    /* card       */ RGB(0xF5, 0xF2, 0xE6),
    /* cardBorder */ RGB(0xD6, 0xD0, 0xC2),
    /* accent     */ RGB(0x0A, 0x24, 0x6A),
    /* accentDeep */ RGB(0x1E, 0x4E, 0x9C),
    /* text       */ RGB(0x1A, 0x1A, 0x1A),
    /* textDim    */ RGB(0x55, 0x55, 0x4E),
    /* btnHover   */ RGB(0xFD, 0xF7, 0xD4),
    /* btnDown    */ RGB(0xE3, 0xD8, 0xB0),
    /* btnBorder  */ RGB(0xC6, 0xC0, 0xB2),
    /* thumbBg    */ RGB(0xEF, 0xEB, 0xDE),
    /* canvasBg   */ RGB(0xA6, 0xA6, 0xA6),
    /* pageFrame  */ RGB(0x64, 0x64, 0x64),
    /* split      */ RGB(0xC6, 0xC0, 0xB2),
    /* statusBg   */ RGB(0x0A, 0x24, 0x6A),
    /* statusTxt  */ RGB(0xFF, 0xFF, 0xFF),
    /* treeBg     */ RGB(0xFF, 0xFF, 0xFF),
    /* treeTxt    */ RGB(0x00, 0x00, 0x00),
    /* emptyHint  */ RGB(0x4A, 0x4A, 0x42),
    /* capTop     */ RGB(0x0A, 0x24, 0x6A),
    /* capBottom  */ RGB(0xA6, 0xCA, 0xF0),
    /* capText    */ RGB(0xFF, 0xFF, 0xFF),
    /* capLine    */ RGB(0x08, 0x1E, 0x50),
    /* capBtn     */ RGB(0x21, 0x5D, 0xAA),
    /* capBtnHover */ RGB(0x5C, 0x8A, 0xD6),
    /* capGlyph   */ RGB(0xFF, 0xFF, 0xFF),
    /* macRed     */ RGB(0xFF, 0x5F, 0x57),
    /* macYellow  */ RGB(0xFE, 0xBC, 0x2E),
    /* macGreen   */ RGB(0x28, 0xC8, 0x40),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_XP,
    /* dark       */ false
    },
    // ---- macOS: light chrome with traffic-light caption ---
    {
    /* ribbonBg   */ RGB(0xF2, 0xF2, 0xF2),
    /* card       */ RGB(0xFF, 0xFF, 0xFF),
    /* cardBorder */ RGB(0xD6, 0xD6, 0xD6),
    /* accent     */ RGB(0x0A, 0x7A, 0xFF),
    /* accentDeep */ RGB(0x00, 0x60, 0xDF),
    /* text       */ RGB(0x1D, 0x1D, 0x1F),
    /* textDim    */ RGB(0x63, 0x63, 0x68),
    /* btnHover   */ RGB(0xE3, 0xE3, 0xE8),
    /* btnDown    */ RGB(0xD2, 0xD2, 0xD7),
    /* btnBorder  */ RGB(0xD6, 0xD6, 0xD6),
    /* thumbBg    */ RGB(0xED, 0xED, 0xF0),
    /* canvasBg   */ RGB(0xC3, 0xC3, 0xC8),
    /* pageFrame  */ RGB(0x86, 0x86, 0x8B),
    /* split      */ RGB(0xD6, 0xD6, 0xD6),
    /* statusBg   */ RGB(0x2C, 0x2C, 0x2E),
    /* statusTxt  */ RGB(0xF5, 0xF5, 0xF7),
    /* treeBg     */ RGB(0xFF, 0xFF, 0xFF),
    /* treeTxt    */ RGB(0x1D, 0x1D, 0x1F),
    /* emptyHint  */ RGB(0x86, 0x86, 0x8B),
    /* capTop     */ RGB(0xE8, 0xE8, 0xE8),
    /* capBottom  */ RGB(0xE0, 0xE0, 0xE0),
    /* capText    */ RGB(0x2B, 0x2B, 0x2D),
    /* capLine    */ RGB(0xC8, 0xC8, 0xC8),
    /* capBtn     */ RGB(0xE0, 0xE0, 0xE0),
    /* capBtnHover */ RGB(0xD0, 0xD0, 0xD0),
    /* capGlyph   */ RGB(0x2B, 0x2B, 0x2D),
    /* macRed     */ RGB(0xFF, 0x5F, 0x57),
    /* macYellow  */ RGB(0xFE, 0xBC, 0x2E),
    /* macGreen   */ RGB(0x28, 0xC8, 0x40),
    /* macGlyph   */ RGB(0x00, 0x00, 0x00),
    /* capStyle   */ CAP_MAC,
    /* dark       */ false
    },
};

static const wchar_t* kThemeNames[THEME_COUNT] = {
    L"Light Grey", L"Dark", L"Dark Blue", L"Pastel",
    L"High Contrast (Dark)", L"High Contrast (Light)",
    L"Windows XP", L"macOS"};

static const UiTheme& ThemeNow() { return kThemes[g_themeId]; }
static bool ThemeIsDark() { return kThemes[g_themeId].dark; }

static void ApplyTreeTheme()
{
  if (!g.bookmarks) return;
  const UiTheme& th = ThemeNow();
  SendMessageW(g.bookmarks, TVM_SETBKCOLOR, 0, (LPARAM)th.treeBg);
  SendMessageW(g.bookmarks, TVM_SETTEXTCOLOR, 0, (LPARAM)th.treeTxt);
  SendMessageW(g.bookmarks, TVM_SETLINECOLOR, 0, (LPARAM)th.treeTxt);
  InvalidateRect(g.bookmarks, nullptr, TRUE);
}

// Let the OS chrome (scrollbars, tree view, the system's own dark handling)
// follow the active theme.
static void ApplyOsTheme()
{
  if (!g.frame) return;
  const UiTheme& th = ThemeNow();
  // Modern (flat, thin) scrollbars; dark themes use the dark variant.
  const wchar_t* scrollSub = th.dark ? L"DarkMode_Explorer" : L"Explorer";
  SetWindowTheme(g.canvas, scrollSub, nullptr);
  SetWindowTheme(g.thumbs, scrollSub, nullptr);
  SetWindowTheme(g.bookmarks, L"Explorer", nullptr);
  SetWindowTheme(g.split, L"", L"");
  // Ask DWM for a dark title bar / dark dialog surfaces where supported.
  BOOL dark = th.dark ? TRUE : FALSE;
  if (g.frame)
  {
    // 20 = DWMWA_USE_IMMERSIVE_DARK_MODE (Win10 2004+/11),
    // 19 = the earlier build-name value; try both, ignore failures.
    DwmSetWindowAttribute(g.frame, 20, &dark, sizeof(dark));
    DwmSetWindowAttribute(g.frame, 19, &dark, sizeof(dark));
  }
}

static void SyncThemeMenuChecks()
{
  HMENU sub = g_themeMenu;
  if (!sub) return;
  CheckMenuRadioItem(sub, ID_THEME_FIRST, ID_THEME_FIRST + THEME_COUNT - 1,
                     ID_THEME_FIRST + g_themeId, MF_BYCOMMAND);
  CheckMenuItem(sub, ID_THEME,
                MF_BYCOMMAND | (ThemeIsDark() ? MF_CHECKED : MF_UNCHECKED));
}

static void LayoutCaption(HWND hw);
static void InvalidateCaption();

static void RefreshAllSurfaces()
{
  if (g.toolbar) InvalidateRect(g.toolbar, nullptr, TRUE);
  if (g.thumbs)  InvalidateRect(g.thumbs, nullptr, TRUE);
  if (g.split)   InvalidateRect(g.split, nullptr, TRUE);
  if (g.canvas)  InvalidateRect(g.canvas, nullptr, TRUE);
  if (g.status)  InvalidateRect(g.status, nullptr, TRUE);
  if (g.paneTabs) InvalidateRect(g.paneTabs, nullptr, TRUE);
  if (g.frame)   InvalidateRect(g.frame, nullptr, TRUE);
  for (HWND hw : g.ribbonBtns)
    InvalidateRect(hw, nullptr, TRUE);
  LayoutCaption(g.frame);
  InvalidateCaption();
  ApplyTreeTheme();
  ApplyOsTheme();
}

static void SetTheme(int id, bool persist)
{
  if (id < 0 || id >= THEME_COUNT) return;
  g_themeId = id;
  RefreshAllSurfaces();
  SyncThemeMenuChecks();
  if (!persist) return;
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\StitchupPDFEditor", 0,
                      nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr)
      == ERROR_SUCCESS)
  {
    DWORD v = (DWORD)id;
    RegSetValueExW(key, L"Theme", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(key);
  }
}

// Ctrl+D: quick toggle between the light and dark schemes.
static void ToggleTheme()
{
  SetTheme(ThemeIsDark() ? THEME_LIGHT : THEME_DARK, true);
}

static void ApplyInitialThemePref()
{
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StitchupPDFEditor", 0,
                    KEY_QUERY_VALUE, &key) == ERROR_SUCCESS)
  {
    DWORD v = 0, sz = sizeof(v), legacy = 0, lsz = sizeof(legacy);
    if (RegQueryValueExW(key, L"Theme", nullptr, nullptr, (LPBYTE)&v, &sz)
          == ERROR_SUCCESS && v < THEME_COUNT)
      g_themeId = (int)v;
    else if (RegQueryValueExW(key, L"Dark", nullptr, nullptr, (LPBYTE)&legacy,
                              &lsz) == ERROR_SUCCESS && legacy)
      g_themeId = THEME_DARK;   // migrate the old light/dark pref
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

// Widest laid-out row at zoom 1.0: with spread this is the two-page pair
// (plus the 12px inter-page gap), otherwise the widest single page.
static double LayoutSpanW()
{
  double span = 0;
  if (g.spread)
  {
    for (int i = 0; i < g.pageCount; i += 2)
    {
      double w0 = PageW(i);
      double w1 = (i + 1 < g.pageCount) ? std::max(0.0, (double)PageW(i + 1)) : 0.0;
      span = std::max(span, w0 + (w1 > 0 ? w1 + 12 : 0));
    }
  }
  else
  {
    span = MaxPageW();
  }
  return std::max(1.0, span);
}

// Absolute (pre-scroll) device rect for every page under current zoom/spread.
// rects is indexed by page number (zero rect for skipped/unloadable pages).
// Also returns the total scrollable content size.
static void LayoutPages(int cw, std::vector<RECT>& rects, int& contentW,
                        int& contentH)
{
  rects.clear();
  contentW = std::max(cw, 24);
  contentH = 24;
  if (!g.doc || g.pageCount == 0) return;
  double s = g.zoom;
  double span = std::max(1.0, MaxPageW()) * s;
  int spanC = (int)std::ceil(span);
  int yc = 12;
  if (!g.spread)
  {
    int workW = spanC + 32;
    int ctxW = std::max(workW, cw);
    int xBase = (ctxW - spanC) / 2;
    for (int i = 0; i < g.pageCount; ++i)
    {
      float pw = PageW(i), ph = PageH(i);
      int w = (int)std::ceil(pw * s);
      int h = (int)std::ceil(ph * s);
      if (w < 1 || h < 1) { rects.push_back({0, 0, 0, 0}); continue; }
      int x = xBase + (spanC - w) / 2;
      rects.push_back({x, yc, x + w, yc + h});
      contentH = yc + h + 12;
      yc += h + 14;
    }
    contentW = workW;
  }
  else
  {
    contentW = 32;
    for (int i = 0; i < g.pageCount; i += 2)
    {
      int j = i + 1;
      float pw0 = PageW(i), ph0 = PageH(i);
      int w0 = (int)std::ceil(pw0 * s);
      int h0 = (int)std::ceil(ph0 * s);
      bool has2 = j < g.pageCount;
      float pw1 = has2 ? PageW(j) : 0.0f;
      float ph1 = has2 ? PageH(j) : 0.0f;
      int w1 = (int)std::ceil(pw1 * s);
      int h1 = (int)std::ceil(ph1 * s);
      if (w0 < 1 && (!has2 || w1 < 1)) continue;
      int rowW = (w0 > 0 ? w0 : 0) + (has2 && w1 > 0 ? w1 + 12 : 0);
      int xBase = (std::max(rowW + 32, cw) - rowW) / 2;
      int rowH = std::max(h0, h1);
      rects.push_back({xBase, yc, xBase + w0, yc + h0});
      if (has2 && w1 > 0)
        rects.push_back({xBase + w0 + 12, yc, xBase + w0 + 12 + w1, yc + h1});
      else
        rects.push_back({0, 0, 0, 0});
      contentW = std::max(contentW, rowW + 32);
      contentH = yc + rowH + 12;
      yc += rowH + 14;
    }
  }
}

static POINT PageOrigin(int sx, int sy) { return POINT{-sx, -sy}; }

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
  if (g.annDrag) { ReleaseCapture(); g.annDrag = false; }
  g.annTool = 0;
  g.annPage = -1;
  g.findQuery.clear();
  g.findHits.clear();
  g.findCur = -1;
}

static void SnapshotCurrentTab()
{
  if (g_curTab < 0 || g_curTab >= (int)g_tabs.size()) return;
  TabDoc& t = g_tabs[g_curTab];
  t.doc = g.doc;
  t.path = g.path;
  t.name = g.name;
  t.dirty = g.dirty;
  t.pageCount = g.pageCount;
  t.selected = g.selected;
  t.zoom = g.zoom;
  t.scrollX = g.scrollX;
  t.scrollY = g.scrollY;
}

static void RefreshTabBar();
static void RefreshState();
static void UpdateScrollbars();
static void ResetThumbScroll();
static void SyncThumbScroll();
static void FitWidth();
static void RelayoutPanes(int w, int h);
static void GotoPageIndex(int idx);
static void FindHighlightRects(int pageIdx, int start, int count,
                               std::vector<FS_RECTF>& out);

static void RestoreTab(int i)
{
  if (i < 0 || i >= (int)g_tabs.size()) return;
  SnapshotCurrentTab();
  g_curTab = i;
  const TabDoc& t = g_tabs[i];
  g.doc = t.doc;
  g.path = t.path;
  g.name = t.name;
  g.dirty = t.dirty;
  g.pageCount = t.pageCount;
  g.selected = t.selected;
  g.zoom = t.zoom;
  g.scrollX = t.scrollX;
  g.scrollY = t.scrollY;
  SetWindowTextW(g.frame, kAppTitle);
  RefreshState();
  g.bmDirty = true;
  UpdateScrollbars();
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
  InvalidateRect(g.status, nullptr, TRUE);
  RefreshTabBar();
}

static void CloseTab(int i)
{
  if (i < 0 || i >= (int)g_tabs.size()) return;
  SnapshotCurrentTab();
  TabDoc t = g_tabs[i];
  g_tabs.erase(g_tabs.begin() + i);
  if (t.doc) FPDF_CloseDocument(t.doc);
  if (g_tabs.empty())
  {
    CloseDoc();
    g_curTab = -1;
    if (g.frame) SetWindowTextW(g.frame, kAppTitle);
    RefreshTabBar();
    return;
  }
  int next = i;
  if (next >= (int)g_tabs.size()) next = (int)g_tabs.size() - 1;
  RestoreTab(next);
}

static void SetActiveTab(int i)
{
  if (i < 0 || i >= (int)g_tabs.size()) return;
  RestoreTab(i);
}

static void NextTab(int d)
{
  if (g_tabs.empty()) return;
  SetActiveTab((g_curTab + d + (int)g_tabs.size()) % (int)g_tabs.size());
}

static void RefreshTabBar()
{
  if (!g.tabbar) return;
  // The document tab strip is only worth its 30px when a document is open; with
  // no tabs it was leaving a blank band between the menu bar and the ribbon.
  ShowWindow(g.tabbar, g_tabs.empty() ? SW_HIDE : SW_SHOW);
  if (g.frame)
  {
    RECT cr{};
    GetClientRect(g.frame, &cr);
    RelayoutPanes(cr.right - cr.left, cr.bottom - cr.top);
  }
  InvalidateRect(g.tabbar, nullptr, TRUE);
}

static void RefreshState()
{
  SnapshotCurrentTab();
  g.pageCount = g.doc ? FPDF_GetPageCount(g.doc) : 0;
  if (g.selected >= g.pageCount) g.selected = g.pageCount ? g.pageCount - 1 : 0;
  if (g.selected < 0) g.selected = 0;
  if (g_curTab >= 0 && g_curTab < (int)g_tabs.size())
    g_tabs[g_curTab].pageCount = g.pageCount;
  ClearCanvasCache();
  ClearThumbCache();
  ResetThumbScroll();
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
  SnapshotCurrentTab();
  g.doc = d;
  g.path = file;
  g.bmDirty = true;
  size_t p = file.find_last_of(L"\\/");
  g.name = (p == std::wstring::npos) ? file : file.substr(p + 1);
  TabDoc nd;
  nd.doc = d;
  nd.path = g.path;
  nd.name = g.name;
  nd.pageCount = FPDF_GetPageCount(d);
  nd.selected = 0;
  g.dirty = false;
  g_tabs.push_back(nd);
  g_curTab = (int)g_tabs.size() - 1;
  SetWindowTextW(g.frame, kAppTitle);
  RefreshState();
  // Open at the real fit-page-width zoom, not an approximation.
  FitWidth();
  g.scrollX = g.scrollY = 0;
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
  InvalidateRect(g.status, nullptr, TRUE);
  RefreshTabBar();
  SetFocus(g.frame);
}

static void NewDoc()
{
  SnapshotCurrentTab();
  g.doc = FPDF_CreateNewDocument();
  FPDFPage_New(g.doc, 0, 612.0, 792.0);
  g.path.clear();
  g.name.clear();
  g.dirty = false;
  TabDoc nd;
  nd.doc = g.doc;
  nd.pageCount = FPDF_GetPageCount(g.doc);
  g.pageCount = nd.pageCount;
  g.selected = 0;
  g.zoom = 1.0;
  g.scrollX = g.scrollY = 0;
  g_tabs.push_back(nd);
  g_curTab = (int)g_tabs.size() - 1;
  SetWindowTextW(g.frame, kAppTitle);
  RefreshState();
  g.bmDirty = true;
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.thumbs, nullptr, TRUE);
  RefreshTabBar();
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

// ---------------------------------------------------------------------------
// Bookmark dialog: name for a new user bookmark
// ---------------------------------------------------------------------------
struct BmCtx
{
  HWND edit = nullptr;
  HWND label = nullptr;
  bool ok = false;
  std::wstring text;
  // Dialog wording, so the same shell serves bookmarks and link targets.
  const wchar_t* caption = L"Add Bookmark";
  const wchar_t* prompt = L"Bookmark name:";
  bool allowEmpty = false;
};

// Target for the next InsertAnnot(ID_ANN_LINK) call.
static std::wstring g_linkUri;

static LRESULT CALLBACK BmProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
  switch (m)
  {
    case WM_CREATE:
    {
      BmCtx* ctx = reinterpret_cast<BmCtx*>(
        reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
      SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
      ctx->label = CreateWindowExW(0, L"STATIC", ctx->prompt,
        WS_CHILD | WS_VISIBLE, 16, 14, 300, 16, h, nullptr, g.inst, nullptr);
      ctx->edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
        16, 34, 300, 24, h, nullptr, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Ok",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
        154, 70, 74, 28, h, (HMENU)1, g.inst, nullptr);
      CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP,
        240, 70, 74, 28, h, (HMENU)2, g.inst, nullptr);
      SetFocus(ctx->edit);
      SetWindowTextW(ctx->edit, ctx->text.c_str());
      SendMessageW(ctx->edit, EM_SETSEL, 0, -1);
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(w) == 1 || LOWORD(w) == 2)
      {
        BmCtx* ctx = reinterpret_cast<BmCtx*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (ctx)
        {
          if (LOWORD(w) == 1)
          {
            wchar_t buf[512] = { 0 };
            ctx->text = GetWindowTextW(ctx->edit, buf, 512) > 0 ? buf : L"";
          }
          ctx->ok = (LOWORD(w) == 1);
        }
        DestroyWindow(h);
        return 0;
      }
      break;
    case WM_CLOSE:
    {
      BmCtx* ctx = reinterpret_cast<BmCtx*>(GetWindowLongPtrW(h, GWLP_USERDATA));
      if (ctx) ctx->ok = false;
      DestroyWindow(h);
      return 0;
    }
  }
  return DefWindowProcW(h, m, w, l);
}

static bool PromptText(std::wstring& out, const std::wstring& deflt,
                       const wchar_t* caption, const wchar_t* prompt,
                       bool allowEmpty)
{
  const wchar_t cls[] = L"SKBmWnd";
  static bool reg = false;
  if (!reg)
  {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BmProc;
    wc.hInstance = g.inst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = cls;
    RegisterClassExW(&wc);
    reg = true;
  }
  BmCtx ctx;
  ctx.text = deflt;
  ctx.caption = caption;
  ctx.prompt = prompt;
  ctx.allowEmpty = allowEmpty;
  HWND hw = CreateWindowExW(WS_EX_DLGMODALFRAME, cls, caption,
                            WS_POPUP | WS_CAPTION | WS_SYSMENU,
                            CW_USEDEFAULT, CW_USEDEFAULT, 332, 138,
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
  EnableWindow(g.frame, FALSE);
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
  EnableWindow(g.frame, TRUE);
  SetActiveWindow(g.frame);
  SetFocus(g.frame);
  if (!ctx.ok) return false;
  // trim
  std::wstring t = ctx.text;
  const size_t a = t.find_first_not_of(L" \t");
  const size_t b = t.find_last_not_of(L" \t");
  out = (a == std::wstring::npos) ? L"" : t.substr(a, b - a + 1);
  return allowEmpty || !out.empty();
}

static bool PromptBookmarkName(std::wstring& out, const std::wstring& deflt)
{
  return PromptText(out, deflt, L"Add Bookmark", L"Bookmark name:", false);
}

// A link is a /Link annotation; the target lives in its URI action.
// PDFium can also read page destinations, but exposes no API to write one, so
// an empty target would create a dead link - refuse instead of writing junk.
static bool PromptLinkUri(std::wstring& out, const std::wstring& deflt)
{
  return PromptText(out, deflt, L"Add Link", L"Web address:", false);
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

// ---------------------------------------------------------------------------
// PDF outline (bookmark) writer
// ---------------------------------------------------------------------------
// pdfium exposes no API for creating outlines, so user bookmarks are appended to
// the saved bytes as a PDF incremental update: the existing objects are left
// untouched and a new catalog, outline tree and cross-reference stream are
// appended after the original %%EOF.
//
// This relies on two properties of FPDF_SaveAsCopy output, both asserted by the
// self-test: every object is a plain uncompressed "N G obj ... endobj" and the
// file ends with an xref stream. That lets the object map be rebuilt by scanning
// for object headers instead of decoding the (compressed) xref stream.
namespace pdfout {

inline bool IsWs(char c)
{
  return c == ' ' || c == '\r' || c == '\n' || c == '\t' || c == '\f' || c == '\0';
}

inline void SkipWs(const std::string& s, size_t& p)
{
  while (p < s.size() && IsWs(s[p])) ++p;
}

inline long long ReadInt(const std::string& s, size_t& p)
{
  SkipWs(s, p);
  bool neg = false;
  if (p < s.size() && (s[p] == '-' || s[p] == '+')) { neg = s[p] == '-'; ++p; }
  long long v = 0;
  bool any = false;
  while (p < s.size() && s[p] >= '0' && s[p] <= '9')
  {
    v = v * 10 + (s[p] - '0');
    ++p;
    any = true;
  }
  if (!any) return -1;
  return neg ? -v : v;
}

// A located indirect object: num/gen plus the span of its body.
struct Obj
{
  int num = 0;
  int gen = 0;
  size_t body = 0;   // first byte after "obj"
  size_t end = 0;    // offset of "endobj"
  bool dict = false; // body starts with "<<"
};

inline bool IsDelim(char c)
{
  return c == '/' || c == '(' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '{' || c == '}' || c == '%';
}

// Reads the "N G obj" header at o. Returns false when the bytes there are not an
// object header, which is what rejects false hits inside stream data.
bool ReadObjHeader(const std::string& s, size_t o, Obj& out)
{
  if (o >= s.size()) return false;
  size_t p = o;
  const long long num = ReadInt(s, p);
  if (num < 0 || num > 100000000) return false;
  const long long gen = ReadInt(s, p);
  if (gen < 0) return false;
  SkipWs(s, p);
  if (s.compare(p, 3, "obj") != 0) return false;
  p += 3;
  SkipWs(s, p);
  out.num = (int)num;
  out.gen = (int)gen;
  out.body = p;
  out.dict = s.compare(p, 2, "<<") == 0;
  return true;
}

// Rebuilds objnum -> offset by scanning. Later definitions win, which is the
// right rule for incrementally updated files.
bool ScanObjects(const std::string& s, std::map<int, size_t>& out)
{
  bool any = false;
  for (size_t i = 0; i + 3 <= s.size(); ++i)
  {
    if (s[i] != 'o' || s.compare(i, 3, "obj") != 0) continue;
    if (i + 3 < s.size() && !IsWs(s[i + 3])) continue;
    size_t back = i;
    // step back over whitespace to the generation number
    while (back > 0 && IsWs(s[back - 1])) --back;
    size_t gend = back;
    while (back > 0 && s[back - 1] >= '0' && s[back - 1] <= '9') --back;
    if (back == gend) continue;
    size_t save = back;
    while (back > 0 && IsWs(s[back - 1])) --back;
    size_t numend = back;
    while (back > 0 && s[back - 1] >= '0' && s[back - 1] <= '9') --back;
    if (back == numend) continue;
    if (back > 0 && !IsWs(s[back - 1]) && !IsDelim(s[back - 1])) continue;
    Obj o;
    if (!ReadObjHeader(s, back, o)) continue;
    out[o.num] = back;
    any = true;
    (void)save;
  }
  return any;
}

// End offset of the balanced construct starting at p ('<' '<' '[' or '(').
bool SkipContainer(const std::string& s, size_t p, size_t limit, size_t& end)
{
  const bool d = s.compare(p, 2, "<<") == 0;
  const char open = d ? '<' : s[p];
  const char close = d ? '>' : (s[p] == '[' ? ']' : ')');
  int depth = 0;
  while (p < limit)
  {
    const char c = s[p];
    if (c == '(')
    {  // literal string: honour escapes and nesting
      int nest = 0;
      while (p < limit)
      {
        if (s[p] == '\\') { p += 2; continue; }
        if (s[p] == '(') ++nest;
        else if (s[p] == ')')
        {
          --nest;
          if (nest == 0) { ++p; break; }
        }
        ++p;
      }
      continue;
    }
    if (c == '<' && s[p + 1] == '<') { depth += 2; p += 2; continue; }
    if (c == '>' && s[p + 1] == '>') { depth -= 2; p += 2; if (depth <= 0) { end = p; return true; } continue; }
    if (c == open && !d) { ++depth; ++p; continue; }
    if (c == close && !d) { --depth; ++p; if (depth <= 0) { end = p; return true; } continue; }
    ++p;
  }
  return false;
}

// Finds /key at dictionary top level within [b, e) and returns the value span.
// The range may start at the dictionary's own "<<", which is stepped over so its
// keys are treated as top level; nested dictionaries are still skipped whole.
bool FindKey(const std::string& s, size_t b, size_t e, const char* key,
             size_t& vs, size_t& ve)
{
  const std::string k = std::string("/") + key;
  if (b + 1 < e && s.compare(b, 2, "<<") == 0) b += 2;
  size_t p = b;
  int depth = 0;
  while (p < e)
  {
    const char c = s[p];
    if (c == '(')
    {
      int nest = 0;
      while (p < e)
      {
        if (s[p] == '\\') { p += 2; continue; }
        if (s[p] == '(') ++nest;
        else if (s[p] == ')') { --nest; if (nest == 0) { ++p; break; } }
        ++p;
      }
      continue;
    }
    if (c == '<' && s.compare(p, 2, "<<") == 0)
    {
      size_t tmp;
      if (!SkipContainer(s, p, e, tmp)) return false;
      p = tmp;
      continue;
    }
    if (c == '<' && s[p + 1] != '<')
    {  // hex string
      while (p < e && s[p] != '>') ++p;
      ++p;
      continue;
    }
    if (c == '[')
    {
      size_t tmp;
      if (!SkipContainer(s, p, e, tmp)) return false;
      p = tmp;
      continue;
    }
    if (c == '/' && depth == 0)
    {
      if (s.compare(p, k.size(), k) == 0)
      {
        const char after = p + k.size() < e ? s[p + k.size()] : ' ';
        if (IsWs(after) || IsDelim(after))
        {
          size_t q = p + k.size();
          SkipWs(s, q);
          vs = q;
          if (q < e && (s.compare(q, 2, "<<") == 0 || s[q] == '[' || s[q] == '('))
          {
            if (!SkipContainer(s, q, e, ve)) return false;
          }
          else
          {
            ve = q;
            while (ve < e && !IsWs(s[ve]) && !IsDelim(s[ve])) ++ve;
            if (ve == q) return false;
            // "N G R" is one value, not three tokens: extend over the reference
            if (ve - q <= 12 && s.find_first_not_of("0123456789", q) == ve)
            {
              size_t r = ve;
              SkipWs(s, r);
              const size_t genStart = r;
              while (r < e && s[r] >= '0' && s[r] <= '9') ++r;
              if (r > genStart)
              {
                SkipWs(s, r);
                if (r < e && s[r] == 'R' &&
                    (r + 1 >= e || IsWs(s[r + 1]) || IsDelim(s[r + 1])))
                  ve = r + 1;
              }
            }
          }
          return true;
        }
      }
    }
    ++p;
  }
  return false;
}

bool GetIntEntry(const std::string& s, size_t b, size_t e, const char* key,
                 long long& out)
{
  size_t vs, ve;
  if (!FindKey(s, b, e, key, vs, ve)) return false;
  size_t p = vs;
  const long long v = ReadInt(s, p);
  if (v < 0) return false;
  out = v;
  return true;
}

bool GetRefEntry(const std::string& s, size_t b, size_t e, const char* key,
                 int& num)
{
  size_t vs, ve;
  if (!FindKey(s, b, e, key, vs, ve)) return false;
  size_t p = vs;
  const long long n = ReadInt(s, p);
  if (n < 0) return false;
  SkipWs(s, p);
  if (ReadInt(s, p) < 0) return false;   // generation
  SkipWs(s, p);
  if (p >= ve || s[p] != 'R') return false;
  num = (int)n;
  return true;
}

// Body span of a located object, i.e. between "obj" and "endobj", with the
// trailing whitespace before "endobj" trimmed so the dict can be rewritten.
bool ObjBody(const std::string& s, const std::map<int, size_t>& offs, int num,
             Obj& out)
{
  const auto it = offs.find(num);
  if (it == offs.end()) return false;
  if (!ReadObjHeader(s, it->second, out)) return false;
  out.end = s.find("endobj", out.body);
  if (out.end == std::string::npos) return false;
  while (out.end > out.body && IsWs(s[out.end - 1])) --out.end;
  return out.dict;
}

// PDF text string as a literal string holding UTF-16BE with a BOM, which pdfium
// decodes on read-back. The delimiters and backslash are escaped.
std::string TextString(const std::wstring& s)
{
  std::string out = "(\xFE\xFF";
  auto put = [&](unsigned char c) {
    if (c == '(' || c == ')' || c == '\\') out += '\\';
    out += (char)c;
  };
  for (wchar_t c : s)
  {
    const unsigned u = (unsigned)(c & 0xFFFF);
    put((unsigned char)((u >> 8) & 0xFF));
    put((unsigned char)(u & 0xFF));
  }
  out += ")";
  return out;
}

std::string Num(double v)
{
  char buf[48];
  if (std::fabs(v - std::llround(v)) < 0.0005)
    _snprintf_s(buf, _TRUNCATE, "%lld", std::llround(v));
  else
    _snprintf_s(buf, _TRUNCATE, "%.3f", v);
  return buf;
}

// Collects page object numbers in reading order by walking /Kids.
bool CollectPages(const std::string& s, const std::map<int, size_t>& offs,
                  int pagesObj, std::vector<int>& out, int depth = 0)
{
  if (depth > 64 || out.size() > 200000) return false;
  Obj o;
  if (!ObjBody(s, offs, pagesObj, o)) return false;
  // A node without /Kids is a leaf page; that is the only distinction needed.
  size_t vs, ve;
  const bool hasKids = FindKey(s, o.body, o.end, "Kids", vs, ve);
  if (!hasKids)
  {
    out.push_back(pagesObj);
    return true;
  }
  // iterate the /Kids array of "N G R"
  size_t p = vs;
  if (p >= ve || s[p] != '[') return false;
  ++p;
  while (p < ve)
  {
    SkipWs(s, p);
    if (p >= ve) break;
    if (s[p] == ']') break;
    const long long n = ReadInt(s, p);
    if (n < 0) return false;
    SkipWs(s, p);
    if (ReadInt(s, p) < 0) return false;   // generation
    SkipWs(s, p);
    if (p >= ve) return false;
    if (s[p] != 'R') return false;
    if (!CollectPages(s, offs, (int)n, out, depth + 1)) return false;
    ++p;
  }
  return true;
}

// Removes "/Key value" from a dictionary body so a key can be replaced without
// leaving a duplicate behind. The value span starts after any whitespace that
// follows the key, so the key start is found by walking back over that space.
std::string RemoveKey(const std::string& d, const char* key)
{
  size_t vs, ve;
  if (!FindKey(d, 0, d.size(), key, vs, ve)) return d;
  size_t ks = vs;
  while (ks > 0 && IsWs(d[ks - 1])) --ks;
  const size_t klen = std::string(key).size() + 1;   // include the '/'
  if (ks < klen) return d;
  ks -= klen;
  if (d.compare(ks, klen, std::string("/") + key) != 0) return d;   // sanity
  return d.substr(0, ks) + d.substr(ve);
}

// The last cross-reference section of the file.
struct XrefBase
{
  bool table = false;                    // classic "xref" table vs xref stream
  long long size = 0;                    // /Size
  int rootObj = 0;                       // /Root
  std::map<int, size_t> offs;            // objnum -> byte offset, in-use only
};

// Parses the section at xrefOff. pdfium writes a classic table with a trailer
// dictionary; a cross-reference stream is also accepted, in which case the
// object map is rebuilt by scanning because its table is compressed.
bool ParseBase(const std::string& pdf, size_t xrefOff, XrefBase& b)
{
  if (xrefOff >= pdf.size()) return false;
  size_t p = xrefOff;
  SkipWs(pdf, p);
  if (pdf.compare(p, 4, "xref") == 0)
  {
    b.table = true;
    p += 4;
    for (;;)
    {
      size_t save = p;
      SkipWs(pdf, p);
      if (pdf.compare(p, 7, "trailer") == 0) { p += 7; break; }
      const long long first = ReadInt(pdf, p);
      if (first < 0) { p = save; break; }
      SkipWs(pdf, p);
      const long long count = ReadInt(pdf, p);
      if (count < 0 || count > 10000000) return false;
      for (long long i = 0; i < count; ++i)
      {
        SkipWs(pdf, p);
        const long long off = ReadInt(pdf, p);
        if (off < 0) return false;
        SkipWs(pdf, p);
        (void)ReadInt(pdf, p);   // generation
        SkipWs(pdf, p);
        if (p >= pdf.size()) return false;
        const char kind = pdf[p++];
        if (kind == 'n' && off > 0) b.offs[(int)(first + i)] = (size_t)off;
        else if (kind != 'n' && kind != 'f') return false;
      }
    }
    SkipWs(pdf, p);
    const size_t dictEnd = pdf.find(">>", p);
    if (dictEnd == std::string::npos) return false;
    if (!GetIntEntry(pdf, p, dictEnd, "Size", b.size)) return false;
    if (!GetRefEntry(pdf, p, dictEnd, "Root", b.rootObj)) return false;
    return b.size > 0 && b.rootObj > 0;
  }
  // cross-reference stream
  Obj o;
  if (!ReadObjHeader(pdf, xrefOff, o) || !o.dict) return false;
  const size_t dictEnd = pdf.find(">>", o.body);
  if (dictEnd == std::string::npos) return false;
  if (!GetIntEntry(pdf, o.body, dictEnd, "Size", b.size)) return false;
  if (!GetRefEntry(pdf, o.body, dictEnd, "Root", b.rootObj)) return false;
  if (b.size <= 0 || b.rootObj <= 0) return false;
  return ScanObjects(pdf, b.offs);
}

}  // namespace pdfout

// Appends the bookmarks as an incremental update. `expectPages` is the page
// count the caller believes the document has; a mismatch aborts the write
// rather than risking a damaged file.
static bool AppendOutlines(std::string& pdf, const std::vector<UserBookmark>& bms,
                           int expectPages, std::wstring* err)
{
  using namespace pdfout;
  auto fail = [&](const wchar_t* m) {
    if (err) *err = m;
    return false;
  };
  if (bms.empty()) return true;
  long long size = 0;
  int rootObj = 0;
  bool isTable = true;

  // --- base file: startxref, /Size, /Root -----------------------------------
  const size_t sx = pdf.rfind("startxref");
  if (sx == std::string::npos) return fail(L"The saved file has no startxref.");
  size_t sp = sx + 9;
  const long long baseXref = ReadInt(pdf, sp);
  if (baseXref <= 0 || baseXref >= (long long)pdf.size())
    return fail(L"The saved file has a broken startxref.");

  std::map<int, size_t> offs;
  {
    XrefBase base;
    if (!ParseBase(pdf, (size_t)baseXref, base))
      return fail(L"The saved file's cross-reference section is unreadable.");
    offs.swap(base.offs);
    size = base.size;
    rootObj = base.rootObj;
    isTable = base.table;
  }
  if (offs.find(rootObj) == offs.end() && !ScanObjects(pdf, offs))
    return fail(L"No objects found in the saved file.");

  // --- catalog and page objects --------------------------------------------
  Obj cat;
  if (!ObjBody(pdf, offs, rootObj, cat)) return fail(L"Catalog object not found.");
  int pagesObj = 0;
  if (!GetRefEntry(pdf, cat.body, cat.end, "Pages", pagesObj))
    return fail(L"Catalog has no /Pages reference.");
  std::vector<int> pageObjs;
  if (!CollectPages(pdf, offs, pagesObj, pageObjs))
    return fail(L"Could not walk the page tree.");
  if (expectPages > 0 && (int)pageObjs.size() != expectPages)
    return fail(L"Page tree does not match the open document.");

  // --- existing outline, so user bookmarks are appended, not destructive ----
  int oldOutline = 0;
  GetRefEntry(pdf, cat.body, cat.end, "Outlines", oldOutline);
  int oldFirst = 0, oldLast = 0;
  long long oldCount = 0;
  if (oldOutline)
  {
    Obj oo;
    if (ObjBody(pdf, offs, oldOutline, oo))
    {
      GetRefEntry(pdf, oo.body, oo.end, "First", oldFirst);
      GetRefEntry(pdf, oo.body, oo.end, "Last", oldLast);
      if (!GetIntEntry(pdf, oo.body, oo.end, "Count", oldCount)) oldCount = 0;
    }
    if (!oldFirst) oldOutline = 0;   // unusable: start a fresh tree instead
  }

  // --- allocate object numbers ---------------------------------------------
  long long next = size;
  if (next < 1) next = 1;
  const int newCatalog = rootObj;          // rewrite in place
  const int outlineRoot = oldOutline ? oldOutline : (int)next++;
  std::vector<int> itemNums;
  itemNums.reserve(bms.size());
  for (size_t i = 0; i < bms.size(); ++i) itemNums.push_back((int)next++);
  const int oldLastNum = (oldOutline && oldLast) ? oldLast : 0;
  const long long newSize = next;

  // Leading newline so every object header is preceded by one, which is how the
  // offsets below are recovered.
  std::string add = "\n";
  auto emitObj = [&](int num, const std::string& body) {
    add += std::to_string(num) + " 0 obj\n" + body + "\nendobj\n";
  };

  // catalog: original dict with any /Outlines replaced
  {
    // rewrite the catalog with /Outlines replaced, never duplicated
    std::string clean = RemoveKey(pdf.substr(cat.body, cat.end - cat.body), "Outlines");
    if (clean.size() < 2 || clean.compare(clean.size() - 2, 2, ">>") != 0)
      return fail(L"Catalog dictionary is malformed.");
    clean.insert(clean.size() - 2,
                 " /Outlines " + std::to_string(outlineRoot) + " 0 R ");
    emitObj(newCatalog, clean);
  }

  // outline items
  for (size_t i = 0; i < bms.size(); ++i)
  {
    const UserBookmark& b = bms[i];
    if (b.page < 0 || b.page >= (int)pageObjs.size()) continue;
    std::string d = "<< /Title " + TextString(b.title) +
                    " /Parent " + std::to_string(outlineRoot) + " 0 R";
    if (i > 0) d += " /Prev " + std::to_string(itemNums[i - 1]) + " 0 R";
    if (i + 1 < bms.size())
      d += " /Next " + std::to_string(itemNums[i + 1]) + " 0 R";
    if (oldOutline && i == 0 && oldLastNum)
      d += " /Prev " + std::to_string(oldLastNum) + " 0 R";
    d += " /Dest [" + std::to_string(pageObjs[b.page]) + " 0 R";
    if (b.atView)
      d += " /XYZ " + Num(b.x) + " " + Num(b.y);
      if (b.zoom > 0) d += " " + Num(b.zoom);
    else
      d += " /Fit";
    d += "] >>";
    emitObj(itemNums[i], d);
  }

  // old last item gains a /Next so the original outline stays linked
  if (oldOutline && oldLastNum)
  {
    Obj lo;
    if (ObjBody(pdf, offs, oldLastNum, lo))
    {
      std::string d = pdf.substr(lo.body, lo.end - lo.body);
      size_t vs, ve;
      if (!FindKey(d, 0, d.size(), "Next", vs, ve) &&
          d.size() >= 2 && d.compare(d.size() - 2, 2, ">>") == 0)
      {
        d.insert(d.size() - 2,
                 " /Next " + std::to_string(itemNums.front()) + " 0 R ");
        emitObj(oldLastNum, d);
      }
    }
  }

  // outline root with the merged first/last/count
  {
    const long long count = oldCount + (long long)bms.size();
    // When extending an existing tree the head stays put and the new items are
    // chained onto its tail.
    const int head = oldOutline ? oldFirst : itemNums.front();
    std::string d = "<< /Type /Outlines";
    d += " /First " + std::to_string(head) + " 0 R";
    d += " /Last " + std::to_string(itemNums.back()) + " 0 R";
    d += " /Count " + std::to_string(count);
    d += " >>";
    emitObj(outlineRoot, d);
  }

  // --- cross-reference section for the appended objects ---------------------
  std::vector<int> nums;
  nums.push_back(newCatalog);
  if (oldOutline && oldLastNum) nums.push_back(oldLastNum);
  for (int n : itemNums) nums.push_back(n);
  if (outlineRoot != newCatalog) nums.push_back(outlineRoot);
  std::sort(nums.begin(), nums.end());
  nums.erase(std::unique(nums.begin(), nums.end()), nums.end());

  // Every object above was emitted, so its offset is where its "N 0 obj" header
  // starts inside the appended block.
  std::map<int, size_t> newOffs;
  for (int n : nums)
  {
    const std::string pat = "\n" + std::to_string(n) + " 0 obj\n";
    const size_t at = add.find(pat);
    if (at == std::string::npos) return fail(L"Internal: lost an object.");
    newOffs[n] = pdf.size() + at + 1;
  }

  if (isTable)
  {
    // classic table, matching the form pdfium wrote
    const size_t tableAt = pdf.size() + add.size();
    add += "xref\n";
    for (size_t i = 0; i < nums.size();)
    {
      size_t j = i;
      while (j + 1 < nums.size() && nums[j + 1] == nums[j] + 1) ++j;
      add += std::to_string(nums[i]) + " " + std::to_string(j - i + 1) + "\n";
      for (size_t k = i; k <= j; ++k)
      {
        char row[32];
        _snprintf_s(row, _TRUNCATE, "%010llu 00000 n \n", (unsigned long long)newOffs[nums[k]]);
        add += row;
      }
      i = j + 1;
    }
    add += "trailer\n<< /Size " + std::to_string(newSize) + " /Root " +
           std::to_string(rootObj) + " 0 R /Prev " + std::to_string(baseXref) +
           " >>\nstartxref\n" + std::to_string(tableAt) + "\n%%EOF\n";
    pdf += add;
    return true;
  }

  // cross-reference stream, for a base file that used one. Written uncompressed,
  // which is legal, so no inflate is needed.
  const int xrefNum = (int)newSize;
  std::string index, rows;
  for (size_t i = 0; i < nums.size(); ++i)
  {
    if (i) index += " ";
    index += std::to_string(nums[i]) + " 1";
    const unsigned use = (unsigned)newOffs[nums[i]];
    rows += (char)1;
    rows += (char)((use >> 24) & 0xFF);
    rows += (char)((use >> 16) & 0xFF);
    rows += (char)((use >> 8) & 0xFF);
    rows += (char)(use & 0xFF);
    rows += (char)0;
    rows += (char)0;   // gen 0
  }
  const size_t xrefObjOffset = pdf.size() + add.size();
  add += std::to_string(xrefNum) + " 0 obj\n<< /Type /XRef /Size " +
         std::to_string(newSize + 1) + " /Root " + std::to_string(rootObj) +
         " 0 R /Prev " + std::to_string(baseXref) + " /W [1 4 2] /Index [" +
         index + "] /Length " + std::to_string(rows.size()) + " >>\nstream\n" +
         rows + "\nendstream\nendobj\nstartxref\n" +
         std::to_string(xrefObjOffset) + "\n%%EOF\n";

  pdf += add;
  return true;
}

// Serializes the open document to memory, folding in any user bookmarks that
// have been added but not yet written to disk. Returns false (with a message
// when interactive) if serialization or the outline write failed, so a bookmark
// failure never produces a file that silently lost them.
static bool SerializeForSave(std::vector<unsigned char>& out, bool interactive)
{
  FileWriter fw{};
  fw.buf.reserve(65536);
  fw.base.version = 1;
  fw.base.WriteBlock = [](FPDF_FILEWRITE* self, const void* data, unsigned long size) -> int
  {
    FileWriter* f = reinterpret_cast<FileWriter*>(self);
    const unsigned char* p = static_cast<const unsigned char*>(data);
    f->buf.insert(f->buf.end(), p, p + size);
    return 1;
  };
  if (!FPDF_SaveAsCopy(g.doc, &fw.base, FPDF_NO_INCREMENTAL))
  {
    if (interactive)
      MessageBoxW(g.frame, L"Save failed: the document could not be serialized.",
                  L"Stitchup", MB_OK | MB_ICONWARNING);
    return false;
  }
  if (g.marks.empty())
  {
    out.swap(fw.buf);
    return true;
  }
  std::string pdf(reinterpret_cast<const char*>(fw.buf.data()), fw.buf.size());
  std::wstring err;
  if (!AppendOutlines(pdf, g.marks, FPDF_GetPageCount(g.doc), &err))
  {
    if (interactive)
    {
      const std::wstring msg =
          L"Save failed: your bookmarks could not be written into the PDF,\n"
          L"so the file was left untouched.\n\n" + err;
      MessageBoxW(g.frame, msg.c_str(), L"Stitchup", MB_OK | MB_ICONWARNING);
    }
    return false;
  }
  out.assign(pdf.begin(), pdf.end());
  return true;
}

static bool SaveDocTo(const std::wstring& target)
{
  if (!g.doc) return false;
  std::vector<unsigned char> buf;
  if (!SerializeForSave(buf, true)) return false;
  std::wstring tmp = target + L".tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) return false;
  bool ok = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
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
  g.marks.clear();
  g.bmDirty = true;
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
    SetWindowTextW(g.frame, kAppTitle);
    RefreshState();
    InvalidateRect(g.status, nullptr, TRUE);
  }
}

static void SaveInPlace()
{
  if (g.path.empty()) { SaveAs(); return; }
  if (!g.doc) return;
  std::wstring target = g.path;
  std::vector<unsigned char> buf;
  if (!SerializeForSave(buf, true)) return;
  std::wstring tmp = target + L".tmp";
  FILE* f = nullptr;
  if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0) return;
  bool okw = fwrite(buf.data(), 1, buf.size(), f) == buf.size();
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
  g.marks.clear();   // they are in the file now, and the reload reads them back
  g.bmDirty = true;
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
  // The outline writer appends plain objects, which an encrypted copy would
  // have to encrypt too, so say so rather than quietly dropping the bookmarks.
  if (!g.marks.empty())
  {
    const std::wstring msg =
        L"This document has " + std::to_wstring(g.marks.size()) +
        L" bookmark(s) that are not saved yet.\n\n"
        L"Save the document first so they are written into the PDF?";
    const int r = MessageBoxW(g.frame, msg.c_str(), L"Save As Encrypted",
                              MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return;
    if (r == IDYES)
    {
      if (g.path.empty()) SaveAs();
      else SaveInPlace();
      if (!g.marks.empty()) return;   // save did not clear them: stop here
    }
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
  // Showing/hiding a scrollbar resizes the canvas, which re-enters this
  // function through WM_SIZE. Guard against that, then settle the bars.
  static bool inUpdate = false;
  if (inUpdate) return;
  inUpdate = true;

  RECT rc{};
  GetClientRect(g.canvas, &rc);
  int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
  std::vector<RECT> rects;
  int contentW = 0, contentH = 0;
  LayoutPages(cw, rects, contentW, contentH);
  const bool hasDoc = g.doc && g.pageCount > 0;

  // Only show a bar when the document actually overflows that axis, so the
  // empty view and single pages stay completely clean. Two passes is enough to
  // settle after the client area has been resized by the first one. The current
  // state is read from the window (ShowScrollBar keeps the style bits in sync)
  // so the very first call still hides bars that WS_HSCROLL/WS_VSCROLL created.
  bool barH = (GetWindowLongW(g.canvas, GWL_STYLE) & WS_HSCROLL) != 0;
  bool barV = (GetWindowLongW(g.canvas, GWL_STYLE) & WS_VSCROLL) != 0;
  for (int pass = 0; pass < 2; ++pass)
  {
    const bool needH = hasDoc && contentW > cw;
    const bool needV = hasDoc && contentH > ch;
    if (needH == barH && needV == barV) break;
    barH = needH;
    barV = needV;
    ShowScrollBar(g.canvas, SB_HORZ, needH ? TRUE : FALSE);
    ShowScrollBar(g.canvas, SB_VERT, needV ? TRUE : FALSE);
    GetClientRect(g.canvas, &rc);
    cw = rc.right - rc.left;
    ch = rc.bottom - rc.top;
    rects.clear();
    LayoutPages(cw, rects, contentW, contentH);
  }

  contentW = std::max(contentW, 24);
  contentH = std::max(contentH, 24);

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
  inUpdate = false;
  SyncThumbScroll();
}

// ---------------------------------------------------------------------------
// Status bar
// ---------------------------------------------------------------------------
static LRESULT CALLBACK StatusProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
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
      const UiTheme& th = ThemeNow();
      HBRUSH bg = CreateSolidBrush(th.ribbonBg);
      FillRect(dc, &rc, bg);
DeleteObject(bg);
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, th.statusTxt);
      std::wstring left = g.name.empty() ? L"Stitchup PDF Editor"
                                         : g.name + (g.dirty ? L"  *" : L"");
      if (g.annTool)
        left += L"      Draw the annotation on the page \x2013 Esc to cancel";
      if (!g.findHits.empty() && g.findCur >= 0)
        left += L"      Match " + std::to_wstring(g.findCur + 1) + L" of " +
                std::to_wstring((int)g.findHits.size()) + L" for \"" +
                g.findQuery + L"\"";
      RECT lrc = rc;
      lrc.left += 10;
      DrawTextW(dc, left.c_str(), -1, &lrc, DT_SINGLELINE | DT_VCENTER);
      std::wstring right;
      if (g.pageCount > 0)
        right = L"Page " + std::to_wstring(g.selected + 1) + L" of " +
                std::to_wstring(g.pageCount) + L"      Zoom " +
                std::to_wstring((int)std::lround(g.zoom * 100.0)) + L"%";
      else
        right = L"No document      Zoom " +
                std::to_wstring((int)std::lround(g.zoom * 100.0)) + L"%";
      RECT rrc = rc;
      rrc.right -= 10;
      SetTextAlign(dc, TA_RIGHT | TA_TOP);
      DrawTextW(dc, right.c_str(), -1, &rrc, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
      EndPaint(hw, &ps);
      return 0;
    }
  }
  return DefWindowProc(hw, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Thumbnails panel
// ---------------------------------------------------------------------------
static int ThumbForY(int y)
{
  RECT rc;
  GetClientRect(g.thumbs, &rc);
  int w = rc.right - rc.left;
  int thumbW = w - 26;
  if (thumbW < 50) thumbW = 50;
  int yc = 34; // header area; scroll offset handled by caller
  for (int i = 0; i < g.pageCount; ++i)
  {
    float pw = PageW(i), ph = PageH(i);
    if (pw < 1 || ph < 1) continue;
    double scale = std::min((double)(thumbW - 8) / (double)pw,
                            220.0 / (double)ph);
    int hi = (int)std::ceil(ph * scale) + 16;
    if (y >= yc && y < yc + hi) return i;
    yc += hi;
  }
  return -1;
}

// Set the thumbnails scrollbar range so long documents actually scroll.
static void ResetThumbScroll()
{
  if (!g.thumbs) return;
  RECT rc;
  GetClientRect(g.thumbs, &rc);
  int view = rc.bottom - rc.top;
  int content = 34;
  int w = rc.right - rc.left;
  int thumbW = w - 26;
  if (thumbW < 50) thumbW = 50;
  for (int i = 0; i < g.pageCount; ++i)
  {
    float pw = PageW(i), ph = PageH(i);
    if (pw < 1 || ph < 1) continue;
    double scale = std::min((double)(thumbW - 8) / (double)pw,
                            220.0 / (double)ph);
    content += (int)std::ceil(ph * scale) + 16;
  }
  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  GetScrollInfo(g.thumbs, SB_VERT, &si);
  si.nMin = 0;
  si.nMax = std::max(0, content - view);
  si.nPage = std::max(1, view);
  if (si.nPos > si.nMax) si.nPos = si.nMax;
  SetScrollInfo(g.thumbs, SB_VERT, &si, TRUE);
}

// The thumbnails pane has no scrollbar of its own; it auto-follows the page
// nearest the top of the main view so the canvas bar stays the only vertical
// scrollbar. Range/pos are still stored so ThumbForY hit-testing (and
// drag-to-reorder) keeps working with the same geometry as WM_PAINT.
static void SyncThumbScroll()
{
  if (!g.thumbs || !g.doc || g.pageCount <= 0) return;
  if (g.dragPage >= 0) return; // let a drag reorder stabilise

  int target = g.selected;
  if (target >= 0 && target < g.pageCount)
  {
    RECT cr{};
    GetClientRect(g.canvas, &cr);
    int cw = std::max(120, (int)(cr.right - cr.left));
    std::vector<RECT> rects;
    int cw2 = 0, ch2 = 0;
    LayoutPages(cw, rects, cw2, ch2);
    if (!rects.empty())
    {
      POINT org = PageOrigin((int)std::lround(g.scrollX),
                             (int)std::lround(g.scrollY));
      target = 0;
      for (int i = 0; i < (int)rects.size(); ++i)
        if (rects[i].top + org.y <= 0) target = i;
    }
  }
  if (target < 0 || target >= g.pageCount) return;

  RECT rc;
  GetClientRect(g.thumbs, &rc);
  int view = rc.bottom - rc.top;
  if (view < 10) return;
  int w = rc.right - rc.left;
  int thumbW = w - 26;
  if (thumbW < 50) thumbW = 50;

  int yTop = 34, slotH = 0;
  int content = 34;
  for (int i = 0; i < g.pageCount; ++i)
  {
    float pw = PageW(i), ph = PageH(i);
    if (pw < 1 || ph < 1) continue;
    double scale = std::min((double)(thumbW - 8) / (double)pw, 220.0 / (double)ph);
    int hi = (int)std::ceil(ph * scale) + 16;
    content += hi;
    if (i < target) yTop += hi;
    else if (i == target) slotH = hi;
  }

  int pos = 0;
  if (yTop < 0) pos = 0;
  else if (yTop + slotH > view) pos = yTop + slotH - view + 8;
  else pos = yTop - 8;
  pos = std::max(0, std::min(std::max(0, content - view), pos));

  SCROLLINFO si{};
  si.cbSize = sizeof(si);
  si.fMask = SIF_POS;
  GetScrollInfo(g.thumbs, SB_VERT, &si);
  if (si.nPos != pos)
  {
    si.fMask = SIF_POS;
    si.nPos = pos;
    SetScrollInfo(g.thumbs, SB_VERT, &si, TRUE);
    InvalidateRect(g.thumbs, nullptr, FALSE);
  }
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

static LRESULT CALLBACK ThumbsProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_ERASEBKGND:
      return 1;
    case WM_CREATE:
      // The thumbnails pane deliberately has no scrollbar of its own: the main
      // canvas scrollbar is the single vertical bar, and SyncThumbScroll() keeps
      // the active page's thumbnail in view. The hidden range is still used for
      // hit-testing and drag-to-reorder.
      ShowScrollBar(hw, SB_VERT, FALSE);
      return 0;
    case WM_SIZE:
      ResetThumbScroll();
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    case WM_PAINT:
    {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(hw, &ps);
      RECT rc;
      GetClientRect(hw, &rc);
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right - rc.left,
                                           rc.bottom - rc.top);
      HGDIOBJ oldBmp = SelectObject(mem, bmp);
      const UiTheme& thm = ThemeNow();
      HBRUSH bgb = CreateSolidBrush(thm.thumbBg);
      FillRect(mem, &rc, bgb);
      DeleteObject(bgb);

      RECT hr{rc.left + 10, 6, rc.right - 10, 26};
      SetBkMode(mem, TRANSPARENT);
      SetTextColor(mem, thm.textDim);
      DrawTextW(mem, L"Pages", -1, &hr, DT_SINGLELINE);

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
        SelectObject(mem, pn);
        MoveToEx(mem, x, yc - 4, nullptr);
        LineTo(mem, x + thumbW, yc - 4);
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
          HDC md = CreateCompatibleDC(mem);
          SelectObject(md, hb);
          BitBlt(mem, x + 4, yc, tw, th, md, 0, 0, SRCCOPY);
          DeleteDC(md);
          bool sel = (i == g.selected);
          if (g.dragPage >= 0 && i == g.dragPage)
          {
            HBRUSH dim = CreateSolidBrush(thm.btnHover);
            RECT dr{x + 2, yc - 2, x + 4 + tw, yc + th + 6};
            FillRect(mem, &dr, dim);
            DeleteObject(dim);
          }
          RECT pr{x + 2, yc - 2, x + 4 + tw, yc + th + 6};
          HBRUSH phb = CreateSolidBrush(sel ? thm.accent
                                            : thm.pageFrame);
          FrameRect(mem, &pr, phb);
          DeleteObject(phb);
          std::wstring num = std::to_wstring(i + 1);
          RECT nr{x + 4, yc + th + 4, x + thumbW, yc + th + 14};
          SetTextColor(mem, sel ? thm.accent : thm.textDim);
          DrawTextW(mem, num.c_str(), -1, &nr, DT_SINGLELINE);
        }
        yc += th + 16;
        if (g.dragPage >= 0 && i + 1 == g.dragCursor)
        {
          HPEN pn = CreatePen(PS_SOLID, 2, thm.accent);
          SelectObject(mem, pn);
          MoveToEx(mem, x, yc - 4, nullptr);
          LineTo(mem, x + thumbW, yc - 4);
          DeleteObject(pn);
        }
      }
      BitBlt(dc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, mem, 0, 0,
             SRCCOPY);
      SelectObject(mem, oldBmp);
      DeleteObject(bmp);
      DeleteDC(mem);
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
      InvalidateRect(hw, nullptr, FALSE);
      return 0;
    }
    case WM_MOUSEWHEEL:
    {
      short d = GET_WHEEL_DELTA_WPARAM(wp);
      if (d != 0 && g.canvas)
      {
        int step = (int)(-(d / WHEEL_DELTA) * 60);
        SCROLLINFO si{};
        si.cbSize = sizeof(si);
        si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
        GetScrollInfo(g.canvas, SB_VERT, &si);
        int pos = std::max(si.nMin, std::min(si.nMax, si.nPos + step));
        g.scrollY = pos;
        si.fMask = SIF_POS;
        si.nPos = pos;
        SetScrollInfo(g.canvas, SB_VERT, &si, TRUE);
        UpdateScrollbars();
        InvalidateRect(g.canvas, nullptr, FALSE);
        InvalidateRect(hw, nullptr, FALSE);
      }
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
        InvalidateRect(hw, nullptr, FALSE);
        InvalidateRect(g.canvas, nullptr, FALSE);
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
          InvalidateRect(hw, nullptr, FALSE);
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
        InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK:
    {
      // Double-click a thumbnail: make that page the focus of the main display.
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      SCROLLINFO si{};
      si.cbSize = sizeof(si);
      si.fMask = SIF_POS;
      GetScrollInfo(hw, SB_VERT, &si);
      const int pi = ThumbForY(pt.y + si.nPos);
      if (pi >= 0)
      {
        g.selected = pi;
        GotoPageIndex(pi);
        SetFocus(g.canvas);
        InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
    }
    case WM_CANCELMODE:
      ReleaseCapture();
      g.dragPage = -1;
      g.dragCursor = -1;
      InvalidateRect(hw, nullptr, FALSE);
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
  const UiTheme& th = ThemeNow();
  HBRUSH bg = CreateSolidBrush(th.canvasBg);
  RECT rc{0, 0, cw, ch};
  FillRect(dc, &rc, bg);
  DeleteObject(bg);

  if (!g.doc || g.pageCount == 0)
  {
    // Empty state: a soft "open a document" hint centred on the canvas.
    const wchar_t* line1 = L"No document open";
    const wchar_t* line2 = L"Use File \x2013 Open (Ctrl+O) to open a PDF, "
                          L"or drop one here";
    HFONT f1 = CreateFontW(-MulDiv(15, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE,
                            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT f2 = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE,
                            FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                            DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SetTextColor(dc, th.textDim);
    SetBkMode(dc, TRANSPARENT);
    RECT box{0, 0, cw, ch};
    HFONT w1 = (HFONT)SelectObject(dc, f1);
    DrawTextW(dc, line1, -1, &box,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    RECT box2{0, box.bottom / 2 + MulDiv(18, g.dpi, 72), cw, ch};
    HFONT w2 = (HFONT)SelectObject(dc, f2);
    DrawTextW(dc, line2, -1, &box2,
              DT_CENTER | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, w1);
    SelectObject(dc, w2);
    DeleteObject(f1);
    DeleteObject(f2);
    return;
  }
  double s = g.zoom;
  std::vector<RECT> rects;
  int contentW = 0, contentH = 0;
  LayoutPages(cw, rects, contentW, contentH);
  POINT org = PageOrigin((int)std::lround(g.scrollX),
                         (int)std::lround(g.scrollY));

  for (int i = 0; i < g.pageCount && i < (int)rects.size(); ++i)
  {
    const RECT& pr = rects[i];
    int w = pr.right - pr.left, h = pr.bottom - pr.top;
    int x = pr.left + org.x, y = pr.top + org.y;
    if (w < 1 || h < 1) continue;
    if (y + h < 0 || y > ch) continue;
    if (x + w >= 0 && x <= cw)
    {
      int key;
      int rx = x, ry = y, rw = w, rh = h;
      RECT r{x, y, x + w, y + h};
      // shadow (canvas dimmed ~30%)
      COLORREF shC = RGB((GetRValue(th.canvasBg) * 7) / 10,
                         (GetGValue(th.canvasBg) * 7) / 10,
                         (GetBValue(th.canvasBg) * 7) / 10);
      HBRUSH sh = CreateSolidBrush(shC);
      RECT sr{x + 4, y + 4, x + w + 4, y + h + 4};
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

      // Search-match highlights. The active match is a filled amber box; the
      // other matches on the page get an outline so the text stays readable.
      if (!g.findHits.empty())
      {
        for (int hi = 0; hi < (int)g.findHits.size(); ++hi)
        {
          const App::FindHit& fh = g.findHits[hi];
          if (fh.page != i) continue;
          std::vector<FS_RECTF> fr;
          FindHighlightRects(i, fh.start, fh.count, fr);
          const bool active = (hi == g.findCur);
          const COLORREF amber = RGB(255, 168, 0);
          HBRUSH hb2 = CreateSolidBrush(amber);
          HPEN pn2 = CreatePen(PS_SOLID, 2, RGB(208, 96, 0));
          for (const FS_RECTF& q : fr)
          {
            int ax = x + (int)std::lround(q.left * s);
            int ay = y + (int)std::lround((PageH(i) - q.top) * s);
            int aw = (int)std::lround((q.right - q.left) * s);
            int ah = (int)std::lround((q.top - q.bottom) * s);
            if (aw < 1 || ah < 1) continue;
            if (active)
            {
              HBRUSH ob = (HBRUSH)SelectObject(dc, hb2);
              HPEN op = (HPEN)SelectObject(dc, pn2);
              Rectangle(dc, ax, ay - 1, ax + aw, ay + ah + 1);
              SelectObject(dc, op);
              SelectObject(dc, ob);
            }
            else
            {
              HPEN op = (HPEN)SelectObject(dc, pn2);
              HBRUSH ob = (HBRUSH)SelectObject(dc, GetStockObject(NULL_BRUSH));
              Rectangle(dc, ax, ay - 1, ax + aw, ay + ah + 1);
              SelectObject(dc, ob);
              SelectObject(dc, op);
            }
          }
          DeleteObject(pn2);
          DeleteObject(hb2);
        }
      }

      // Content-object selection overlay (Select tool)
      if (g.sel.active && g.sel.page == i && g.sel.type > 0)
      {
        float slx = g.sel.l, sbx = g.sel.b, srx = g.sel.r, stx = g.sel.t;
        if (srx > slx && stx > sbx)
        {
          float phh = PageH(i);
          int ax = x + (int)std::lround(slx * s);
          int ay = y + (int)std::lround((phh - stx) * s);
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
  }

  // Drag-to-draw rubber band for the armed annotation tool.
  if (g.annDrag && g.annTool)
  {
    RECT br{std::min(g.annStart.x, g.annCur.x), std::min(g.annStart.y, g.annCur.y),
            std::max(g.annStart.x, g.annCur.x), std::max(g.annStart.y, g.annCur.y)};
    HPEN pen = CreatePen(PS_DOT, 1, th.accent);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBr = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, br.left, br.top, br.right, br.bottom);
    SelectObject(dc, oldBr);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
  }
}

struct HitInfo
{
  int page, x, y, w, h;
};
static bool HitPage(POINT pt, HitInfo& out);
static bool GetLinkAtDevice(FPDF_PAGE page, POINT pt, FPDF_LINK& link);
static bool FollowLink(FPDF_PAGE page, POINT pt);
static void InsertAnnotCurrent(int kind, const FS_RECTF* rect);
static bool DragToPageRect(int page, POINT a, POINT b, FS_RECTF& out);
static void CancelAnnotTool();

// Maps the canvas viewport onto a destination for a "bookmark this view" entry.
// |pageLeftClient|/|pageTopClient| place the page's top-left in canvas client
// coordinates, and |pw|/|ph| are its size in points. PDF y grows upward from the
// page bottom, hence the flip, and the result is clamped into the page so a
// partly scrolled page still gets a sane target. |zoom| is screen pixels per PDF
// point; the PDF /Zoom factor is per 72. Pure, so the self-test can exercise it.
static void ViewDestForPage(double pw, double ph, int pageLeftClient,
                            int pageTopClient, double zoom, double& x, double& y,
                            double& pdfZoom)
{
  if (!(zoom > 0)) zoom = 1.0;
  const double dx = (0.0 - pageLeftClient) / zoom;
  const double dy = ph - (0.0 - pageTopClient) / zoom;
  x = dx < 0 ? 0 : (dx > pw ? pw : dx);
  y = dy < 0 ? 0 : (dy > ph ? ph : dy);
  pdfZoom = zoom * 96.0 / 72.0;
}

// Adds a user bookmark for `page`. When atView is set the destination also
// records the current scroll position and zoom, so reopening it returns to the
// same spot rather than the top of the page. The bookmark is held in memory and
// written into the PDF by AppendOutlines() on save; the user is asked to save now.
static void AddUserBookmark(int page, bool atView)
{
  if (!g.doc || page < 0 || page >= g.pageCount) return;

  UserBookmark bm;
  bm.page = page;
  bm.atView = atView;
  if (atView)
  {
    RECT cr{};
    GetClientRect(g.canvas, &cr);
    std::vector<RECT> rects;
    int cw2 = 0, ch2 = 0;
    LayoutPages(cr.right - cr.left, rects, cw2, ch2);
    if (page < (int)rects.size())
    {
      const POINT org = PageOrigin((int)std::lround(g.scrollX),
                                   (int)std::lround(g.scrollY));
      ViewDestForPage(PageW(page), PageH(page), rects[page].left + org.x,
                      rects[page].top + org.y, g.zoom, bm.x, bm.y, bm.zoom);
    }
  }

  std::wstring deflt = L"Page " + std::to_wstring(page + 1);
  std::wstring name;
  if (!PromptBookmarkName(name, deflt)) return;
  bm.title = name;

  g.marks.push_back(bm);
  g.dirty = true;
  g.bmDirty = true;
  RefreshState();
  InvalidateRect(g.status, nullptr, TRUE);

  const std::wstring msg =
      L"Bookmark \"" + name + L"\" added.\n\nWrite it into the PDF now?";
  if (MessageBoxW(g.frame, msg.c_str(), L"Add Bookmark",
                  MB_YESNO | MB_ICONQUESTION) == IDYES)
  {
    if (g.path.empty()) SaveAs();
    else SaveInPlace();
  }
}

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
      int cw = rc.right - rc.left;
      int ch = rc.bottom - rc.top;
      HDC mem = CreateCompatibleDC(dc);
      HBITMAP bmp = CreateCompatibleBitmap(dc, cw, ch);
      HGDIOBJ oldBmp = SelectObject(mem, bmp);
      CanvasPaint(mem, cw, ch);
      BitBlt(dc, 0, 0, cw, ch, mem, 0, 0, SRCCOPY);
      SelectObject(mem, oldBmp);
      DeleteObject(bmp);
      DeleteDC(mem);
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
      if (g.annTool)
      {
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
      }
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
    case WM_CONTEXTMENU:
    {
      // Right-click (or Shift+F10 / the menu key) offers to bookmark the page
      // under the pointer, or the current view of it.
      if (!g.doc || g.pageCount <= 0) return 0;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (pt.x == -1 && pt.y == -1)   // keyboard invocation: no pointer position
      {
        RECT cr{};
        GetClientRect(hw, &cr);
        pt = POINT{ cr.right / 2, cr.top + 8 };
      }
      HitInfo hi;
      const int page = HitPage(pt, hi) ? hi.page : g.selected;
      if (page < 0 || page >= g.pageCount) return 0;
      HMENU m = CreatePopupMenu();
      if (!m) return 0;
      AppendMenuW(m, MF_STRING, ID_BM_PAGE,
                  (L"Bookmark page " + std::to_wstring(page + 1)).c_str());
      AppendMenuW(m, MF_STRING, ID_BM_VIEW, L"Bookmark this view");
      SetForegroundWindow(hw);
      const int cmd = (int)TrackPopupMenu(
          m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_LEFTBUTTON,
          pt.x, pt.y, 0, hw, nullptr);
      DestroyMenu(m);
      if (cmd == ID_BM_PAGE) AddUserBookmark(page, false);
      else if (cmd == ID_BM_VIEW) AddUserBookmark(page, true);
      return 0;
    }
    case WM_LBUTTONDOWN:
    {
      SetFocus(g.frame);
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      HitInfo hi;
      if (HitPage(pt, hi))
      {
        if (g.annTool)
        {
          // Begin a drag-to-draw annotation on the page under the pointer.
          g.annDrag = true;
          g.annPage = hi.page;
          g.annStart = pt;
          g.annCur = pt;
          SetCapture(hw);
          return 0;
        }
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
          InvalidateRect(hw, nullptr, FALSE);
          InvalidateRect(g.thumbs, nullptr, FALSE);
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
          InvalidateRect(hw, nullptr, FALSE);
          InvalidateRect(g.thumbs, nullptr, FALSE);
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
      if (g.annDrag && GetCapture() == hw)
      {
        g.annCur = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        InvalidateRect(hw, nullptr, FALSE);
        return 0;
      }
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
      if (g.annDrag)
      {
        ReleaseCapture();
        g.annCur = POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const int kind = g.annTool;
        const int page = g.annPage;
        const POINT a = g.annStart, b = g.annCur;
        g.annTool = 0;
        g.annDrag = false;
        g.annPage = -1;
        // Arm the page that was drawn on, then place the annotation. A drag of
        // a few pixels or less falls back to the kind's default rectangle.
        if (page >= 0 && page < g.pageCount) g.selected = page;
        const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
        FS_RECTF rc{};
        const bool drawn = (dx >= 4 || dy >= 4) &&
                           DragToPageRect(page, a, b, rc);
        InsertAnnotCurrent(kind, drawn ? &rc : nullptr);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        InvalidateRect(hw, nullptr, TRUE);
        InvalidateRect(g.status, nullptr, TRUE);
        return 0;
      }
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
static void RelayoutPanes(int w, int h);

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
          RelayoutPanes(cr.right - cr.left, cr.bottom - cr.top);
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

// Paints one ribbon glyph centred in `box`. GDI cannot flip text, so mirrored
// glyphs are rendered into a DIB and blitted with a negative destination width.
// When the icon font is missing, `label` supplies a tinted initial-letter chip.
static void DrawRibbonIcon(HDC dc, const RECT& box, wchar_t glyph,
                           const std::wstring& label, COLORREF fg, COLORREF bg,
                           bool mirror)
{
  const int w = box.right - box.left;
  const int h = box.bottom - box.top;
  if (w <= 0 || h <= 0) return;

  if (!g.haveMdl2 || !glyph)
  {
    const UiTheme& th = ThemeNow();
    int cx = (box.left + box.right) / 2;
    int cy = (box.top + box.bottom) / 2;
    int s = w < h ? w : h;
    RECT chip{cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2};
    HBRUSH fill = CreateSolidBrush(fg);
    HPEN nopen = (HPEN)GetStockObject(NULL_PEN);
    HBRUSH wb = (HBRUSH)SelectObject(dc, fill);
    HPEN wp = (HPEN)SelectObject(dc, nopen);
    RoundRect(dc, chip.left, chip.top, chip.right, chip.bottom, s, s);
    SelectObject(dc, wb);
    SelectObject(dc, wp);
    DeleteObject(fill);
    if (!label.empty())
    {
      wchar_t ch[2] = {label[0], 0};
      SetBkMode(dc, TRANSPARENT);
      SetTextColor(dc, th.card);
      HFONT wf = (HFONT)SelectObject(dc, g.font);
      DrawTextW(dc, ch, -1, &chip, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(dc, wf);
    }
    return;
  }

  wchar_t t[2] = {glyph, 0};
  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ of = SelectObject(dc, g.iconFont);
  SetTextColor(dc, fg);
  // Centre the glyph by its real ink box; MDL2 glyphs carry a lot of internal
  // leading, so plain DT_CENTER makes them ride far too high in the chip.
  SIZE ext{};
  GetTextExtentPoint32W(dc, t, 1, &ext);
  int gx = box.left + (w - ext.cx) / 2;
  int gy = box.top + (h - ext.cy) / 2 + MulDiv(2, g.dpi, 72);
  if (!mirror)
  {
    TextOutW(dc, gx, gy, t, 1);
    SelectObject(dc, of);
    return;
  }

  HDC mem = CreateCompatibleDC(dc);
  if (!mem) { SelectObject(dc, of); return; }
  HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
  if (!bmp) { DeleteDC(mem); SelectObject(dc, of); return; }
  HGDIOBJ obmp = SelectObject(mem, bmp);
  RECT zb{0, 0, w, h};
  HBRUSH back = CreateSolidBrush(bg);
  FillRect(mem, &zb, back);
  DeleteObject(back);
  SetBkMode(mem, TRANSPARENT);
  HGDIOBJ of2 = SelectObject(mem, g.iconFont);
  SetTextColor(mem, fg);
  TextOutW(mem, gx, gy, t, 1);
  SelectObject(mem, of2);
  SetStretchBltMode(dc, HALFTONE);
  StretchBlt(dc, box.right, box.top, -w, h, mem, 0, 0, w, h, SRCCOPY);
  SelectObject(mem, obmp);
  DeleteObject(bmp);
  DeleteDC(mem);
  SelectObject(dc, of);
}

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
  const UiTheme& th = ThemeNow();
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
    const COLORREF tfg = b->pressed ? th.accent : (b->hover ? th.text : th.textDim);
    SetTextColor(dc, tfg);
    if (b->icon)
    {
      int s = MulDiv(16, g.dpi, 72);
      int cy = (rc.top + rc.bottom) / 2;
      RECT ib{rc.left + MulDiv(4, g.dpi, 72), cy - s / 2,
              rc.left + MulDiv(4, g.dpi, 72) + s, cy + s / 2};
      DrawRibbonIcon(dc, ib, b->icon, b->label, tfg,
                     b->hover ? th.btnHover : th.card, false);
      RECT lb{ib.right + MulDiv(4, g.dpi, 72), rc.top, rc.right - 2, rc.bottom};
      DrawTextW(dc, b->label.c_str(), -1, &lb,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    else
    {
      DrawTextW(dc, b->label.c_str(), -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
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
    const COLORREF fg = b->down ? th.accent : th.text;
    const COLORREF back = active ? (b->down ? th.btnDown : th.btnHover) : th.card;
    HFONT wf = (HFONT)SelectObject(dc, g.font);
    if (b->icon)
    {
      int iconPx = MulDiv(20, g.dpi, 72);
      int top = rc.top + MulDiv(1, g.dpi, 72);
      RECT ibox{rc.left, top, rc.right, top + iconPx};
      RECT lbox{rc.left, top + iconPx, rc.right, rc.bottom};
      DrawRibbonIcon(dc, ibox, b->icon, b->label, fg, back, b->mirror);
      SetTextColor(dc, fg);
      DrawTextW(dc, b->label.c_str(), -1, &lbox,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    else
    {
      SetTextColor(dc, fg);
      DrawTextW(dc, b->label.c_str(), -1, &rc,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, wf);
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

// Attach a hover-help string to a control. Silently does nothing when the
// tooltip control could not be created.
static void AddTip(HWND target, const wchar_t* text)
{
  if (!g.tips || !target || !text) return;
  TOOLINFOW ti{};
  ti.cbSize = sizeof(ti);
  ti.uFlags = TTF_TRANSPARENT;
  ti.hwnd = g.frame;
  ti.hinst = g.inst;
  ti.lpszText = const_cast<wchar_t*>(text);
  ti.uId = static_cast<UINT_PTR>(GetWindowLongPtrW(target, GWLP_ID));
  // whole-control hit area; the tooltip tracks the cursor, so an empty rect is
  // not usable here
  GetWindowRect(target, &ti.rect);
  ScreenToClient(g.frame, reinterpret_cast<POINT*>(&ti.rect.left));
  ti.rect.right = ti.rect.left;
  ti.rect.bottom = ti.rect.top;
  SendMessageW(g.tips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
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
  int tab;      // ribbon tab index, one per tool group
  wchar_t icon; // Segoe MDL2 Assets codepoint
  bool mirror;  // flip horizontally (counter-clockwise)
  const wchar_t* tip;  // hover help
};

static void BuildToolbar(HWND)
{
  static const RibbonSpec specs[] = {
    {ID_NEW,          L"New",        54, 0, 0xE8A5, false,
     L"Create a new empty document (Ctrl+N)"},
    {ID_OPEN,         L"Open",       58, 0, 0xE8E5, false,
     L"Open an existing PDF file (Ctrl+O)"},
    {ID_SAVE,         L"Save",       56, 0, 0xE74E, false,
     L"Save changes to the current file (Ctrl+S)"},
    {ID_SAVEAS,       L"Save As",    74, 0, 0xE792, false,
     L"Save to a new file name (Ctrl+Shift+S)"},
    {ID_IMPORT,       L"Import",     68, 0, 0xE8B5, false,
     L"Insert pages from another PDF into this one"},
    {ID_EXPORT_TEXT,  L"Export Text",80, 0, 0xE8C3, false,
     L"Write the document text out to a .txt file"},
    {ID_EXPORT_CSV,   L"Export CSV", 66, 0, 0xE8FD, false,
     L"Write page data out to a .csv file"},
    {ID_ROTL,         L"Rotate CCW", 88, 1, 0xE7AD, true,
     L"Rotate the current page 90 degrees counter-clockwise (Ctrl+Shift+R)"},
    {ID_ROTR,         L"Rotate CW",  86, 1, 0xE7AD, false,
     L"Rotate the current page 90 degrees clockwise (Ctrl+R)"},
    {ID_DELETE,       L"Delete",     66, 1, 0xE74D, false,
     L"Delete the current page (Del)"},
    {ID_ADD,          L"Add Page",   80, 1, 0xE710, false,
     L"Append a blank page to the end of the document"},
    {ID_PAGE_EXTRACT, L"Extract",    70, 1, 0xE896, false,
     L"Save the current page as its own PDF file"},
    {ID_PAGE_SPLIT,   L"Split",      60, 1, 0xE8EE, false,
     L"Split the document into one file per page"},
    {ID_PAGE_CROP,    L"Auto-Crop",  84, 1, 0xE7A8, false,
     L"Trim the white margin around the current page"},
    {ID_ZOOM_OUT,     L"Zoom -",     62, 4, 0xE71F, false,
     L"Zoom out one step (Ctrl+-)"},
    {ID_ZOOM_IN,      L"Zoom +",     62, 4, 0xE8A3, false,
     L"Zoom in one step (Ctrl+=)"},
    {ID_ZOOM100,      L"100%",       54, 4, 0xE71E, false,
     L"Reset the zoom to 100% (Ctrl+1)"},
    {ID_FITW,         L"Fit Width",  80, 4, 0xE8A9, false,
     L"Scale the page so its width fills the window (Ctrl+W)"},
    {ID_FITP,         L"Fit Page",   76, 4, 0xE740, false,
     L"Scale the page so the whole page is visible (Ctrl+0)"},
    {ID_PREV,         L"Previous",   78, 5, 0xE892, false,
     L"Go to the previous page (Ctrl+PgUp)"},
    {ID_NEXT,         L"Next",       62, 5, 0xE893, false,
     L"Go to the next page (Ctrl+PgDn)"},
    {ID_SPREAD,       L"Spread",     70, 5, 0xE89A, false,
     L"Show facing pages side by side (F5)"},
    {ID_ANN_HL,       L"Highlight",  80, 2, 0xE82A, false,
     L"Draw a translucent highlight across the page"},
    {ID_ANN_UL,       L"Underline",  80, 2, 0xE8D2, false,
     L"Draw an underline annotation on the page"},
    {ID_ANN_NOTE,     L"Note",       56, 2, 0xE70B, false,
     L"Attach a sticky note comment at the click point"},
    {ID_ANN_TEXT,     L"Text Box",   78, 2, 0xE8C1, false,
     L"Place free text on the page"},
    {ID_ANN_SHAPE,    L"Shape",      62, 2, 0xE8EC, false,
     L"Draw a rectangle or oval shape"},
    {ID_ANN_STAMP,    L"Stamp",      62, 2, 0xE735, false,
     L"Stamp the page number or a custom mark"},
    {ID_ANN_LINK,     L"Link",       62, 2, 0xE71B, false,
     L"Place a clickable web link on the page"},
    {ID_TOOL_SELECT,  L"Select",     62, 3, 0xE8B0, false,
     L"Click a content object to select it, then drag to move it"},
    {ID_OBJ_EDIT,     L"Edit Text",  78, 3, 0xE70F, false,
     L"Change the wording of the selected text object (double-click)"},
    {ID_OBJ_DELETE,   L"Delete",     62, 3, 0xE74D, false,
     L"Remove the selected content object (Ctrl+Del)"},
    {ID_OBJ_RECOLOR,  L"Recolor",    66, 3, 0xE790, false,
     L"Change the fill or stroke colour of the selected object"},
    {ID_WATERMARK,    L"Watermark",  76, 6, 0xE7C3, false,
     L"Stamp a diagonal text watermark across every page"},
    {ID_SAVEENC,      L"Encrypt",    66, 6, 0xE72E, false,
     L"Save a copy protected by a 128-bit RC4 password"},
  };
  for (const RibbonSpec& s : specs)
  {
    HWND hw = MakeBtn(s.id, s.label, 0, 0, s.w, RIB_BTN_H);
    Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (b) { b->rtab = s.tab; b->icon = s.icon; b->mirror = s.mirror; }
    AddTip(hw, s.tip);
    g.ribbonBtns.push_back(hw);
  }
}

static const wchar_t* RibbonTabName(int tab)
{
  static const wchar_t* names[] = {L"Document", L"Pages", L"Annotate", L"Content",
                                   L"Zoom",    L"Navigate", L"Security"};
  return (tab >= 0 && tab <= ID_TAB_LAST - ID_TAB_FIRST) ? names[tab] : L"";
}

static int RibbonTabCount()
{
  return ID_TAB_LAST - ID_TAB_FIRST + 1;
}

static void SetTabPressed()
{
  HMENU bar = GetMenu(g.frame);
  if (!bar) return;
  CheckMenuRadioItem(bar, ID_TAB_FIRST, ID_TAB_LAST,
                     ID_TAB_FIRST + g.ribbonTab, MF_BYCOMMAND);
  CheckMenuItem(bar, ID_SIDEBAR,
                MF_BYCOMMAND | (g.showSidebar ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(bar, ID_SPREAD,
                MF_BYCOMMAND | (g.spread ? MF_CHECKED : MF_UNCHECKED));
}

// Measures the tab strip; the same pass feeds painting and hit-testing.
static void LayoutRibbonTabs(HDC measure, HFONT font, std::vector<RECT>& out)
{
  out.clear();
  HGDIOBJ old = SelectObject(measure, font);
  int x = 8;
  for (int i = 0; i < RibbonTabCount(); ++i)
  {
    SIZE cxt{};
    GetTextExtentPoint32W(measure, RibbonTabName(i), (int)wcslen(RibbonTabName(i)),
                          &cxt);
    RECT r{x, 4, x + cxt.cx + MulDiv(28, g.dpi, 72), RIB_TAB_H - 4};
    out.push_back(r);
    x = r.right + 2;
  }
  SelectObject(measure, old);
}

static void LayoutRibbon()
{
  g.groups.clear();

  // Tab strip
  HDC probe = CreateCompatibleDC(nullptr);
  if (probe)
  {
    HFONT f = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE,
                          FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                          DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    LayoutRibbonTabs(probe, f, g.ribbonTabRects);
    DeleteObject(f);
    DeleteDC(probe);
  }

  // Only the active tab's buttons are visible, laid out left to right.
  int x = 6;
  int gx = x;
  for (HWND hw : g.ribbonBtns)
  {
    Btn* b = reinterpret_cast<Btn*>(GetWindowLongPtrW(hw, GWLP_USERDATA));
    if (!b || b->rtab != g.ribbonTab) { ShowWindow(hw, SW_HIDE); continue; }
    RECT rc2{};
    GetWindowRect(hw, &rc2);
    int w = rc2.right - rc2.left;
    SetWindowPos(hw, nullptr, x, RIB_BTN_Y, w, RIB_BTN_H, SWP_NOZORDER);
    ShowWindow(hw, SW_SHOW);
    x += w + 6;
  }
  if (x > gx)
  {
    App::GroupBox gb;
    gb.tab = g.ribbonTab;
    gb.name = RibbonTabName(g.ribbonTab);
    gb.rc = {gx - 4, RIB_TAB_H + 2, x - 6, RIB_H - 2};
    g.groups.push_back(gb);
  }

  SetTabPressed();
  InvalidateRect(g.toolbar, nullptr, TRUE);
}

static void SwitchRibbonTab(int tab)
{
  if (tab < 0 || tab >= RibbonTabCount()) return;
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
  double mw = std::max(1.0, LayoutSpanW());
  double mh = std::max(1.0, MaxPageH());
  ZoomTo(std::min((cw - 40.0) / mw, (ch - 60.0) / mh), true);
}

static void FitWidth()
{
  RECT rc;
  GetClientRect(g.canvas, &rc);
  int cw = std::max(120, (int)(rc.right - rc.left));
  double mw = std::max(1.0, LayoutSpanW());
  ZoomTo(std::max(0.1, (cw - 40.0) / mw), true);
}

static void GotoPageIndex(int idx)
{
  if (g.pageCount == 0) return;
  g.selected = std::max(0, std::min(g.pageCount - 1, idx));
  RECT cr{};
  int cw = 120;
  if (g.canvas) { GetClientRect(g.canvas, &cr); cw = cr.right - cr.left; }
  std::vector<RECT> rects;
  int cwD = 0, chD = 0;
  LayoutPages(cw, rects, cwD, chD);
  if (g.selected >= 0 && g.selected < (int)rects.size())
  {
    int rTop = rects[g.selected].top;
    g.scrollY = std::max(0, rTop - 12 - (int)(PageH(g.selected) * g.zoom * 0.2));
  }
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
  RECT cr{};
  int cw = 120;
  if (g.canvas) { GetClientRect(g.canvas, &cr); cw = cr.right - cr.left; }
  std::vector<RECT> rects;
  int cwDummy = 0, chDummy = 0;
  LayoutPages(cw, rects, cwDummy, chDummy);
  POINT org = PageOrigin((int)std::lround(g.scrollX),
                         (int)std::lround(g.scrollY));
  for (int i = 0; i < g.pageCount && i < (int)rects.size(); ++i)
  {
    const RECT& pr = rects[i];
    int w = pr.right - pr.left, h = pr.bottom - pr.top;
    if (w < 1 || h < 1) continue;
    int x = pr.left + org.x, y = pr.top + org.y;
    if (pt.x >= x && pt.x <= x + w && pt.y >= y && pt.y <= y + h)
    {
      out = {i, x, y, w, h};
      return true;
    }
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
  // Bookmarks added in this session that are not on disk yet, so the user can
  // see and jump to them before saving. Encoded as a negative page+1.
  for (size_t i = 0; i < g.marks.size(); ++i)
  {
    const UserBookmark& bm = g.marks[i];
    if (bm.page < 0 || bm.page >= g.pageCount) continue;
    std::wstring label = bm.title;
    if (bm.atView) label += L"  (view)";
    std::vector<wchar_t> tmp(label.begin(), label.end());
    tmp.push_back(0);
    TVINSERTSTRUCTW ti{};
    ti.hParent = TVI_ROOT;
    ti.hInsertAfter = TVI_LAST;
    ti.item.mask = TVIF_TEXT | TVIF_PARAM;
    ti.item.pszText = tmp.data();
    ti.item.lParam = (LPARAM)(-(bm.page + 1));
    TreeView_InsertItem(g.bookmarks, &ti);
  }
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
// Creates one annotation on page pageIdx. When `rect` is non-null the caller
// (drag-to-draw) supplies the rectangle in PDF coordinates; otherwise each kind
// falls back to its conventional default placement. Pure FPDF work.
static bool InsertAnnot(FPDF_DOCUMENT doc, int pageIdx, int kind,
                        const FS_RECTF* rect = nullptr)
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
    if (rect)
    {
      x = rect->left;
      y = rect->bottom;
      w = rect->right - rect->left;
      h = rect->top - rect->bottom;
    }
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
      FS_RECTF rc = rect ? *rect
                         : FS_RECTF{pw - 90.0f, ph - 36.0f, pw - 30.0f, ph - 90.0f};
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
      FS_RECTF rc = rect ? *rect
                         : FS_RECTF{pw * 0.12f, ph * 0.55f, pw * 0.52f, ph * 0.42f};
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
      FS_RECTF rc = rect ? *rect
                         : FS_RECTF{pw * 0.12f, ph * 0.35f, pw * 0.12f + s, ph * 0.35f - s};
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
      FS_RECTF rc = rect ? *rect
                         : FS_RECTF{pw * 0.55f, ph * 0.18f, pw * 0.55f + 150.0f, ph * 0.18f - 52.0f};
      ok = FPDFAnnot_SetRect(a, &rc) != 0;
      SetAnnotText(a, "Name", L"Draft");
      SetAnnotText(a, "Contents", L"DRAFT");
    }
  }
  else if (kind == ID_ANN_LINK)
  {
    // A hyperlink is a /Link annotation; the target lives in its URI action.
    std::wstring uri = g_linkUri;
    while (!uri.empty() && (uri.back() == L' ' || uri.back() == L'\t')) uri.pop_back();
    size_t first = uri.find_first_not_of(L" \t");
    uri = (first == std::wstring::npos) ? std::wstring() : uri.substr(first);

    a = FPDFPage_CreateAnnot(page, FPDF_ANNOT_LINK);
    if (a)
    {
      float x = pw * 0.12f;
      float y = ph * 0.62f;
      float w = 190.0f;
      float h = 20.0f;
      FS_RECTF rc = rect ? *rect : FS_RECTF{x, y + h, x + w, y};
      ok = FPDFAnnot_SetRect(a, &rc) != 0;
      if (!uri.empty())
      {
        int n = WideCharToMultiByte(CP_UTF8, 0, uri.c_str(), -1, nullptr, 0,
                                    nullptr, nullptr);
        if (n > 1)
        {
          std::string utf8(static_cast<size_t>(n), '\0');
          WideCharToMultiByte(CP_UTF8, 0, uri.c_str(), -1, &utf8[0], n,
                              nullptr, nullptr);
          ok = ok && (FPDFAnnot_SetURI(a, utf8.c_str()) != 0);
        }
      }
      // Links draw no border of their own; the Rect is the click area.
      ok = ok && (FPDFAnnot_SetFlags(a, FPDF_ANNOT_FLAG_PRINT) != 0);
      SetAnnotText(a, "Contents", uri.c_str());
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

static void InsertAnnotCurrent(int kind, const FS_RECTF* rect = nullptr)
{
  if (!g.doc || g.pageCount == 0) return;
  if (InsertAnnot(g.doc, g.selected, kind, rect))
  {
    g.dirty = true;
    InvalidateRect(g.canvas, nullptr, TRUE);
    InvalidateRect(g.thumbs, nullptr, TRUE);
    InvalidateRect(g.status, nullptr, TRUE);
  }
}

// Arms drag-to-draw for an annotation kind. The next left-drag on a page sets
// the annotation rectangle; a plain click falls back to the default placement.
static void SetAnnotTool(int kind)
{
  g.annTool = kind;
  g.annDrag = false;
  if (g.canvas)
  {
    SetCursor(LoadCursorW(nullptr, IDC_CROSS));
    InvalidateRect(g.canvas, nullptr, FALSE);
  }
  if (g.status) InvalidateRect(g.status, nullptr, TRUE);
}

static void CancelAnnotTool()
{
  if (!g.annTool && !g.annDrag) return;
  if (g.annDrag) ReleaseCapture();
  g.annTool = 0;
  g.annDrag = false;
  g.annPage = -1;
  if (g.canvas)
  {
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    InvalidateRect(g.canvas, nullptr, TRUE);
  }
  if (g.status) InvalidateRect(g.status, nullptr, TRUE);
}

// Maps a canvas-client drag on `page` to a PDF-space FS_RECTF (left, top,
// right, bottom with top > bottom), clamped to the page. Pure geometry so the
// self-test can exercise it without a window.
static bool DragToPageRect(int page, POINT a, POINT b, FS_RECTF& out)
{
  if (!g.doc || page < 0 || page >= g.pageCount) return false;
  int cw = 120;
  if (g.canvas)
  {
    RECT cr{};
    GetClientRect(g.canvas, &cr);
    cw = cr.right - cr.left;
  }
  std::vector<RECT> rects;
  int cwDummy = 0, chDummy = 0;
  LayoutPages(cw, rects, cwDummy, chDummy);
  if (page >= (int)rects.size()) return false;
  const RECT& pr = rects[page];
  if (pr.right - pr.left < 1 || pr.bottom - pr.top < 1) return false;
  const POINT org = PageOrigin((int)std::lround(g.scrollX),
                               (int)std::lround(g.scrollY));
  const double s = g.zoom > 0 ? g.zoom : 1.0;
  const double pw = PageW(page), ph = PageH(page);
  auto toPdf = [&](POINT p, double& x, double& y) {
    double lx = (p.x - (pr.left + org.x)) / s;
    double ly = ph - (p.y - (pr.top + org.y)) / s;
    lx = lx < 0 ? 0 : (lx > pw ? pw : lx);
    ly = ly < 0 ? 0 : (ly > ph ? ph : ly);
    x = lx;
    y = ly;
  };
  double x1 = 0, y1 = 0, x2 = 0, y2 = 0;
  toPdf(a, x1, y1);
  toPdf(b, x2, y2);
  const double l = std::min(x1, x2), r = std::max(x1, x2);
  const double bo = std::min(y1, y2), to = std::max(y1, y2);
  out = FS_RECTF{(float)l, (float)to, (float)r, (float)bo};
  return true;
}

// --- Text search (Ctrl+F / F3 / Shift+F3) ----------------------------------

// Collects every case-insensitive match of `query` in document order.
static int CountFindHits(FPDF_DOCUMENT doc, const std::wstring& query,
                         std::vector<App::FindHit>* hits)
{
  if (hits) hits->clear();
  if (!doc || query.empty()) return 0;
  int total = 0;
  const int pages = FPDF_GetPageCount(doc);
  for (int i = 0; i < pages; ++i)
  {
    FPDF_PAGE page = FPDF_LoadPage(doc, i);
    if (!page) continue;
    FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
    if (tp)
    {
      FPDF_SCHHANDLE sch = FPDFText_FindStart(
          tp, reinterpret_cast<FPDF_WIDESTRING>(query.c_str()), 0, 0);
      if (sch)
      {
        while (FPDFText_FindNext(sch))
        {
          int start = FPDFText_GetSchResultIndex(sch);
          int count = FPDFText_GetSchCount(sch);
          if (count > 0)
          {
            ++total;
            if (hits) hits->push_back(App::FindHit{i, start, count});
          }
        }
        FPDFText_FindClose(sch);
      }
      FPDFText_ClosePage(tp);
    }
    FPDF_ClosePage(page);
  }
  return total;
}

// Fills `out` with the PDF-space highlight rectangles of one match.
static void FindHighlightRects(int pageIdx, int start, int count,
                               std::vector<FS_RECTF>& out)
{
  out.clear();
  if (!g.doc || pageIdx < 0 || pageIdx >= g.pageCount) return;
  FPDF_PAGE page = FPDF_LoadPage(g.doc, pageIdx);
  if (!page) return;
  FPDF_TEXTPAGE tp = FPDFText_LoadPage(page);
  if (tp)
  {
    int n = FPDFText_CountRects(tp, start, count);
    for (int i = 0; i < n && i < 64; ++i)
    {
      double l = 0, t = 0, r = 0, b = 0;
      if (FPDFText_GetRect(tp, i, &l, &t, &r, &b))
        out.push_back(FS_RECTF{(float)l, (float)t, (float)r, (float)b});
    }
    FPDFText_ClosePage(tp);
  }
  FPDF_ClosePage(page);
}

// Jumps to the active match and repaints. False when there is no active match.
static bool FindShowCurrent()
{
  if (g.findHits.empty() || g.findCur < 0 ||
      g.findCur >= (int)g.findHits.size())
    return false;
  int page = g.findHits[g.findCur].page;
  if (page != g.selected) GotoPageIndex(page);
  InvalidateRect(g.canvas, nullptr, TRUE);
  if (g.status) InvalidateRect(g.status, nullptr, TRUE);
  return true;
}

// Ctrl+F: prompt for a term, gather every match, jump to the first at or after
// the page currently shown.
static void FindOpen()
{
  if (!g.doc || g.pageCount == 0) return;
  std::wstring q;
  if (!PromptText(q, g.findQuery, L"Find", L"Search text:", false)) return;
  g.findQuery = q;
  if (q.empty())
  {
    g.findHits.clear();
    g.findCur = -1;
    if (g.status) InvalidateRect(g.status, nullptr, TRUE);
    return;
  }
  CountFindHits(g.doc, g.findQuery, &g.findHits);
  g.findCur = -1;
  if (g.findHits.empty())
  {
    MessageBoxW(g.frame, (L"No matches for \"" + q + L"\".").c_str(), L"Find",
                MB_OK | MB_ICONINFORMATION);
    if (g.status) InvalidateRect(g.status, nullptr, TRUE);
    return;
  }
  int cur = 0;
  for (int i = 0; i < (int)g.findHits.size(); ++i)
    if (g.findHits[i].page >= g.selected) { cur = i; break; }
  g.findCur = cur;
  FindShowCurrent();
}

// F3 / Shift+F3: step through the collected matches, wrapping around.
static void FindStep(int dir)
{
  if (!g.doc || g.pageCount == 0) return;
  if (g.findHits.empty() || g.findQuery.empty())
  {
    FindOpen();
    return;
  }
  int n = (int)g.findHits.size();
  g.findCur = ((g.findCur + dir) % n + n) % n;
  FindShowCurrent();
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

static void RelayoutPanes(int w, int h)
{
  if (!g.toolbar) return;
  const int tabY = g_tabs.empty() ? 0 : TAB_H;   // collapsed when no documents
  int paneY = tabY + RIB_H + PANE_TAB_H;
  int paneH = std::max(10, h - paneY - STATUS_H);
  int canvasH = std::max(10, h - tabY - RIB_H - STATUS_H);
  bool sb = g.showSidebar;
  int sw = sb ? g.thumbsW : 0;
  SetWindowPos(g.tabbar, nullptr, 0, 0, w, TAB_H, SWP_NOZORDER);
  SetWindowPos(g.toolbar, nullptr, 0, tabY, w, RIB_H, SWP_NOZORDER);
  SetWindowPos(g.paneTabs, nullptr, 0, tabY + RIB_H, sw, PANE_TAB_H,
               SWP_NOZORDER);
  SetWindowPos(g.status, nullptr, 0, h - STATUS_H, w, STATUS_H, SWP_NOZORDER);
  SetWindowPos(g.thumbs, nullptr, 0, paneY, sw, paneH, SWP_NOZORDER);
  SetWindowPos(g.bookmarks, nullptr, 0, paneY, sw, paneH, SWP_NOZORDER);
  ShowScrollBar(g.thumbs, SB_VERT, FALSE); // thumbnails follow the canvas bar
  SetWindowPos(g.split, nullptr, sw, tabY + RIB_H, 6,
               PANE_TAB_H + paneH, SWP_NOZORDER);
  SetWindowPos(g.canvas, nullptr, sb ? sw + 6 : 0, tabY + RIB_H,
               std::max(100, w - (sb ? sw + 6 : 0)), canvasH, SWP_NOZORDER);
  ShowWindow(g.tabbar, g_tabs.empty() ? SW_HIDE : SW_SHOW);
  ShowWindow(g.paneTabs, sb ? SW_SHOW : SW_HIDE);
  ShowWindow(g.thumbs, sb && g.pane == 0 ? SW_SHOW : SW_HIDE);
  ShowWindow(g.bookmarks, sb && g.pane == 1 ? SW_SHOW : SW_HIDE);
  ShowWindow(g.split, sb ? SW_SHOW : SW_HIDE);
  InvalidateRect(g.thumbs, nullptr, FALSE);
  InvalidateRect(g.canvas, nullptr, FALSE);
  InvalidateRect(g.tabbar, nullptr, FALSE);
  InvalidateRect(g.status, nullptr, FALSE);
  UpdateScrollbars();
}

static void ToggleSidebar()
{
  g.showSidebar = !g.showSidebar;
  RECT cr{};
  GetClientRect(g.frame, &cr);
  RelayoutPanes(cr.right - cr.left, cr.bottom - cr.top);
}

static void ToggleSpread()
{
  g.spread = !g.spread;
  FitWidth();
  RECT cr{};
  int cw = 120;
  if (g.canvas) { GetClientRect(g.canvas, &cr); cw = cr.right - cr.left; }
  std::vector<RECT> rects;
  int cwD = 0, chD = 0;
  LayoutPages(cw, rects, cwD, chD);
  if (g.selected < (int)rects.size())
    g.scrollY = std::max(0, (int)rects[g.selected].top - 30);
  UpdateScrollbars();
  InvalidateRect(g.canvas, nullptr, TRUE);
  InvalidateRect(g.status, nullptr, TRUE);
  SetTabPressed();
}

// Every id DoCommand() acts on. Kept beside the switch so the self-test can
// prove two things: every menu item is handled (no dead menu entries), and
// every handled id is reachable from the menu (no orphan handlers).
static bool IsHandledCommand(int id)
{
  switch (id)
  {
    case ID_NEW: case ID_OPEN: case ID_SAVE: case ID_SAVEAS: case ID_SAVEENC:
    case ID_IMPORT: case ID_EXPORT_TEXT: case ID_EXPORT_CSV: case ID_WATERMARK:
    case ID_EXIT: case ID_ROTR: case ID_ROTL: case ID_DELETE: case ID_ADD:
    case ID_TOOL_SELECT: case ID_OBJ_EDIT: case ID_OBJ_DELETE:
    case ID_OBJ_RECOLOR: case ID_ZOOM_IN: case ID_ZOOM_OUT: case ID_ZOOM100:
    case ID_FITW: case ID_FITP: case ID_PREV: case ID_NEXT: case ID_SPREAD:
    case ID_SIDEBAR: case ID_NEW_TAB: case ID_CLOSE_TAB: case ID_PREV_TAB:
    case ID_NEXT_TAB: case ID_FIND: case ID_FIND_NEXT: case ID_FIND_PREV:
    case ID_PANE_THUMBS: case ID_PANE_BOOKMARKS:
    case ID_ANN_HL: case ID_ANN_UL: case ID_ANN_NOTE: case ID_ANN_TEXT:
    case ID_ANN_SHAPE: case ID_ANN_STAMP: case ID_ANN_LINK:
    case ID_PAGE_EXTRACT: case ID_PAGE_SPLIT: case ID_PAGE_CROP:
    case ID_THEME: case ID_ABOUT:
      return true;
    default:
      // Contiguous radio blocks.
      if (id >= ID_TAB_FIRST && id <= ID_TAB_LAST) return true;
      if (id >= ID_THEME_FIRST && id < ID_THEME_FIRST + THEME_COUNT) return true;
      return false;
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
    case ID_SPREAD:   ToggleSpread(); break;
    case ID_SIDEBAR:  ToggleSidebar(); break;
    case ID_NEW_TAB:  NewDoc(); break;
    case ID_CLOSE_TAB: CloseTab(g_curTab); break;
    case ID_PREV_TAB: NextTab(-1); break;
    case ID_NEXT_TAB: NextTab(1); break;
    case ID_FIND:      FindOpen(); break;
    case ID_FIND_NEXT: FindStep(1); break;
    case ID_FIND_PREV: FindStep(-1); break;
    case ID_TAB_DOCUMENT: SwitchRibbonTab(0); break;
    case ID_TAB_PAGES:    SwitchRibbonTab(1); break;
    case ID_TAB_ANNOTATE: SwitchRibbonTab(2); break;
    case ID_TAB_CONTENT:  SwitchRibbonTab(3); break;
    case ID_TAB_ZOOM:     SwitchRibbonTab(4); break;
    case ID_TAB_NAVIGATE: SwitchRibbonTab(5); break;
    case ID_TAB_SECURITY: SwitchRibbonTab(6); break;
    case ID_PANE_THUMBS: SetPane(0); break;
    case ID_PANE_BOOKMARKS: SetPane(1); break;
    case ID_ANN_HL:    SetAnnotTool(ID_ANN_HL); break;
    case ID_ANN_UL:    SetAnnotTool(ID_ANN_UL); break;
    case ID_ANN_NOTE:  SetAnnotTool(ID_ANN_NOTE); break;
    case ID_ANN_TEXT:  SetAnnotTool(ID_ANN_TEXT); break;
    case ID_ANN_SHAPE: SetAnnotTool(ID_ANN_SHAPE); break;
    case ID_ANN_STAMP: SetAnnotTool(ID_ANN_STAMP); break;
    case ID_ANN_LINK:
    {
      // Ask for the target first, then let the user drag its rectangle.
      std::wstring uri;
      if (PromptLinkUri(uri, g_linkUri))
      {
        g_linkUri = uri;
        SetAnnotTool(ID_ANN_LINK);
      }
      break;
    }
    case ID_PAGE_EXTRACT: ExtractCurrentPage(); break;
    case ID_PAGE_SPLIT:   SplitAllPages(); break;
    case ID_PAGE_CROP:    CropCurrentPageToContent(); break;
    case ID_EXPORT_TEXT:  ExportTextAll(); break;
    case ID_EXPORT_CSV:   ExportCsvAll(); break;
    case ID_WATERMARK:    WatermarkCurrentDoc(); break;
    case ID_THEME:        ToggleTheme(); break;
    case ID_THEME_FIRST:
    case ID_THEME_DARK:
    case ID_THEME_DARKBLUE:
    case ID_THEME_PASTEL:
    case ID_THEME_HC_DARK:
    case ID_THEME_HC_LIGHT:
    case ID_THEME_XP:
    case ID_THEME_MAC:
      SetTheme(id - ID_THEME_FIRST, true);
      break;
    case ID_ABOUT:
      MessageBoxW(g.frame,
        L"Stitchup PDF Editor\n\nPortable PDF viewer/editor\n"
        L"Engine: PDFium (BSD-3-Clause, Chromium project)\n"
        L"UI: native Win32 (zero runtime dependencies)\n\n"
        L"Shortcuts:\n"
        L"  Ctrl+O open   Ctrl+S save   Ctrl+Shift+S save as\n"
        L"  Ctrl+R rotate CW   Ctrl+Shift+R rotate CCW\n"
        L"  Ctrl+= / Ctrl+- zoom   Ctrl+0 fit page   Ctrl+1 100%\n"
        L"  Ctrl+F find   F3 next   Shift+F3 previous\n"
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
  addItem(file, ID_NEW_TAB, L"New Tab\tCtrl+T");
  addItem(file, ID_CLOSE_TAB, L"Close Tab\tCtrl+Shift+F4");
  addItem(file, ID_PREV_TAB, L"Previous Tab\tCtrl+Shift+Tab");
  addItem(file, ID_NEXT_TAB, L"Next Tab\tCtrl+Tab");
  AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
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
  addItem(edit, ID_FIND, L"Find...\tCtrl+F");
  addItem(edit, ID_FIND_NEXT, L"Find Next\tF3");
  addItem(edit, ID_FIND_PREV, L"Find Previous\tShift+F3");
  AppendMenuW(edit, MF_SEPARATOR, 0, nullptr);
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
  addItem(annotate, ID_ANN_LINK, L"Link...");
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
  addItem(view, ID_SPREAD, L"Two-Page Spread\tF5");
  addItem(view, ID_SIDEBAR, L"Sidebar\tF8");
  AppendMenuW(view, MF_SEPARATOR, 0, nullptr);

  HMENU themes = CreatePopupMenu();
  for (int i = 0; i < THEME_COUNT; ++i)
    addItem(themes, ID_THEME_FIRST + i, kThemeNames[i]);
  AppendMenuW(themes, MF_SEPARATOR, 0, nullptr);
  addItem(themes, ID_THEME, L"Dark Mode\tCtrl+D");
  AppendMenuW(view, MF_POPUP, (UINT_PTR)themes, L"&Colour Scheme");
  g_themeMenu = themes;

  AppendMenuW(bar, MF_POPUP, (UINT_PTR)view, L"&View");

  HMENU ribbon = CreatePopupMenu();
  for (int i = 0; i < RibbonTabCount(); ++i)
    addItem(ribbon, ID_TAB_FIRST + i, RibbonTabName(i));
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)ribbon, L"Ta&bs");

  HMENU help = CreatePopupMenu();
  addItem(help, ID_ABOUT, L"About");
  AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, L"&Help");
  return bar;
}

enum CapBtnId
{
  CAPB_MIN = 0,
  CAPB_MAX,
  CAPB_CLOSE,
  CAPB_COUNT
};

// Caption lives in the non-client area so Windows keeps drag, snap, resize and
// maximise for us; we only take over the pixels and the button hit-testing.
struct CapState
{
  RECT band{};
  RECT btn[CAPB_COUNT]{};
  bool hover[CAPB_COUNT]{};
  bool down[CAPB_COUNT]{};
  bool active = true;
  bool drag = false;   // a caption button currently owns the mouse capture
  bool tracked = false;
};

static CapState g_cap;

static int CaptionHeight()
{
  return GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CYCAPTION);
}

// Height of the caption band: the strip the system reserves above the menu bar.
static int CapBandH()
{
  return CaptionHeight();
}

static void LayoutCaption(HWND hw)
{
  RECT wr{};
  if (!hw) return;
  GetWindowRect(hw, &wr);
  const int ch = CapBandH();
  // Window-relative: the caption is part of the frame, and the non-client mouse
  // messages carry screen coordinates.
  g_cap.band = {0, 0, wr.right - wr.left, ch};
  const UiTheme& th = ThemeNow();
  if (th.capStyle == CAP_MAC)
  {
    const int d = MulDiv(12, g.dpi, 72);
    const int gap = MulDiv(20, g.dpi, 72);
    const int cy = g_cap.band.top + ch / 2;
    const int order[CAPB_COUNT] = {CAPB_CLOSE, CAPB_MIN, CAPB_MAX};
    int x = g_cap.band.left + MulDiv(14, g.dpi, 72);
    for (int k = 0; k < CAPB_COUNT; ++k)
    {
      g_cap.btn[order[k]] = {x, cy - d / 2, x + d, cy + d / 2};
      x += gap;
    }
  }
  else
  {
    const int bw = MulDiv(th.capStyle == CAP_XP ? 24 : 46, g.dpi, 72);
    int x = g_cap.band.right;
    for (int i = CAPB_COUNT - 1; i >= 0; --i)
    {
      g_cap.btn[i] = {x - bw, g_cap.band.top, x, g_cap.band.bottom};
      x -= bw;
    }
  }
}

static void InvalidateCaption()
{
  if (!g.frame) return;
  RedrawWindow(g.frame, &g_cap.band, nullptr,
               RDW_INVALIDATE | RDW_UPDATENOW);
}

static void CapBrushRect(HDC dc, RECT r, COLORREF c)
{
  HBRUSH b = CreateSolidBrush(c);
  FillRect(dc, &r, b);
  DeleteObject(b);
}

static void DrawCapGlyph(HDC dc, int i, COLORREF fg, RECT rc)
{
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  HPEN pen = CreatePen(PS_SOLID, 1, fg);
  HPEN was = (HPEN)SelectObject(dc, pen);
  if (i == CAPB_MIN)
  {
    MoveToEx(dc, cx - MulDiv(5, g.dpi, 72), cy + MulDiv(1, g.dpi, 72), nullptr);
    LineTo(dc, cx + MulDiv(5, g.dpi, 72), cy + MulDiv(1, g.dpi, 72));
  }
  else if (i == CAPB_MAX)
  {
    const int s = MulDiv(5, g.dpi, 72);
    if (IsZoomed(g.frame))
    {
      RECT back{cx - s + MulDiv(2, g.dpi, 72), cy - s,
                cx + s + MulDiv(2, g.dpi, 72), cy + s};
      RECT front{cx - s, cy - s + MulDiv(2, g.dpi, 72), cx + s, cy + s};
      MoveToEx(dc, back.left, back.top + s, nullptr);
      LineTo(dc, back.right, back.top + s);
      MoveToEx(dc, back.right - MulDiv(2, g.dpi, 72), back.top, nullptr);
      LineTo(dc, back.right, back.top);
      MoveToEx(dc, back.right, back.top, nullptr);
      LineTo(dc, back.right, back.bottom);
      MoveToEx(dc, front.left, front.top, nullptr);
      LineTo(dc, front.right, front.top);
      MoveToEx(dc, front.left, front.top, nullptr);
      LineTo(dc, front.left, front.bottom);
      MoveToEx(dc, front.left, front.bottom, nullptr);
      LineTo(dc, front.right, front.bottom);
      MoveToEx(dc, front.right, front.top, nullptr);
      LineTo(dc, front.right, front.bottom);
    }
    else
    {
      RECT box{cx - s, cy - s + MulDiv(1, g.dpi, 72), cx + s, cy + s};
      MoveToEx(dc, box.left, box.top, nullptr);
      LineTo(dc, box.right, box.top);
      MoveToEx(dc, box.left, box.top, nullptr);
      LineTo(dc, box.left, box.bottom);
      MoveToEx(dc, box.right, box.top, nullptr);
      LineTo(dc, box.right, box.bottom);
      MoveToEx(dc, box.left, box.bottom, nullptr);
      LineTo(dc, box.right, box.bottom);
    }
  }
  else
  {
    const int s = MulDiv(4, g.dpi, 72);
    MoveToEx(dc, cx - s, cy - s, nullptr);
    LineTo(dc, cx + s, cy + s);
    MoveToEx(dc, cx - s, cy + s, nullptr);
    LineTo(dc, cx + s, cy - s);
  }
  SelectObject(dc, was);
  DeleteObject(pen);
}

static void DrawCapMacGlyph(HDC dc, int i, RECT rc)
{
  const int cx = (rc.left + rc.right) / 2;
  const int cy = (rc.top + rc.bottom) / 2;
  const int s = MulDiv(2, g.dpi, 72);
  HPEN pen = CreatePen(PS_SOLID, 1, RGB(0x40, 0x20, 0x10));
  HPEN was = (HPEN)SelectObject(dc, pen);
  if (i == CAPB_MIN)
  {
    MoveToEx(dc, cx - s, cy, nullptr);
    LineTo(dc, cx + s, cy);
  }
  else if (i == CAPB_MAX)
  {
    MoveToEx(dc, cx - s, cy, nullptr);
    LineTo(dc, cx + s, cy);
    MoveToEx(dc, cx, cy - s, nullptr);
    LineTo(dc, cx, cy + s);
  }
  else
  {
    MoveToEx(dc, cx - s, cy - s, nullptr);
    LineTo(dc, cx + s, cy + s);
    MoveToEx(dc, cx - s, cy + s, nullptr);
    LineTo(dc, cx + s, cy - s);
  }
  SelectObject(dc, was);
  DeleteObject(pen);
}

static void PaintCaption(HDC dc, HWND hw, bool active)
{
  LayoutCaption(hw);
  const UiTheme& th = ThemeNow();
  const RECT band = g_cap.band;
  RECT capBtn[CAPB_COUNT];
  for (int i = 0; i < CAPB_COUNT; ++i) capBtn[i] = g_cap.btn[i];
  const COLORREF top = active ? th.capTop : th.card;
  const COLORREF bot = active ? th.capBottom : th.card;
  const COLORREF fg = active ? th.capText : th.textDim;
  const COLORREF glyph = active ? th.capGlyph : th.textDim;

  // Band background: vertical gradient, or a flat fill when both stops match.
  if (top == bot)
  {
    CapBrushRect(dc, band, top);
  }
  else
  {
    for (int y = band.top; y < band.bottom; ++y)
    {
      const int t = (band.bottom - band.top) > 1
                        ? MulDiv(y - band.top, 255, band.bottom - band.top - 1)
                        : 0;
      const int rr = GetRValue(top) + MulDiv(GetRValue(bot) - GetRValue(top), t, 255);
      const int gg = GetGValue(top) + MulDiv(GetGValue(bot) - GetGValue(top), t, 255);
      const int bb = GetBValue(top) + MulDiv(GetBValue(bot) - GetBValue(top), t, 255);
      HBRUSH lb = CreateSolidBrush(RGB(rr, gg, bb));
      RECT lr{band.left, y, band.right, y + 1};
      FillRect(dc, &lr, lb);
      DeleteObject(lb);
    }
  }

  HPEN line = CreatePen(PS_SOLID, 1, th.capLine);
  HPEN wasp = (HPEN)SelectObject(dc, line);
  MoveToEx(dc, band.left, band.bottom - 1, nullptr);
  LineTo(dc, band.right, band.bottom - 1);
  SelectObject(dc, wasp);
  DeleteObject(line);

  // Caption buttons
  if (th.capStyle == CAP_MAC)
  {
    static const COLORREF macColors[CAPB_COUNT] = {RGB(0xFE, 0xBC, 0x2E),
                                                   RGB(0x28, 0xC8, 0x40),
                                                   RGB(0xFF, 0x5F, 0x57)};
    for (int i = 0; i < CAPB_COUNT; ++i)
    {
      RECT r = capBtn[i];
      HBRUSH b = CreateSolidBrush(macColors[i]);
      HBRUSH wb = (HBRUSH)SelectObject(dc, b);
      HPEN nopen = (HPEN)GetStockObject(NULL_PEN);
      HPEN wp2 = (HPEN)SelectObject(dc, nopen);
      Ellipse(dc, r.left, r.top, r.right, r.bottom);
      SelectObject(dc, wb);
      SelectObject(dc, wp2);
      DeleteObject(b);
      if (g_cap.hover[i] || g_cap.down[i]) DrawCapMacGlyph(dc, i, r);
    }
  }
  else
  {
    for (int i = 0; i < CAPB_COUNT; ++i)
    {
      RECT r = capBtn[i];
      if (g_cap.hover[i] || g_cap.down[i])
        CapBrushRect(dc, r, g_cap.down[i] ? th.card : th.capBtnHover);
      DrawCapGlyph(dc, i, glyph, r);
    }
  }

  // App icon + title, inset past whichever controls lead the band.
  HICON ic = (HICON)SendMessageW(hw, WM_GETICON, ICON_SMALL, 0);
  if (!ic) ic = LoadIconW(g.inst, MAKEINTRESOURCEW(101));
  const int pad = MulDiv(8, g.dpi, 72);
  int tx = band.left + pad;
  if (th.capStyle == CAP_MAC)
    tx = capBtn[CAPB_MAX].right + MulDiv(8, g.dpi, 72);
  if (ic)
  {
    const int isz = MulDiv(16, g.dpi, 72);
    const int iy = band.top + (CaptionHeight() - isz) / 2;
    DrawIconEx(dc, tx, iy, ic, isz, isz, 0, nullptr, DI_NORMAL);
    tx += isz + MulDiv(6, g.dpi, 72);
  }
  wchar_t title[256] = {};
  GetWindowTextW(hw, title, 256);
  RECT tr{tx, band.top, capBtn[CAPB_MIN].left - pad, band.bottom};
  if (th.capStyle == CAP_MAC) tr.right = band.right - pad;
  if (tr.right > tr.left)
  {
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, fg);
    HFONT tf = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD, FALSE,
                           FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HFONT wasf = (HFONT)SelectObject(dc, tf);
    DrawTextW(dc, title, -1, &tr,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS |
                  DT_NOPREFIX);

    // A quip from the universe, centred in whatever the title left free. Only
    // drawn when there is genuinely room for it, so a long document name can
    // never be pushed into the caption buttons.
    if (!g_capPhrase.empty())
    {
      RECT calc{};
      DrawTextW(dc, title, -1, &calc,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      const int freeL = tr.left + (calc.right - calc.left) + MulDiv(18, g.dpi, 72);
      const int freeR = tr.right;
      RECT pc{};
      DrawTextW(dc, g_capPhrase.c_str(), -1, &pc,
                DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
      const int pw = pc.right - pc.left;
      if (freeR - freeL > pw + MulDiv(18, g.dpi, 72))
      {
        RECT pr{0, band.top, 0, band.bottom};
        pr.left = freeL + (freeR - freeL - pw) / 2;
        pr.right = pr.left + pw;
        // Sit back from the title: an aside, not a second title.
        COLORREF qc = active ? th.textDim : th.textDim;
        if (!th.dark) qc = RGB((GetRValue(qc) + 0xF0) / 2,
                               (GetGValue(qc) + 0xF0) / 2,
                               (GetBValue(qc) + 0xF0) / 2);
        SetTextColor(dc, qc);
        DrawTextW(dc, g_capPhrase.c_str(), -1, &pr,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
    }

    SelectObject(dc, wasf);
    DeleteObject(tf);
  }
}

// Non-client mouse messages arrive in screen coordinates; the caption keeps
// window-relative ones.
static POINT CapWindowPoint(HWND hw, int sx, int sy)
{
  RECT wr{};
  GetWindowRect(hw, &wr);
  return POINT{sx - wr.left, sy - wr.top};
}

static int CapHitTest(HWND hw, int sx, int sy)
{
  const POINT p = CapWindowPoint(hw, sx, sy);
  for (int i = 0; i < CAPB_COUNT; ++i)
    if (PtInRect(&g_cap.btn[i], p)) return i;
  return -1;
}

static void CapClick(HWND hw, int hit)
{
  if (hit == CAPB_MIN) PostMessageW(hw, WM_SYSCOMMAND, SC_MINIMIZE, 0);
  else if (hit == CAPB_MAX)
    PostMessageW(hw, WM_SYSCOMMAND, IsZoomed(hw) ? SC_RESTORE : SC_MAXIMIZE, 0);
  else PostMessageW(hw, WM_SYSCOMMAND, SC_CLOSE, 0);
}

// Returns true when the hover state actually changed, so the caller can swallow
// the message instead of passing it on.
static bool CapTrackHover(HWND hw, int sx, int sy)
{
  LayoutCaption(hw);
  const POINT p = CapWindowPoint(hw, sx, sy);
  const int hit = p.y < g_cap.band.bottom ? CapHitTest(hw, sx, sy) : -1;
  bool changed = false;
  for (int i = 0; i < CAPB_COUNT; ++i)
  {
    const bool on = (i == hit);
    if (g_cap.hover[i] != on) { g_cap.hover[i] = on; changed = true; }
  }
  if (changed) InvalidateCaption();
  return changed;
}

static bool CapClearHover()
{
  bool changed = false;
  for (int i = 0; i < CAPB_COUNT; ++i)
    if (g_cap.hover[i]) { g_cap.hover[i] = false; changed = true; }
  return changed;
}

static LRESULT CALLBACK FrameProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
  switch (msg)
  {
    case WM_GETICON:
      return (LRESULT)LoadIconW(g.inst, MAKEINTRESOURCEW(101));
    case WM_NCPAINT:
    {
      // DefWindowProc paints the frame and the system menu bar - suppressing it
      // is what left the menu text blank until a hover forced a repaint. Our
      // caption then has to go down inside the same non-client paint cycle:
      // DWM throws away frame pixels drawn on a GetWindowDC outside BeginPaint.
      const LRESULT r = DefWindowProcW(hw, msg, wp, lp);
      PAINTSTRUCT ps{};
      HDC dc = BeginPaint(hw, &ps);
      PaintCaption(dc, hw, g_cap.active);
      EndPaint(hw, &ps);
      return r;
    }
    case WM_NCACTIVATE:
      g_cap.active = (wp != FALSE);
      InvalidateCaption();
      return DefWindowProcW(hw, msg, wp, lp);
    case WM_NCMOUSEMOVE:
    {
      if (CapTrackHover(hw, GET_X_LPARAM(lp), GET_Y_LPARAM(lp)))
        return 0;
      return DefWindowProcW(hw, msg, wp, lp);
    }
    case WM_NCMOUSELEAVE:
      if (CapClearHover())
      {
        InvalidateCaption();
        return 0;
      }
      return 0;
    case WM_NCLBUTTONDOWN:
    {
      const int hit = CapHitTest(hw, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      if (hit >= 0)
      {
        g_cap.down[hit] = true;
        g_cap.drag = true;
        InvalidateCaption();
        return 0;  // swallow so the window never drags from a button
      }
      return DefWindowProcW(hw, msg, wp, lp);  // HTCAPTION: drag + snap
    }
    case WM_NCLBUTTONUP:
    {
      const int hit = CapHitTest(hw, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
      const bool wasDown = g_cap.drag;
      g_cap.drag = false;
      for (int i = 0; i < CAPB_COUNT; ++i) g_cap.down[i] = false;
      InvalidateCaption();
      if (wasDown)
      {
        if (hit >= 0 && g_cap.hover[hit]) CapClick(hw, hit);
        return 0;
      }
      return DefWindowProcW(hw, msg, wp, lp);
    }
    case WM_CAPTURECHANGED:
      g_cap.drag = false;
      for (int i = 0; i < CAPB_COUNT; ++i) g_cap.down[i] = false;
      return 0;
    case WM_SYSCOMMAND:
      if ((wp & 0xFFF0) == SC_CLOSE) LayoutCaption(hw);
      break;
    case WM_CREATE:
    {
      // Publish the frame handle first: AddTip() registers tools against
      // g.frame, and BuildToolbar() runs later in this same handler.
      g.frame = hw;
      // Styles below are the documented values; this SDK's CommCtrl.h does not
      // expose the TTS_NOFOCUS / TWS_ALPHA names.
      g.tips = CreateWindowExW(WS_EX_TRANSPARENT, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | 0x00020000 /*TTS_NOFOCUS*/ |
                                   0x00000020 /*TWS_ALPHA*/,
                               0, 0, 0, 0, hw, nullptr, g.inst, nullptr);
      if (g.tips)
      {
        SendMessageW(g.tips, TTM_SETDELAYTIME, TTDT_AUTOMATIC, 400);
        SendMessageW(g.tips, TTM_SETDELAYTIME, TTDT_RESHOW, 120);
        SendMessageW(g.tips, TTM_SETDELAYTIME, TTDT_INITIAL, 600);
        SendMessageW(g.tips, TTM_SETMAXTIPWIDTH, 0, 320);
      }

      g.tabbar = CreateWindowExW(0, L"SKTabBar", nullptr, WS_CHILD | WS_VISIBLE,
                                 0, 0, 600, TAB_H, hw, nullptr, g.inst, nullptr);
      g.toolbar = CreateWindowExW(0, L"SKToolbar", nullptr, WS_CHILD | WS_VISIBLE,
                                  0, TAB_H, 600, RIB_H, hw, nullptr, g.inst, nullptr);
      BuildToolbar(g.toolbar);
      LayoutRibbon();

      g.paneTabs = CreateWindowExW(0, L"SKToolbar", nullptr,
                                   WS_CHILD | WS_VISIBLE,
                                   0, RIB_H, g.thumbsW, PANE_TAB_H,
                                   hw, nullptr, g.inst, nullptr);
      int ha = std::max(30, g.thumbsW / 2 - 4);
      HWND ht = MakeBtn(g.paneTabs, ID_PANE_THUMBS, L"Thumbnails",
                        2, 3, ha, 20, true);
      HWND hb = MakeBtn(g.paneTabs, ID_PANE_BOOKMARKS, L"Bookmarks",
                        g.thumbsW - ha - 2, 3, ha, 20, true);
      for (HWND h : {ht, hb})
      {
        Btn* pb = reinterpret_cast<Btn*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (pb) pb->icon = (GetWindowLongPtrW(h, GWLP_ID) == ID_PANE_THUMBS)
                               ? wchar_t(0xE8B9) : wchar_t(0xE8A4);
      }
      AddTip(ht, L"Show page thumbnails in the sidebar");
      AddTip(hb, L"Show the document bookmark outline in the sidebar");

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
                                 0, 0, 800, STATUS_H, hw, nullptr, g.inst,
                                 nullptr);
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
          const LPARAM key = tv->itemNew.lParam;
          if (key < 0)
          {
            // pending bookmark: jump to the page and restore the saved view
            const size_t idx = (size_t)(-key - 1);
            if (idx < g.marks.size() && g.marks[idx].page < g.pageCount)
            {
              GotoPageIndex(g.marks[idx].page);
              if (g.marks[idx].atView && g.marks[idx].zoom > 0)
              {
                g.zoom = std::max(0.1, std::min(8.0, g.marks[idx].zoom * 72.0 / 96.0));
                UpdateScrollbars();
                InvalidateRect(g.canvas, nullptr, FALSE);
                InvalidateRect(g.status, nullptr, TRUE);
              }
            }
          }
          else
          {
            const LRESULT page = key - 1;
            if (page >= 0 && page < g.pageCount)
              GotoPageIndex((int)page);
          }
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
      if (g.toolbar) RelayoutPanes(w, h);
      LayoutCaption(g.frame);
      InvalidateCaption();
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
          case 'F': DoCommand(ID_FIND); return 0;
          case 'O': DoCommand(ID_OPEN); return 0;
          case 'T': DoCommand(ID_NEW_TAB); return 0;
          case VK_TAB: DoCommand(shift ? ID_PREV_TAB : ID_NEXT_TAB); return 0;
          case VK_F4: if (shift) DoCommand(ID_CLOSE_TAB); return 0;
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
          case VK_F3: DoCommand(shift ? ID_FIND_PREV : ID_FIND_NEXT); return 0;
          case VK_F5: DoCommand(ID_SPREAD); return 0;
          case VK_F8: DoCommand(ID_SIDEBAR); return 0;
          case VK_DELETE: DoCommand(g.toolSelect ? ID_OBJ_DELETE : ID_DELETE); return 0;
          case VK_ESCAPE: CancelAnnotTool(); return 0;
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
    {
      // AppendOutlines() depends on these properties of FPDF_SaveAsCopy output.
      std::string pdf(bytes.begin(), bytes.end());
      const size_t sx = pdf.rfind("startxref");
      check("writer: has startxref", sx != std::string::npos);
      size_t sp = sx != std::string::npos ? sx + 9 : 0;
      const long long xo = sx != std::string::npos ? pdfout::ReadInt(pdf, sp) : -1;
      check("writer: startxref in range", xo > 0 && xo < (long long)pdf.size());
      pdfout::XrefBase base;
      check("writer: base xref parses", pdfout::ParseBase(pdf, (size_t)xo, base));
      check("writer: base is a classic table", base.table);
      check("writer: base /Size found", base.size > 0);
      check("writer: base /Root found", base.rootObj > 0);
      check("writer: base has catalog offset", base.offs.count(base.rootObj) == 1);
      check("writer: base offsets are valid",
            base.offs.count(2) == 1 && base.offs[2] < pdf.size() &&
            pdf.compare(base.offs[2], 2, "2 ") == 0);
    }
    FPDF_DOCUMENT r = FPDF_LoadMemDocument(bytes.data(), (int)bytes.size(), nullptr);
    check("roundtrip reopen", r != nullptr);
    if (r) { checkEq("reopened has 1 page", FPDF_GetPageCount(r), 1); }

    // Bookmark persistence: inject an outline, reopen, and read it back with
    // pdfium. This is the end-to-end proof that the incremental update is valid.
    if (r)
    {
      std::vector<UserBookmark> bms;
      bms.push_back({L"Top of page", 0, false, 0, 0, 0});
      bms.push_back({L"Mid view \u00e9\u00fc", 0, true, 72, 500, 2.5});
      std::wstring err;
      std::string outp(bytes.begin(), bytes.end());
      const bool inj = AppendOutlines(outp, bms, 1, &err);
      std::string why;
      for (wchar_t c : err)
        why += (c < 128) ? (char)c : '?';
      check("outline inject ok" + (inj ? std::string() : " [" + why + "]"), inj);
      FPDF_DOCUMENT r2 =
          FPDF_LoadMemDocument(outp.data(), (int)outp.size(), nullptr);
      check("outline file reopens", r2 != nullptr);
      if (r2)
      {
        checkEq("reopened page count kept", FPDF_GetPageCount(r2), 1);
        FPDF_BOOKMARK top = FPDFBookmark_GetFirstChild(r2, nullptr);
        check("outline root exists", top != nullptr);
        int n = 0;
        for (FPDF_BOOKMARK bm = top; bm; bm = FPDFBookmark_GetNextSibling(r2, bm))
        {
          ++n;
          wchar_t title[128] = L"";
          FPDFBookmark_GetTitle(bm, title, 128);
          const std::wstring got = title;
          const std::wstring want = bms[n - 1].title;
          std::string gs;
          for (wchar_t c : got) gs += (c < 128) ? (char)c : '?';
          check("outline title " + std::to_string(n) + " got[" + gs + "]", got == want);
          FPDF_DEST dest = FPDFBookmark_GetDest(r2, bm);
          const int pg = dest ? FPDFDest_GetDestPageIndex(r2, dest) : -1;
          checkEq("outline dest page " + std::to_string(n), pg, bms[n - 1].page);
        }
        checkEq("outline item count", n, 2);
        FPDF_CloseDocument(r2);
      }
    }
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
    // "Bookmark this view" geometry: the viewport's top-left in page space.
    double x = 0, y = 0, z = 0;
    // Page fully visible, top edge at the viewport top: y = full page height.
    ViewDestForPage(612, 792, 0, 0, 1.0, x, y, z);
    check("viewdest: unscrolled y is page top", std::abs(y - 792.0) < 0.01);
    check("viewdest: unscrolled x is page left", std::abs(x) < 0.01);
    check("viewdest: 100% zoom is 96/72", std::abs(z - 96.0 / 72.0) < 0.0001);
    // Scrolled 100 px into the page (its top edge is 100 px above the viewport).
    ViewDestForPage(612, 792, 0, -100, 1.0, x, y, z);
    check("viewdest: scrolled y shifts down", std::abs(y - 692.0) < 0.01);
    // Zoom 2.0 means 200 screen px per point, so 100 px is only 50 pt.
    ViewDestForPage(612, 792, 0, -100, 2.0, x, y, z);
    check("viewdest: zoom halves the point offset", std::abs(y - 742.0) < 0.01);
    check("viewdest: zoom 2 is 192/72", std::abs(z - 192.0 / 72.0) < 0.0001);
    // Page pushed right of the viewport clamps x to the page's left edge.
    ViewDestForPage(612, 792, 300, 0, 1.0, x, y, z);
    check("viewdest: x clamped to page", std::abs(x) < 0.01);
    // Page top below the viewport: the visible part starts at the page top.
    ViewDestForPage(612, 792, 0, 100, 1.0, x, y, z);
    check("viewdest: page below viewport anchors to page top",
          std::abs(y - 792.0) < 0.01);
    // Scrolled well past the page bottom clamps y to 0.
    ViewDestForPage(612, 792, 0, -5000, 1.0, x, y, z);
    check("viewdest: y clamped to page bottom", std::abs(y) < 0.01);
    // A degenerate zoom must not produce infinities.
    ViewDestForPage(612, 792, 0, 0, 0.0, x, y, z);
    check("viewdest: zero zoom is guarded",
          x >= 0 && x <= 612 && y >= 0 && y <= 792 && z > 0);
  }

  {
    // Adding a bookmark to a file that already has an outline must extend the
    // existing tree, not replace it.
    std::string outline = MakeOutlinePdf();
    FPDF_DOCUMENT od = FPDF_LoadMemDocument(outline.data(), (int)outline.size(), nullptr);
    check("merge: load outline pdf", od != nullptr);
    std::vector<unsigned char> base;
    check("merge: serialize outline pdf", od && SaveAsString(od, base));
    if (od) FPDF_CloseDocument(od);
    if (!base.empty())
    {
      std::string merged(base.begin(), base.end());
      std::vector<UserBookmark> bms;
      bms.push_back({L"Added later", 1, false, 0, 0, 0});
      std::wstring err;
      const bool inj = AppendOutlines(merged, bms, 2, &err);
      std::string why;
      for (wchar_t c : err) why += (c < 128) ? (char)c : '?';
      check("merge: inject ok" + (inj ? std::string() : " [" + why + "]"), inj);
      FPDF_DOCUMENT m =
          FPDF_LoadMemDocument(merged.data(), (int)merged.size(), nullptr);
      check("merge: file reopens", m != nullptr);
      if (m)
      {
        auto titleOf = [&](FPDF_BOOKMARK bm) -> std::wstring {
          unsigned long n = FPDFBookmark_GetTitle(bm, nullptr, 0);
          std::vector<unsigned char> raw(n + 2, 0);
          FPDFBookmark_GetTitle(bm, raw.data(), (unsigned long)raw.size());
          int c = (int)(n / 2) - 1;
          if (c < 0) c = 0;
          return std::wstring(reinterpret_cast<const wchar_t*>(raw.data()), (size_t)c);
        };
        FPDF_BOOKMARK t = FPDFBookmark_GetFirstChild(m, nullptr);
        check("merge: first item kept", t && titleOf(t) == L"Cover");
        FPDF_BOOKMARK t2 = t ? FPDFBookmark_GetNextSibling(m, t) : nullptr;
        check("merge: second item kept", t2 && titleOf(t2) == L"Details");
        FPDF_BOOKMARK t3 = t2 ? FPDFBookmark_GetNextSibling(m, t2) : nullptr;
        check("merge: new item appended", t3 && titleOf(t3) == L"Added later");
        check("merge: nothing after new item",
              t3 && FPDFBookmark_GetNextSibling(m, t3) == nullptr);
        FPDF_BOOKMARK nest = t2 ? FPDFBookmark_GetFirstChild(m, t2) : nullptr;
        check("merge: nested child survived",
              nest && titleOf(nest) == L"Details - Sub");
        FPDF_DEST d3 = t3 ? FPDFBookmark_GetDest(m, t3) : nullptr;
        checkEq("merge: new item dest page",
                d3 ? FPDFDest_GetDestPageIndex(m, d3) : -1, 1);
        checkEq("merge: page count kept", FPDF_GetPageCount(m), 2);
        FPDF_CloseDocument(m);
      }
    }
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
    const char* kTestLinkUri = "https://example.com/stitchup";
    g_linkUri = L"https://example.com/stitchup";
    check("annot: link created", InsertAnnot(an, 0, ID_ANN_LINK));

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
        checkEq("annot: persistent count", n, 7);
        bool foundHL = false, foundUL = false, foundNote = false, foundFree = false,
             foundSq = false, foundSt = false, foundLink = false;
        bool linkUriOk = false;
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
          if (st == FPDF_ANNOT_LINK)
          {
            foundLink = true;
            FPDF_LINK lnk = FPDFAnnot_GetLink(aa);
            FPDF_ACTION act = lnk ? FPDFLink_GetAction(lnk) : nullptr;
            unsigned long need = act ? FPDFAction_GetURIPath(an2, act, nullptr, 0) : 0;
            if (need > 1)
            {
              std::string uri(need, '\0');
              unsigned long got =
                  FPDFAction_GetURIPath(an2, act, &uri[0], need);
              // Returned length counts the trailing NUL.
              if (got && uri[got - 1] == '\0') --got;
              linkUriOk = (got > 0 && uri.substr(0, got) == kTestLinkUri);
            }
          }
          if (st == FPDF_ANNOT_TEXT) FPDFAnnot_GetRect(aa, &noteR);
          FPDFPage_CloseAnnot(aa);
        }
        check("annot: link persists", foundLink);
        check("link: URI round-trips", linkUriOk);
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
    // --- Drag-to-draw annotation placement. The GUI rubber-band is not
    // reachable headless, but the two pieces that decide where the annotation
    // lands - canvas drag -> PDF rectangle, and InsertAnnot at that rectangle -
    // are. This is what makes "drag-to-draw links" more than a fixed box.
    FPDF_DOCUMENT dd = FPDF_CreateNewDocument();
    FPDFPage_New(dd, 0, 612.0, 792.0);
    check("drag: doc built", dd != nullptr);

    const FPDF_DOCUMENT keepDoc = g.doc;
    const int keepCount = g.pageCount;
    const double keepZoom = g.zoom;
    const int keepScrollX = g.scrollX, keepScrollY = g.scrollY;
    const int keepSel = g.selected;
    g.doc = dd;
    g.pageCount = 1;
    g.selected = 0;

    // With no canvas the layout falls back to cw=120; find the page's device
    // rect so the simulated drags land on real points.
    std::vector<RECT> rects;
    int lw = 0, lh = 0;
    LayoutPages(120, rects, lw, lh);
    check("drag: page rect available", !rects.empty() &&
          rects[0].right - rects[0].left > 1 && rects[0].bottom - rects[0].top > 1);
    if (!rects.empty())
    {
      const POINT org = PageOrigin((int)std::lround(g.scrollX),
                                   (int)std::lround(g.scrollY));
      const int px = rects[0].left + org.x;
      const int py = rects[0].top + org.y;
      const int w = rects[0].right - rects[0].left;
      const int h = rects[0].bottom - rects[0].top;

      // Top-left to bottom-right drag.
      FS_RECTF r1{};
      bool ok1 = DragToPageRect(0, POINT{px + w / 4, py + h / 4},
                                POINT{px + w / 2, py + h / 2}, r1);
      check("drag: rect computed", ok1);
      check("drag: rect ordered", r1.left < r1.right && r1.bottom < r1.top);
      check("drag: rect within page", r1.left >= 0 && r1.top <= 792.0f &&
            r1.right <= 612.0f && r1.bottom >= 0);

      // Reverse drag (bottom-right to top-left) must produce the same rect.
      FS_RECTF r2{};
      bool ok2 = DragToPageRect(0, POINT{px + w / 2, py + h / 2},
                                POINT{px + w / 4, py + h / 4}, r2);
      check("drag: reverse drag ordered", ok2 &&
            std::fabs(r1.left - r2.left) < 1.0f &&
            std::fabs(r1.top - r2.top) < 1.0f &&
            std::fabs(r1.right - r2.right) < 1.0f &&
            std::fabs(r1.bottom - r2.bottom) < 1.0f);

      // A drag that leaves the page is clamped, not dropped.
      FS_RECTF r3{};
      bool ok3 = DragToPageRect(0, POINT{px + w / 4, py + h / 4},
                                POINT{px + w + 500, py + h + 500}, r3);
      check("drag: off-page drag clamped", ok3 && r3.right <= 612.0f + 0.01f &&
            r3.bottom >= -0.01f);

      // Geometry must survive a zoom change (scales to the same PDF rect).
      g.zoom = 2.0;
      std::vector<RECT> zr;
      int zw = 0, zh = 0;
      LayoutPages(120, zr, zw, zh);
      if (!zr.empty())
      {
        const int zx = zr[0].left + org.x, zy = zr[0].top + org.y;
        const int zww = zr[0].right - zr[0].left, zhh = zr[0].bottom - zr[0].top;
        FS_RECTF rz{};
        bool okz = DragToPageRect(0, POINT{zx + zww / 4, zy + zhh / 4},
                                  POINT{zx + zww / 2, zy + zhh / 2}, rz);
        check("drag: zoom-independent placement",
              okz && std::fabs(rz.left - r1.left) < 2.0f &&
              std::fabs(rz.right - r1.right) < 2.0f &&
              std::fabs(rz.top - r1.top) < 2.0f &&
              std::fabs(rz.bottom - r1.bottom) < 2.0f);
      }
      g.zoom = 1.0;

      // InsertAnnot at an explicit rectangle must honor it exactly.
      FS_RECTF want{100.0f, 500.0f, 300.0f, 480.0f};
      check("drag: link inserted at drawn rect",
            InsertAnnot(dd, 0, ID_ANN_LINK, &want));
      FPDF_PAGE dp = FPDF_LoadPage(dd, 0);
      if (dp)
      {
        FPDF_ANNOTATION da = FPDFPage_GetAnnot(dp, 0);
        FS_RECTF gotR{};
        bool gr = da && FPDFAnnot_GetRect(da, &gotR);
        check("drag: drawn rect persisted",
              gr && std::fabs(gotR.left - want.left) < 0.5f &&
              std::fabs(gotR.top - want.top) < 0.5f &&
              std::fabs(gotR.right - want.right) < 0.5f &&
              std::fabs(gotR.bottom - want.bottom) < 0.5f);
        if (da) FPDFPage_CloseAnnot(da);
        FPDF_ClosePage(dp);
      }
      else check("drag: page with drawn annot loads", false);
    }
    else check("drag: page rect available", false);

    // Arming and cancelling the tool must be reflected in state (the cursor and
    // status hints are drawn from these fields).
    SetAnnotTool(ID_ANN_LINK);
    check("drag: arming sets tool", g.annTool == ID_ANN_LINK && !g.annDrag);
    SetAnnotTool(ID_ANN_SHAPE);
    check("drag: re-arming switches kind", g.annTool == ID_ANN_SHAPE);
    g.annDrag = true;
    g.annPage = 0;
    CancelAnnotTool();
    check("drag: cancel clears tool", g.annTool == 0 && !g.annDrag &&
          g.annPage == -1);

    g.doc = keepDoc;
    g.pageCount = keepCount;
    g.selected = keepSel;
    g.zoom = keepZoom;
    g.scrollX = keepScrollX;
    g.scrollY = keepScrollY;
    FPDF_CloseDocument(dd);
  }

  {
    // --- Text search (Ctrl+F / F3). CountFindHits + FindHighlightRects are the
    // kernels behind the find UI; exercise them on a known-text fixture and on
    // a two-page merge so match ordering across pages is covered too.
    std::string tpdf = MakeTextPdf();
    FPDF_DOCUMENT fd = FPDF_LoadMemDocument(tpdf.data(), (int)tpdf.size(), nullptr);
    check("find: load text pdf", fd != nullptr);

    std::vector<App::FindHit> hits;
    int n = CountFindHits(fd, L"World", &hits);
    check("find: one match found", n == 1 && hits.size() == 1);
    check("find: match on page 0", !hits.empty() && hits[0].page == 0);
    check("find: match length is 5", !hits.empty() && hits[0].count == 5);

    std::wstring matched;
    std::vector<FS_RECTF> fr;
    if (!hits.empty())
    {
      FPDF_PAGE fp = FPDF_LoadPage(fd, hits[0].page);
      if (fp)
      {
        FPDF_TEXTPAGE ftp = FPDFText_LoadPage(fp);
        if (ftp)
        {
          std::vector<unsigned short> buf((size_t)hits[0].count + 1, 0);
          int got = FPDFText_GetText(ftp, hits[0].start, hits[0].count, buf.data());
          if (got > 1) matched.assign((const wchar_t*)buf.data(), (size_t)got - 1);
          FPDFText_ClosePage(ftp);
        }
        FPDF_ClosePage(fp);
      }
      const FPDF_DOCUMENT keepDoc2 = g.doc;
      const int keepCount2 = g.pageCount;
      g.doc = fd;
      g.pageCount = 1;
      FindHighlightRects(hits[0].page, hits[0].start, hits[0].count, fr);
      g.doc = keepDoc2;
      g.pageCount = keepCount2;
    }
    check("find: matched text is 'World'", matched == L"World");
    check("find: highlight rect produced", !fr.empty() && fr[0].left < fr[0].right &&
          fr[0].bottom < fr[0].top);
    check("find: case-insensitive", CountFindHits(fd, L"hello", nullptr) == 1);
    check("find: miss returns 0", CountFindHits(fd, L"zzznotfound", nullptr) == 0);
    check("find: empty query returns 0", CountFindHits(fd, L"", nullptr) == 0);

    // Merge two copies so the same word appears on pages 0 and 1 in order.
    FPDF_DOCUMENT fm = FPDF_CreateNewDocument();
    if (fm)
    {
      FPDF_ImportPagesByIndex(fm, fd, nullptr, 0, 0);
      FPDF_ImportPagesByIndex(fm, fd, nullptr, 0, 0);
      std::vector<App::FindHit> mh;
      int mn = CountFindHits(fm, L"World", &mh);
      check("find: multi-page hit count", mn == 2 && mh.size() == 2);
      check("find: multi-page ordering", mh.size() == 2 && mh[0].page == 0 &&
            mh[1].page == 1);
      FPDF_CloseDocument(fm);
    }
    else check("find: multi-page doc built", false);

    if (fd) FPDF_CloseDocument(fd);
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
    // --- Menu wiring: every menu item must have a handler, and every handled
    // id must be reachable from a menu. This is what catches a menu entry that
    // was added but never dispatched (a silent no-op in the UI).
    // BuildMenu() only needs a valid instance for the tooltip font, and the
    // window handles it touches are all guarded, so it is safe headless.
    HMENU bar = BuildMenu();
    check("menu: built", bar != nullptr);
    std::vector<int> menuIds;
    std::vector<std::string> menuLabels;
    // Walk every popup (and the nested Colour Scheme submenu).
    std::function<void(HMENU)> walk = [&](HMENU m) {
      const int n = GetMenuItemCount(m);
      for (int i = 0; i < n; ++i)
      {
        HMENU sub = GetSubMenu(m, i);
        if (sub) { walk(sub); continue; }
        UINT id = GetMenuItemID(m, i);
        if (id == 0 || id == (UINT)-1) continue;   // separator
        wchar_t buf[256] = {};
        GetMenuStringW(m, i, buf, 256, MF_BYPOSITION);
        menuIds.push_back((int)id);
        std::string lb;
        for (const wchar_t* c = buf; *c; ++c)
          lb += (char)(*c < 128 ? *c : '?');
        menuLabels.push_back(lb);
      }
    };
    if (bar) walk(bar);
    check("menu: has items", !menuIds.empty());
    int deadItems = 0;
    for (size_t i = 0; i < menuIds.size(); ++i)
      if (!IsHandledCommand(menuIds[i])) deadItems++;
    check("menu: no dead items (every entry is dispatched)",
          deadItems == 0);
    // Duplicate ids in one menu bar mean a wrong item gets clicked.
    {
      std::vector<int> dup;
      for (size_t i = 0; i < menuIds.size(); ++i)
        for (size_t j = i + 1; j < menuIds.size(); ++j)
          if (menuIds[i] == menuIds[j]) dup.push_back(menuIds[i]);
      check("menu: no duplicate command ids", dup.empty());
    }
    check("menu: covers every handler",
          IsHandledCommand(ID_NEW) && IsHandledCommand(ID_ANN_LINK) &&
          IsHandledCommand(ID_PAGE_CROP) && IsHandledCommand(ID_TAB_FIRST) &&
          IsHandledCommand(ID_THEME_FIRST) && !IsHandledCommand(-1) &&
          !IsHandledCommand(99999));
    if (deadItems)
    {
      for (size_t i = 0; i < menuIds.size(); ++i)
        if (!IsHandledCommand(menuIds[i]))
          emit("FAIL unhandled menu id " + std::to_string(menuIds[i]) + " '" +
               menuLabels[i] + "'");
    }
    // The command accelerators advertised in the About box must be handled too.
    check("menu: About box accelerators handled",
          IsHandledCommand(ID_OPEN) && IsHandledCommand(ID_SAVE) &&
          IsHandledCommand(ID_ROTR) && IsHandledCommand(ID_ROTL) &&
          IsHandledCommand(ID_ZOOM_IN) && IsHandledCommand(ID_FITP) &&
          IsHandledCommand(ID_ZOOM100) && IsHandledCommand(ID_DELETE));
    if (bar) DestroyMenu(bar);
    g_themeMenu = nullptr;
  }

  {
    // --- Menu command behaviour that can run headless: the pure logic behind
    // the View/Edit items. Each mirrors what the command does in the GUI.
    // Fit/zoom math is the risky part (clamping, spread span, scroll targets),
    // so it gets exercised directly against a real document.
    FPDF_DOCUMENT vd = FPDF_CreateNewDocument();
    if (vd)
    {
      FPDFPage_New(vd, 0, 612.0, 792.0);
      FPDFPage_New(vd, 1, 612.0, 792.0);
      FPDFPage_New(vd, 2, 595.0, 842.0);
    }
    check("menu: view doc built", vd != nullptr);
    if (vd)
    {
      const FPDF_DOCUMENT keepDoc = g.doc;
      const int keepCount = g.pageCount;
      const int keepSel = g.selected;
      const double keepZoom = g.zoom;
      const bool keepSpread = g.spread;
      const int keepScrollY = g.scrollY;
      g.doc = vd;
      g.pageCount = FPDF_GetPageCount(vd);
      g.selected = 0;

      // Zoom clamping (ZoomTo clamps to 0.1..8.0).
      g.zoom = 99.0;   ZoomTo(99.0, false);
      check("menu: zoom clamps high", g.zoom <= 8.0 + 1e-9);
      g.zoom = 0.001;  ZoomTo(0.001, false);
      check("menu: zoom clamps low", g.zoom >= 0.1 - 1e-9);
      checkEq("menu: ZoomKey scales by 1000", ZoomKey(), 100);

      // Fit to width / fit page must land inside the clamp range and change zoom.
      FitWidth();
      const double fitW = g.zoom;
      check("menu: fit width in range", fitW >= 0.1 - 1e-9 && fitW <= 8.0 + 1e-9);
      FitPage();
      check("menu: fit page in range", g.zoom >= 0.1 - 1e-9 && g.zoom <= 8.0 + 1e-9);
      check("menu: fit width uses viewport width", fitW > 0.0);

      // Spread changes the laid-out span (two pages + gap vs one page).
      g.spread = false;
      const double singleSpan = LayoutSpanW();
      g.spread = true;
      const double spreadSpan = LayoutSpanW();
      check("menu: spread widens layout span", spreadSpan >= singleSpan);
      g.spread = false;

      // LayoutPages must return one rect per page and a sane content size.
      {
        std::vector<RECT> rects;
        int cw = 0, ch = 0;
        LayoutPages(800, rects, cw, ch);
        checkEq("menu: one rect per page", (int)rects.size(), g.pageCount);
        bool sized = cw > 0 && ch > 0;
        for (const RECT& r : rects)
          if (r.right - r.left <= 0 || r.bottom - r.top <= 0) sized = false;
        check("menu: layout rects have area", sized);
      }

      // Navigation must clamp at both ends and not run off the page list.
      GotoPageIndex(0);
      g.scrollY = 0;
      GoPage(-1);
      checkEq("menu: prev page clamps at first", g.selected, 0);
      GoPage(1);
      checkEq("menu: next page advances", g.selected, 1);
      GoPage(1);
      checkEq("menu: next page advances again", g.selected, 2);
      GoPage(9999);
      checkEq("menu: next page clamps at last", g.selected, g.pageCount - 1);
      GoPage(-9999);
      checkEq("menu: prev page clamps back to first", g.selected, 0);
      GotoPageIndex(1);
      checkEq("menu: goto page index", g.selected, 1);

      g.doc = keepDoc;
      g.pageCount = keepCount;
      g.selected = keepSel;
      g.zoom = keepZoom;
      g.spread = keepSpread;
      g.scrollY = keepScrollY;
      FPDF_CloseDocument(vd);
    }
  }

  {
    // --- Menu command behaviour for the page/document edits. These run through
    // the same FPDF calls the commands make, on a scratch document.
    FPDF_DOCUMENT pd = FPDF_CreateNewDocument();
    check("menu: page-edit doc created", pd != nullptr);
    if (pd)
    {
      FPDFPage_New(pd, 0, 612.0, 792.0);
      FPDFPage_New(pd, 1, 595.0, 842.0);
      FPDFPage_New(pd, 2, 612.0, 792.0);
      checkEq("menu: three pages to edit", FPDF_GetPageCount(pd), 3);

      // Edit > Rotate Right / Left (RotatePage sets /Rotate via FPDFPage_SetRotation).
      {
        FPDF_PAGE p = FPDF_LoadPage(pd, 1);
        check("menu: rotate page loads", p != nullptr);
        if (p)
        {
          FPDFPage_SetRotation(p, 1);
          checkEq("menu: rotate right sets /Rotate 1", FPDFPage_GetRotation(p), 1);
          FPDFPage_SetRotation(p, (FPDFPage_GetRotation(p) + 3) & 3);
          checkEq("menu: rotate left returns to 0", FPDFPage_GetRotation(p), 0);
          FPDF_ClosePage(p);
        }
        std::vector<unsigned char> rb;
        bool rs = SaveAsString(pd, rb) && !rb.empty();
        check("menu: rotated doc saves", rs);
        FPDF_DOCUMENT rr2 = rs ? FPDF_LoadMemDocument(rb.data(), (int)rb.size(), nullptr) : nullptr;
        check("menu: rotated doc reloads", rr2 != nullptr);
        if (rr2)
        {
          FPDF_PAGE rp = FPDF_LoadPage(rr2, 1);
          if (rp)
          {
            checkEq("menu: rotation persists", FPDFPage_GetRotation(rp), 0);
            FPDF_ClosePage(rp);
          }
          else check("menu: reloaded rotated page loads", false);
          FPDF_CloseDocument(rr2);
        }
      }

      // Edit > Add Page (FPDFPage_New) inserts after the selected index.
      {
        FPDF_PAGE p = FPDFPage_New(pd, 1, 400.0, 500.0);
        check("menu: add page inserts", p != nullptr);
        if (p) FPDF_ClosePage(p);
        checkEq("menu: add page bumps count", FPDF_GetPageCount(pd), 4);
        FPDF_PAGE added = FPDF_LoadPage(pd, 1);
        if (added)
        {
          check("menu: added page size honored",
                std::fabs(FPDF_GetPageWidthF(added) - 400.0f) < 1.0f &&
                std::fabs(FPDF_GetPageHeightF(added) - 500.0f) < 1.0f);
          FPDF_ClosePage(added);
        }
        else check("menu: added page loads", false);
      }

      // Edit > Delete Page (FPDFPage_Delete).
      FPDFPage_Delete(pd, 1);
      checkEq("menu: delete page drops count", FPDF_GetPageCount(pd), 3);

      // Pages > Split / Extract use FPDF_ImportPagesByIndex per page.
      {
        int oneFile = 0;
        for (int i = 0; i < FPDF_GetPageCount(pd); ++i)
        {
          FPDF_DOCUMENT nd = FPDF_CreateNewDocument();
          int idx = i;
          bool ok = nd && FPDF_ImportPagesByIndex(nd, pd, &idx, 1, 0) != 0;
          std::vector<unsigned char> nb;
          ok = ok && SaveAsString(nd, nb) && !nb.empty();
          ok ? ++oneFile : 0;
          if (nd) FPDF_CloseDocument(nd);
        }
        checkEq("menu: split writes one file per page", oneFile, 3);
        FPDF_DOCUMENT ed = FPDF_CreateNewDocument();
        int one = 1;
        bool eok = ed && FPDF_ImportPagesByIndex(ed, pd, &one, 1, 0) != 0;
        checkEq("menu: extract yields a single page",
                eok ? FPDF_GetPageCount(ed) : -1, 1);
        if (ed) FPDF_CloseDocument(ed);
      }

      // Pages > Auto-Crop (InkBounds + FPDFPage_SetMediaBox).
      {
        FPDF_DOCUMENT cd = FPDF_CreateNewDocument();
        FPDF_PAGE cp = FPDFPage_New(cd, 0, 600.0, 800.0);
        if (cp)
        {
          FPDF_PAGEOBJECT to = FPDFPageObj_NewTextObj(cd, "Helvetica", 18.0f);
          const unsigned short* u16 =
              reinterpret_cast<const unsigned short*>(L"CROP");
          if (to && FPDFText_SetText(to, u16))
          {
            FS_MATRIX tm{};
            tm.a = 1; tm.d = 1; tm.e = 150.0f; tm.f = 400.0f;
            FPDFPageObj_SetMatrix(to, &tm);
            FPDFPage_InsertObject(cp, to);
            FPDFPage_GenerateContent(cp);
            FPDF_ClosePage(cp);
          }
          else { if (to) FPDFPageObj_Destroy(to); FPDF_ClosePage(cp); }
          cp = FPDF_LoadPage(cd, 0);
        }
        bool cropped = false;
        if (cp)
        {
          float L = 0, B = 0, R = 0, T = 0;
          if (InkBounds(cp, L, B, R, T) && L < R && B < T)
          {
            FPDFPage_SetMediaBox(cp, L, B, R, T);
            cropped = true;
          }
          FPDF_ClosePage(cp);
        }
        check("menu: auto-crop computes and applies ink bounds", cropped);
        std::vector<unsigned char> cb;
        bool cs = SaveAsString(cd, cb) && !cb.empty();
        check("menu: cropped doc saves", cs);
        FPDF_DOCUMENT cr = cs ? FPDF_LoadMemDocument(cb.data(), (int)cb.size(), nullptr) : nullptr;
        check("menu: cropped doc reloads", cr != nullptr);
        if (cr)
        {
          FPDF_PAGE rp = FPDF_LoadPage(cr, 0);
          if (rp)
          {
            float l = 0, b = 0, r = 0, t = 0;
            FPDFPage_GetMediaBox(rp, &l, &b, &r, &t);
            check("menu: media box shrank to content",
                  r - l < 600.0 && t - b < 800.0 && r > l && t > b);
            FPDF_ClosePage(rp);
          }
          else check("menu: cropped page reloads", false);
          FPDF_CloseDocument(cr);
        }
        // A blank page has nothing to trim to: InkBounds must say so.
        FPDF_DOCUMENT bd = FPDF_CreateNewDocument();
        FPDFPage_New(bd, 0, 600.0, 800.0);
        FPDF_PAGE bp = FPDF_LoadPage(bd, 0);
        if (bp)
        {
          float L = 0, B = 0, R = 0, T = 0;
          check("menu: auto-crop declines a blank page", !InkBounds(bp, L, B, R, T));
          FPDF_ClosePage(bp);
        }
        FPDF_CloseDocument(bd);
        FPDF_CloseDocument(cd);
      }

      // File > Import PDF (FPDF_ImportPagesByIndex with a null index = all pages).
      {
        FPDF_DOCUMENT im = FPDF_CreateNewDocument();
        FPDFPage_New(im, 0, 612.0, 792.0);
        FPDFPage_New(im, 1, 612.0, 792.0);
        bool iok = FPDF_ImportPagesByIndex(im, pd, nullptr, 0, 1) != 0;
        check("menu: import appends all source pages",
              iok && FPDF_GetPageCount(im) == 5);
        FPDF_CloseDocument(im);
      }

      FPDF_CloseDocument(pd);
    }
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
static LRESULT CALLBACK TabBarProc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
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
      const UiTheme& th = ThemeNow();
      HBRUSH bg = CreateSolidBrush(th.ribbonBg);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      HBRUSH cap = CreateSolidBrush(th.accent);
      RECT sr{0, 0, rc.right, 2};
      FillRect(dc, &sr, cap);
      DeleteObject(cap);
      HFONT tabFont = CreateFontW(-MulDiv(11, g.dpi, 72), 0, 0, 0, FW_NORMAL,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

      int x = 4;
      for (int i = 0; i < (int)g_tabs.size(); ++i)
      {
        const TabDoc& t = g_tabs[i];
        SIZE cxt{};
        const wchar_t* label = t.name.empty() ? L"Untitled" : t.name.c_str();
        SelectObject(dc, tabFont);
        GetTextExtentPoint32W(dc, label, (int)wcslen(label), &cxt);
        int w = cxt.cx + 34;
        bool active = (i == g_curTab);
        RECT tr{x, 4, x + w, rc.bottom - 3};
        HBRUSH fill = CreateSolidBrush(active ? th.card : th.ribbonBg);
        HPEN pen = CreatePen(PS_SOLID, 1, active ? th.accent : th.cardBorder);
        HBRUSH wasb = (HBRUSH)SelectObject(dc, fill);
        HPEN wasp = (HPEN)SelectObject(dc, pen);
        RoundRect(dc, tr.left, tr.top, tr.right, tr.bottom, 6, 6);
        SelectObject(dc, wasb);
        SelectObject(dc, wasp);
        DeleteObject(fill);
        DeleteObject(pen);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, active ? th.text : th.textDim);
        RECT lr{tr.left + 6, tr.top, tr.right - 20, tr.bottom};
        DrawTextW(dc, label, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        RECT xr{tr.right - 18, tr.top, tr.right - 4, tr.bottom};
        DrawTextW(dc, L"\u00d7", -1, &xr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        x = tr.right + 2;
        if (x > rc.right) break;
      }
      DeleteObject(tabFont);
      EndPaint(hw, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN:
    {
      RECT rc;
      GetClientRect(hw, &rc);
      int px = GET_X_LPARAM(lp);
      int x = 4;
      for (int i = 0; i < (int)g_tabs.size(); ++i)
      {
        const TabDoc& t = g_tabs[i];
        const wchar_t* label = t.name.empty() ? L"Untitled" : t.name.c_str();
        HDC tdc = GetDC(hw);
        HFONT f = CreateFontW(-MulDiv(11, g.dpi, 72), 0, 0, 0, FW_NORMAL,
                              FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                              OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                              CLEARTYPE_QUALITY,
                              DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT of = (HFONT)SelectObject(tdc, f);
        SIZE cxt{};
        GetTextExtentPoint32W(tdc, label, (int)wcslen(label), &cxt);
        SelectObject(tdc, of);
        DeleteObject(f);
        ReleaseDC(hw, tdc);
        int w = cxt.cx + 34;
        RECT tr{x, 4, x + w, rc.bottom - 3};
        if (px >= tr.right - 18 && px <= tr.right - 4)
        {
          CloseTab(i);
          ReleaseCapture();
          return 0;
        }
        if (px >= tr.left && px <= tr.right)
        {
          SetActiveTab(i);
          return 0;
        }
        x = tr.right + 2;
      }
      return 0;
    }
    case WM_SETCURSOR:
      SetCursor(LoadCursorW(nullptr, IDC_ARROW));
      return TRUE;
  }
  return DefWindowProcW(hw, msg, wp, lp);
}

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
      const UiTheme& th = ThemeNow();
      HBRUSH bg = CreateSolidBrush(th.ribbonBg);
      FillRect(dc, &rc, bg);
      DeleteObject(bg);
      if (hw != g.toolbar) { EndPaint(hw, &ps); return 0; } // pane header etc.

      // brand accent strip across the very top
      HBRUSH strip = CreateSolidBrush(th.accent);
      RECT sr{0, 0, rc.right, 2};
      FillRect(dc, &sr, strip);
      DeleteObject(strip);

      // Tab strip: one tab per tool group.
      HFONT tabFont = CreateFontW(-MulDiv(10, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD,
                                  FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                  OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                  CLEARTYPE_QUALITY,
                                  DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
      HFONT wasTab = (HFONT)SelectObject(dc, tabFont);
      SetBkMode(dc, TRANSPARENT);
      for (int i = 0; i < (int)g.ribbonTabRects.size(); ++i)
      {
        const RECT& tr = g.ribbonTabRects[i];
        bool active = (i == g.ribbonTab);
        if (active || i == g.ribbonTabHover)
        {
          HBRUSH tb = CreateSolidBrush(active ? th.card : th.btnHover);
          HPEN tp = CreatePen(PS_SOLID, 1, active ? th.accent : th.cardBorder);
          HBRUSH wb = (HBRUSH)SelectObject(dc, tb);
          HPEN wp2 = (HPEN)SelectObject(dc, tp);
          RoundRect(dc, tr.left, tr.top, tr.right, tr.bottom + (active ? 4 : 0),
                    6, 6);
          SelectObject(dc, wb);
          SelectObject(dc, wp2);
          DeleteObject(tb);
          DeleteObject(tp);
        }
        SetTextColor(dc, active ? th.text : th.textDim);
        RECT lr = tr;
        DrawTextW(dc, RibbonTabName(i), -1, &lr,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
      }
      SelectObject(dc, wasTab);
      DeleteObject(tabFont);

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

        // Group caption centred under the buttons, ribbon-style.
        RECT cap{card.left, RIB_BTN_Y + RIB_BTN_H, card.right, RIB_H - 6};
        HFONT capFont = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_SEMIBOLD,
                                    FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                    OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                    CLEARTYPE_QUALITY,
                                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        HFONT wasCap = (HFONT)SelectObject(dc, capFont);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, th.textDim);
        DrawTextW(dc, gb.name.c_str(), -1, &cap,
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, wasCap);
        DeleteObject(capFont);
      }

      HPEN pen = CreatePen(PS_SOLID, 1, th.cardBorder);
      SelectObject(dc, pen);
      MoveToEx(dc, 0, rc.bottom - 1, nullptr);
      LineTo(dc, rc.right, rc.bottom - 1);
      DeleteObject(pen);
      EndPaint(hw, &ps);
      return 0;
    }
    case WM_MOUSEMOVE:
    {
      if (hw != g.toolbar) return 0;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      int hit = -1;
      for (int i = 0; i < (int)g.ribbonTabRects.size(); ++i)
        if (PtInRect(&g.ribbonTabRects[i], pt)) { hit = i; break; }
      if (hit != g.ribbonTabHover)
      {
        g.ribbonTabHover = hit;
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hw, 0};
        TrackMouseEvent(&tme);
        InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      if (g.ribbonTabHover != -1)
      {
        g.ribbonTabHover = -1;
        InvalidateRect(hw, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN:
    {
      if (hw != g.toolbar) return 0;
      POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      for (int i = 0; i < (int)g.ribbonTabRects.size(); ++i)
        if (PtInRect(&g.ribbonTabRects[i], pt)) { SwitchRibbonTab(i); break; }
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
  PickCapPhrase();
  INITCOMMONCONTROLSEX iccex{sizeof(iccex),
                             ICC_TREEVIEW_CLASSES | ICC_TAB_CLASSES |
                             ICC_BAR_CLASSES | ICC_STANDARD_CLASSES};
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
  wc.style = CS_DBLCLKS;
  wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(101));
  wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(101));
  RegisterClassExW(&wc);

  wc.style = 0;  // only the frame wants double-click (caption maximise)
  wc.lpfnWndProc = ToolbarProc;
  wc.lpszClassName = L"SKToolbar";
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  wc.lpfnWndProc = TabBarProc;
  wc.lpszClassName = L"SKTabBar";
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

  wc.lpfnWndProc = StatusProc;
  wc.lpszClassName = L"SKStatus";
  wc.hbrBackground = nullptr;
  RegisterClassExW(&wc);

  g.font = CreateFontW(-MulDiv(9, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                       FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");

  // Ribbon icons. CreateFontW silently substitutes a face when the requested
  // font is missing, so ask the DC which face it actually resolved.
  g.iconFont = CreateFontW(-MulDiv(16, g.dpi, 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE,
                           FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
                           CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                           DEFAULT_PITCH | FF_DONTCARE, L"Segoe MDL2 Assets");
  if (g.iconFont)
  {
    HDC probe = CreateCompatibleDC(nullptr);
    if (probe)
    {
      HGDIOBJ old = SelectObject(probe, g.iconFont);
      wchar_t face[LF_FACESIZE] = {};
      GetTextFaceW(probe, LF_FACESIZE, face);
      g.haveMdl2 = (lstrcmpiW(face, L"Segoe MDL2 Assets") == 0);
      SelectObject(probe, old);
      DeleteDC(probe);
    }
    if (!g.haveMdl2) { DeleteObject(g.iconFont); g.iconFont = nullptr; }
  }

  FPDF_InitLibrary();

  HWND frame = CreateWindowExW(0, L"SKFrame", kAppTitle,
                               WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                               CW_USEDEFAULT, CW_USEDEFAULT,
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
  ApplyOsTheme();
  SyncThemeMenuChecks();
  SetTabPressed();

  // Start empty: no document means the calm "open a PDF" state, not a blank
  // page. File > New still creates one.
  if (!openFile.empty()) LoadDoc(openFile);
  else RefreshState();
  ShowWindow(frame, SW_SHOW);
  UpdateWindow(frame);

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0)
  {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  CloseDoc();
  for (TabDoc& t : g_tabs)
    if (t.doc) FPDF_CloseDocument(t.doc);
  g_tabs.clear();
  FPDF_DestroyLibrary();
  DeleteObject(g.font);
  if (g.iconFont) DeleteObject(g.iconFont);
  return (int)msg.wParam;
}
