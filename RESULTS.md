# RESULTS - Project 027: Stitchup PDF Editor

## Goal

Portable Windows PDF editor + viewer with a Nitro Pro v7-style layout, using the
PDFium engine and native Win32 API. Replaces the 026 CLI model app, which the
user rejected as a product (no GUI).

## Deliverables

- `dist\Stitchup.exe` - GUI application (portable, static CRT)
- `dist\pdfium.dll` - PDFium engine (chromium/8057, BSD-3-Clause)
- `src\app.cpp` - single-file Win32 GUI + PDFium integration (GUI, editing
  operations, demo/self-test modes)
- `CMakeLists.txt`, `Build.cmd` - build scripts (MSVC/CMake/NMake)
- `third_party\pdfium\` - PDFium headers, import lib, licenses, version info
- `example.pdf` - sample 5-page demo document (via `--demo`)

## Verification (self-test, headless)

Command: `Stitchup.exe --self-test`

Result: **105 passed, 0 failed** (exit code 0).

Coverage: PDFium init; create doc + add page; save to buffer (PDF header and
`%%EOF` trailer checks); reopen roundtrip; page count; load sample PDF (612x792);
render page bitmap to BGRA (non-white pixel ink check); save-as-copy; virtual
rotate (width/height swap); merge/import pages; delete page to 0; file save +
reopen + page count; outline/bookmark fixture (root present, top-level siblings,
nested child, `/Count`, UTF-16LE titles via `FPDFBookmark_GetTitle`, destination
page resolution via `FPDFDest_GetDestPageIndex`); annotations: create all six
types (highlight/underline/note/free-text/shape/stamp), save + reload, persisted
count, subtype round-trip, note rect sanity, appearance rendering (annotated
render produces ink where the plain render has none); links: hit-testing a
hand-crafted fixture (`FPDFLink_GetLinkAtPoint` positive inside both the
internal-destination and URI link rects, negative outside), enumeration
(`FPDFLink_Enumerate` = 2), internal destination tracking to the correct page
(`FPDFDest_GetDestPageIndex` = 1), and URI extraction round-trip
(`FPDFAction_GetURIPath` returns `https://example.com/stitchup`); page
management: `DocToFile` save + reopen (3 pages round-trip), extract one page via
`FPDF_ImportPagesByIndex` (count = 1, page keeps its 700x500 size), split each
page to its own 1-page document, and auto-crop via `InkBounds`: a 1:1 render's
non-white bounding box (72, 672, 283, 738 on the sample) trimmed the MediaBox to
match (left 71..73, bottom 669..674, right 282..287, top 730..744); text export:
a hand-crafted single-page text PDF produces the expected run via
`FPDFText_LoadPage/CountChars/GetText`, `ExportTextToFile` writes a UTF-8 BOM
(+ BOM checked byte-for-byte, exported run present), and a blank document yields
no output file; security: RFC 1321 MD5 and RC4 known-answer vectors, a
hand-built standard-security-handler fixture (padded `/O`+`/U` of the real
32-byte PDF padding string, `/P` = -4, RC4 content stream encrypted with the
per-object key `MD5(K || objnum || gen)` truncated to `min(keylen+5, 16)`)
proves the revision-2 40-bit variant opens with the correct password
(`FPDF_GetLastError` = `FPDF_ERR_PASSWORD` for wrong/empty passwords), and the
on-disk fixture reopens with the same password via `FPDF_LoadDocument`.

`test_result.txt` (SHA-256 of the run captured in the report):
```
-- enter -- ... SUMMARY 105 passed, 0 failed
```

GUI verification (programmatic, window handles + messages):
- Ribbon renders Home/View/Tools tabs; `WM_COMMAND` on tab control IDs switches
  button sets per group (View shows Zoom/Navigate/Panes, Home hides); Home now
  has a third "Annotate" group (Highlight/Underline/Note/Text Box/Shape/Stamp).
- Pane header tabs switch Page Thumbnails <-> Bookmarks (thumbnail list vs
  `SysTreeView32` visibility toggles).
- Bookmarks tree populated from an outline PDF on disk: TVM_GETCOUNT = 3
  (2 top-level items + 1 nested child); selecting a bookmark jumps pages.
- CLI file argument opens a PDF at startup; window title shows the file name.
- Highlight insert via ribbon command + Save-in-place: saved PDF gains an
  `/Annots` entry with `/Subtype/Highlight`, QuadPoints, Rect, color, `/F 4`
  print flag and the Contents string (verified in raw file bytes).
- Link activation: opening a 3-page PDF with an internal link on page 1 and
  clicking its region (WM_LBUTTONDOWN at the page coordinates) jumps the canvas
  to the destination page (vertical scroll position went 0 -> 690 in one step);
  hovering a link region shows the hand cursor (IDC_HAND via WM_SETCURSOR).
- Page management (Home > Pages group has Extract/Split/Auto-Crop buttons with
  IDs 4030/4031/4032 — verified present): Extract opens the Save dialog for the
  current page; Split wrote `linked3 - split - page 1..3.pdf` (one 1-page file
  per page) from a 3-page document and confirmed the result in a message box;
  each output is a valid %PDF (page 1 carries its two link annotations).
- Text export (File > Export Text, menu id 4033): with a text-bearing PDF the
  command opens the Save As dialog prefilled with `<name>.txt`; on a blank
  document it shows the "No extractable text" box instead (both observed
  through the UI). The engine writes a UTF-8 BOM file (verified headless).
- Password-protected open: launching with an encrypted fixture raises the
  "Password required" dialog (class `SKPwdWnd`) with an ES_PASSWORD entry and
  Ok/Cancel buttons (both present and inside the dialog bounds); entering the
  correct password opens the document (title becomes
  `stitchup_enc_test.pdf - Stitchup PDF Editor`); a wrong password re-prompts
  twice and then shows the "The password was incorrect (3 attempts)." box, after
  which no document is loaded (title stays `Stitchup PDF Editor`).

## Bug fixed during this session

- **Save-in-place hung after opening a PDF**: PDFium keeps the loaded file
  handle open, so the atomic `MoveFileExW` replace hit a sharing violation and
  showed a modal error box. Now the document is released (serialized fully to a
  buffer first) before the file is replaced, then reloaded. Verified: the
  annotate + save cycle completes instantly and persists the annotation.
- Save-As onto the currently-open file now routes through the same path.

GUI smoke test: window opens ("Stitchup PDF Editor"), message loop stays alive.

## Portability

`dumpbin /dependents` on `dist\Stitchup.exe`:

```
GDI32.dll  USER32.dll  SHELL32.dll  COMDLG32.dll  COMCTL32.dll
pdfium.dll  KERNEL32.dll
```

No CRT DLLs (static `/MT` linked). Portable package = `Stitchup.exe` +
`pdfium.dll` in one folder.

## Artifacts (SHA-256)

| File           | Size     | SHA-256                                                           |
|----------------|----------|-------------------------------------------------------------------|
| Stitchup.exe   | 341,504 B | 63F2852A01EC8975F1F904DA1ACBFDE3300942F08ED68CEEB2162E7ACE6CCFD4 |
| pdfium.dll     | 7,375,360 B | 55E7EBEF29A1EC9523D1ADB8B260A73E7DFB0F64D3F0285121D20ECD6148EF18 |
| example.pdf    | 1,034 B   | B276682EFD75780E462C17489176D710AD1339D84E69C6218AD3C2F8E60FC132 |

## Notes

- Prebuilt PDFium is a DLL + import lib; static single-exe build deferred
  (requires building PDFium from source with static CRT).
- `FPDF_RenderPageBitmapWithMatrix` renders nothing on this build/scale path;
  the plain `FPDF_RenderPageBitmap` API is used instead (verified via an
  isolated probe linked against the same pdfium.dll.lib).
- Project 026 (CLI model app) remains complete and reusable; its engine
  learning carried into this GUI rebuild.

## Status

Nitro-style UI shell (Phase B) COMPLETE and verified. Phase C in progress:
annotation engine done (highlight, underline, sticky note, free text, shape,
stamp) with persistence and rendered appearances; link activation done (click
navigates or opens a URI, hand cursor); page management done (extract current
page to its own PDF, split a document into one file per page, auto-crop the
current page to its ink bounding box via the InkBounds render pass); text
export added (File > Export Text writes a BOM'd UTF-8 .txt of every page's
extractable text); security started (password-protected PDFs are detected and
unlocked through a modal password dialog, up to 3 attempts before an error
box); the Save-in-place file-replace bug is fixed. Remaining Phase C: export
(CSV), watermarks, and encrypt-on-save.
Self-test 105/105. User verdict pending — open `dist\Stitchup.exe file.pdf` and
try Home > Annotate, click any PDF link, Home > Pages (Extract / Split /
Auto-Crop), File > Export Text, and open a password-protected PDF.