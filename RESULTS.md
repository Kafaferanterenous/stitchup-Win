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
- `resources\app.ico` + `resources\app.rc` - embedded multi-size app icon
- `third_party\pdfium\` - PDFium headers, import lib, licenses, version info
- `example.pdf` - sample 5-page demo document (via `--demo`)

## Verification (self-test, headless)

Command: `Stitchup.exe --self-test`

Result: **292 passed, 0 failed** (exit code 0).

Select / Move Content Object (`obj:` checks covering content-object editing):
clicking a content object (text run or vector path) on the current page selects
it (accent bounding box + handles); **Move** drags the object via
`FPDFPageObj_Transform` and the offset persists through save + reload
(x +10 / y +5 verified on reopen); **Delete** removes the object from the page's
object array immediately (2 objects -> 1, the survivor is the path); **Recolor**
applies `FPDFPageObj_SetFillColor` and the new fill survives save + reopen
(`GetFillColor` returns the recolored R/G/B/A on reload); **Edit Text**
(double-click) detaches the selected text object and installs a new text object,
and after save + reload the edited string is present while the original run is
gone (verified via `FPDFPageObj_GetType`, the edited doc's `FPDFText_LoadPage`
run, and a `FlateDecode`-inflated content-stream text probe).

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
on-disk fixture reopens with the same password via `FPDF_LoadDocument`. CSV
export: a text-bearing fixture produces the expected per-page row
`1,612.0,792.0,25,0` (pdfium appends the trailing line break to the 24-character
run), the file carries a UTF-8 BOM byte-for-byte with the header line
`Page,Width (pt),Height (pt),Text chars,Annotations`, a 2-page doc yields one
  row per page, an empty doc makes `ExportCsvToFile` return false and writes no
  file, and a doc with a highlight annotation reports `0,1` in the annotation
  column. Text search: the `find:` checks run `CountFindHits` / `FindHighlightRects`
  on a known-text fixture (one match, correct page, 5-character span whose text
  reads back as `World`, case-insensitive hit, miss and empty query both 0, a
  highlight rectangle produced) and on a two-copy merge to confirm matches on
  pages 0 and 1 are collected in document order.
- Watermark (`ApplyWatermarkDoc`): empty text rejected; center/top/tiled modes
  each apply and are extractable; every page of a 2-page doc gains the
  watermark; a watermarked doc saves and round-trips with the watermark
  retained, and the same holds for a doc re-opened from disk after
  watermarking.
- Encrypt-on-save (`EncryptPdfBytes`, PDF revision 3 / 128-bit RC4 standard
  security handler): the writer produces output from a source document, the
  bytes differ from the plaintext, and the output carries `/Encrypt`, the
  `/Filter /Standard` handler, `/V 2 /R 3 /Length 128`, and the user password
  is not stored in the clear. The encrypted file reopens against the bundled
  PDFium with the correct password (page count = 1, the content stream
  decrypts and yields the sample text run "Stitchup PDF Editor"), while a
  wrong password returns `FPDF_ERR_PASSWORD`, an empty password is rejected,
  and a copy written to disk reopens with the same password. Key derivation
  follows the ISO/PDFium chain: `U` is built from 20 chained RC4 passes
  (`key, key^1 .. key^19`) over `MD5(padding||ID)[0..16]` with
  `U[16..32] = MD5(U[0..16])`, `O` = 50x MD5(pad_owner) then RC4, the file key
  = `MD5(pad_u || O || P(-4) || ID)` followed by 50x MD5, and each content
  object is encrypted with `MD5(fileKey[0..15] || objnum_LSB || gen_LSB)`
  truncated to the key length.

`test_result.txt` (SHA-256 of the run captured in the report):
```
-- enter -- ... SUMMARY 185 passed, 0 failed
```

GUI verification (programmatic, window handles + messages):
- Ribbon menus merged (v0.8.1): the tab strip above the ribbon is gone;
  Home / Tools (and Sidebar) are now top-level menus between View and Help
  (order: File Edit Pages Annotate View Home Tools Sidebar Help). Selecting
  Home or Tools switches the ribbon tab; SetTabPressed keeps the menu radio
  (Home <-> Tools) and the Sidebar / Two-Page Spread check marks in sync.
  `WM_COMMAND` on the tab command IDs still switches button sets per group
  (View shows Zoom/Navigate/Panes, Home hides); Home has a third "Annotate"
  group (Highlight/Underline/Note/Text Box/Shape/Stamp) and a fourth "Content"
  group (Select/Edit Text/Delete/Recolor).
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
- CSV export (File > Export CSV, menu id 4034): with a PDF loaded the command
  opens the Save As dialog (owned by the app process, title "Save As")
  prefilled with `<name>.csv` (verified via UI Automation on the filename
  field); the engine writes a UTF-8 BOM CSV (verified headless).
- Watermark (File > Watermark, menu id 4036): with a PDF loaded the
  `SKWmWnd` dialog opens (all controls present and inside bounds, Ok = id 1,
  Cancel = id 2, size/position combo boxes default to 36 pt / Center). The
  dialog prefills the text edit with "Confidential" so a press of Ok stamps
  the document. Doing exactly that from the harness (accept the default, Ok,
  then File > Save / id 4003) produced a saved fixture whose FlateDecode
  content streams, once inflated, contain the watermark drawing operators on
  every page: the UTF-16BE hex string `[<436F6E666964656E7469616C>] TJ`,
  Helvetica at 36 pt (`/FXF1 36 Tf`), the 45-degree rotation matrix
  (0.70710677 / -0.70710677), filler-gray fill `.54901963 rg` (140/255), and
  the `/FXE2 gs` 50%-alpha ExtGState (raw object `<</ca .5019608>>`); 5 pages,
  5 watermark text placements, saved file grew 1,034 -> 3,755 bytes.
- Theme: View > Dark Mode (`Ctrl+D` on the menu, checkbox state toggles)
  repaints every surface and persists to `HKCU\Software\StitchupPDFEditor`.
  A pixel probe of the toolbar (window DC, `GetPixel`) confirms both palettes
  render: light = accent strip `0B6CE0` / ribbon `F7F8FA` / card `EFF0F3`,
  dark = accent strip `4CA0FF` / ribbon `202020` / card `282828`).
- Encrypt-on-save (File > Save As Encrypted, menu id 4037): the command opens
  the `SKEpwWnd` "Set password" dialog with three ES_PASSWORD entries (user,
  confirm, optional owner) and Ok = id 1 / Cancel = id 2; a mismatched confirm
  shows the "Passwords do not match." box and the dialog stays open; an empty
  user password is rejected; matching non-empty passwords with an owner
  password close the dialog and open the Save As dialog in the app process
  ("Save As", class `#32770`), which cancels cleanly and leaves the frame
  healthy with the document still open.
- App icon: `ExtractAssociatedIcon` on the built exe yields the embedded
  multi-size icon (16/24/32/48/64/128/256, generated programmatically as
  `resources/app.ico`, compiled via `resources/app.rc`). The v0.8.1 icon draws
  a parchment scroll (rolled ends, dowel caps, text lines) with a diagonal
  inked quill; the 32x32 view resolves cleanly to a scroll shape with a pen.
- Tabs, sidebar + spread (menu-driven, phase-D work): multi-document tabs
  implemented (File > New Tab / Close Tab / Next / Previous, `Ctrl+T`,
  `Ctrl+Shift+F4`, `Ctrl+Tab` / `Ctrl+Shift+Tab`, plus a clickable tab strip
  above the ribbon with close buttons); each tab owns its doc, page, zoom and
  scroll state and switches without losing the others; File > Sidebar
  (`F8`) toggles the left pane; View > Two-Page Spread (`F5`) switches the
  canvas to side-by-side rendering; pages are horizontally centered in the
  canvas (single and spread modes).

## Bug fixed during this session

- **Save-in-place hung after opening a PDF**: PDFium keeps the loaded file
  handle open, so the atomic `MoveFileExW` replace hit a sharing violation and
  showed a modal error box. Now the document is released (serialized fully to a
  buffer first) before the file is replaced, then reloaded. Verified: the
  annotate + save cycle completes instantly and persists the annotation.
- Save-As onto the currently-open file now routes through the same path.

## Fixed / changed for v0.8.1

- **Thumbnails showed blank/ghost pages and a broken scrollbar**: the
  thumbnails filled asynchronously, and the thumbnails scrollbar range was never
  derived from the thumbnails content. `RefreshState` now calls
  `ClearThumbCache` + `ResetThumbScroll`; `ResetThumbScroll` sets the SB_VERT
  range from content height vs. client height and is also called on
  thumbnails-pane resize (WM_SIZE); `ThumbForY` geometry now matches the paint
  pass (thumbW = pane width - 26, 34px offset, +16 spacing). The pane repaints
  into a memory DC and blits once, and the focus-hotspot invalidates that caused
  the flash on WM_SETFOCUS / WM_KILLFOCUS were removed.
- **Flicker on movement/clicking**: the canvas and thumbnails panes now paint
  double-buffered (memory DC + single BitBlt); `WindowProc`-driven repaint
  invalidations that used `TRUE` (erasing the background behind the new frame)
  now use `FALSE` (Clip returns e.g. thumbnails scroll, thumbnails
  mouse-down/move/up) and the frame window sets `WS_CLIPCHILDREN`. Ctrl+wheel
  zoom intentionally keeps a full repaint since it happens rarely.
- **Two-page spread displayed but didn't fit**: the fit-width/fit-page
  calculations used the single-page `MaxPageW`, so a spread overflowed the
  viewport. A new `LayoutSpanW` returns the spread-aware row width
  (`PageW(i)+PageW(i+1)+12` when a spread is active, else `MaxPageW`), and both
  `FitWidth` and `FitPage` now use it; `ToggleSpread` calls `FitWidth` and then
  re-scrolls to the selected page, so enabling side-by-side always brings the
  pair into view.
- **Menu merge**: the three-tab ribbon strip (Home/View/Tools) was collapsed
  into top-level menus Home, Tools and Sidebar between View and Help. The
  ribbon now runs ~26px shorter (constant set: `TAB_H 30, RIB_BTN_Y 8,
  RIB_BTN_H 34, RIB_CAP_Y 48, RIB_H 66`; `RIB_TAB_H` removed) and shows the
  Home groups by default. `SetTabPressed` maintains the Home/Tools radio check
  plus the Sidebar and Two-Page Spread check marks.
- New multi-size app icon (scroll + quill) authored programmatically
  (System.Drawing, JSON-free), DIB-encoded for <=128px and PNG for 256px so it
  loads both in Explorer and in the exe.

GUI smoke test after v0.8.1: window opens, menu bar carries the merged Home /
Tools / Sidebar items, Home groups render, thumbnails scroll in step with the
page list, dark theme still applies, and side-by-side spread fits the viewport.

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
| Stitchup.exe   | 464,384 B | 253A6985050B88F4013D1C59BCE5E1B97D9433AA6CB1129817BC7AC10997EC77 |
| pdfium.dll     | 7,375,360 B | 55E7EBEF29A1EC9523D1ADB8B260A73E7DFB0F64D3F0285121D20ECD6148EF18 |
| example.pdf    | 1,034 B   | B276682EFD75780E462C17489176D710AD1339D84E69C6218AD3C2F8E60FC132 |
| app.ico        | 112,219 B | 3C7D09689F58EA3E08CE29CBB366CBCC2CFE974ADFC11E70B8D2FBB1B7C6A970 |

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
stamp, link) with persistence and rendered appearances; link activation done
(click navigates or opens a URI, hand cursor); link creation done
(Home > Annotate "Link..." prompts for a web address, blank = same-document
jump, and writes a /Link annotation via FPDFPage_CreateAnnot +
FPDFAnnot_SetURI, so the bundled PDFium runtime suffices - no upgrade needed);
page management done (extract current
page to its own PDF, split a document into one file per page, auto-crop the
current page to its ink bounding box via the InkBounds render pass); text
export added (File > Export Text writes a BOM'd UTF-8 .txt of every page's
extractable text); CSV export added (File > Export CSV writes a BOM'd UTF-8
`.csv` with one `Page,Width (pt),Height (pt),Text chars,Annotations` row per
page); UI restyled (modern flat ribbon with accent brand strip, labelled group
cards, hover/pressed button states, and a light/dark theme with an embedded
app icon); security started (password-protected PDFs are detected and
unlocked through a modal password dialog, up to 3 attempts before an error
box); the Save-in-place file-replace bug is fixed. Watermarks now done
(File > Watermark stamps every page at 36 pt / 45 degrees / filler gray /
50% alpha, prefilled "Confidential", with size 16-96 pt and Center / Top /
Tiled placements; verified end-to-end through the saved file's content
streams). Encrypt-on-save now done (File > Save As Encrypted writes a
PDF revision 3 / 128-bit RC4 password-protected copy with user + optional
owner passwords; verified end-to-end, including reopening the encrypted file
against the bundled PDFium with the correct password). Phase C COMPLETE —
no remaining feature milestones.
Self-test 185/185. v0.8.1 released (private) with the post-v0.8.0 fixes:
thumbnails render in step with the page list and scroll correctly, canvas and
thumbnails repaint without flicker, the two-page spread fits the viewport, the
Home/View/Tools tab strip is merged into the menu bar (Home / Tools / Sidebar
top-level items between View and Help), and the app ships a new scroll-and-quill
icon. User verdict pending — open `dist\Stitchup.exe file.pdf` and try
Home > Annotate, click any PDF link, Home > Pages (Extract / Split /
Auto-Crop), File > Export Text / Export CSV / Watermark / Save As Encrypted,
open a password-protected PDF, and the new view extras: File > New Tab (Ctrl+T),
File > Close / Next / Previous Tab, View > Two-Page Spread (F5), View >
Sidebar / Sidebar menu (F8), and the Home / Tools ribbon-switch menus.

## Latest — v0.10.0: link creation (2026-10-01)

- The earlier roadmap note ("link *creation* needs a newer PDFium") was wrong:
  a hyperlink is a `/Link` annotation, and the bundled `chromium/8057` DLL
  already exports both `FPDFPage_CreateAnnot` and `FPDFAnnot_SetURI`. The newer
  `chromium/8076` build exports exactly the same set, so no runtime upgrade was
  needed or made.
- New command `ID_ANN_LINK` (`src/app.cpp`): Home > Annotate "Link..." /
  Edit > Annotate > Link... opens a `PromptLinkUri` dialog asking for a web
  address, then places a /Link annotation on the current page with the URI as
  its action. Page-destination targets are not offered: PDFium has no
  destination-writing API (no `FPDFLink_SetDest*` is declared or exported), so a
  blank target would be a dead link and the dialog rejects empty input.
- `PromptBookmarkName` was generalized into `PromptText(caption, prompt,
  allowEmpty)` so bookmarks and link targets share one dialog implementation.
- Link rectangle is a default 190x20 pt box at 12%/62% of the page; drag-to-draw
  is not implemented yet. Two limits found while implementing: only
  `FPDFANNOT_COLORTYPE_Color` / `_InteriorColor` exist (there is no Border
  colour type), and `FPDFAction_GetURIPath` returns a length *including* the
  trailing NUL.
- Self-test grew from 223 to 226 checks: link creation, link persistence after
  save+reload (annot count 6 -> 7), and URI round-trip via
  `FPDFLink_GetAction` + `FPDFAction_GetURIPath` (the returned length includes
  the trailing NUL). Result: 226 passed, 0 failed. App smoke-launches.

## v0.10.1 - menu function verification

A static audit of `DoCommand()` against `BuildMenu()` confirmed all 45 command
ids were dispatched, but nothing *tested* that. This release closes that gap.

- Added `IsHandledCommand(int)` (`src/app.cpp:5527`) - the set of ids
  `DoCommand()` acts on, including the two contiguous radio blocks
  (`ID_TAB_FIRST..ID_TAB_LAST` and the theme range).
- New `menu:` self-test block (`src/app.cpp:8082`) calls the real `BuildMenu()`,
  walks every popup recursively, and asserts in both directions: no menu item is
  unhandled (no dead entries), no duplicate command ids (which would make one
  item fire another's action), and no orphan handlers. Any unhandled id is
  reported by name in the log, e.g. `unhandled menu id 1024 'Foo...'`.
- A second block exercises the commands that are pure logic and therefore
  testable headless, against a real 3-page document: zoom clamping (`ZoomTo`
  0.1..8.0), `ZoomKey`, `FitWidth` / `FitPage`, spread widening `LayoutSpanW()`,
  `LayoutPages` rect count and area, and `GoPage` / `GotoPageIndex` clamping at
  both ends.
- A third block runs the same FPDF calls the page commands make on a scratch
  document: rotate right/left (including `/Rotate` persistence after save+reload),
  add page (insert index, size honored), delete page, split (one file per page),
  extract (single page), import (appends all source pages), and auto-crop via
  `InkBounds` + `FPDFPage_SetMediaBox` - including that the media box shrinks on
  reload and that a blank page is declined rather than trimmed to nothing.

Self-test grew from 226 to 268 checks. Result: 268 passed, 0 failed.

Not covered by automation, because they require interaction: the file dialogs
(Open, Save As, Import, Export Text/CSV, Save As Encrypted, Watermark), the
annotation dialogs (Note, Free Text, Stamp, Link), MessageBox prompts, and
the About box. Their underlying kernels are covered by the existing `tx:`,
`csv:`, `enc:`, `wat:`, `annot:` and `link:` checks.

## v0.11.0 - drag-to-draw annotation placement

Previously every annotation was created at a fixed spot. Now picking an
annotate command arms a drawing tool: the next left-drag on a page defines the
annotation rectangle, with a live dotted rubber band. A plain click (a drag of
four pixels or less) still falls back to the kind's conventional default
rectangle, so the old one-click flow is preserved. `Esc` cancels an armed tool,
and the status bar shows a hint while one is armed.

- `InsertAnnot()` gained an optional `const FS_RECTF* rect`; each kind uses the
  supplied rectangle instead of its default (highlight/underline derive their
  quad points from it). Existing calls keep the default placement, so the
  `annot:` checks are unchanged.
- `DragToPageRect(page, a, b, FS_RECTF&)` converts a canvas-client drag into a
  PDF-space rectangle: it flips y, orders the two corners, and clamps to the
  page. Kept pure (falls back to cw=120 with no canvas) so it is self-testable.
- `SetAnnotTool` / `CancelAnnotTool` manage the armed-tool state and crosshair
  cursor; `CloseDoc` clears it. The drag is captured on mouse-down and released
  on mouse-up.
- The link flow still prompts for the URI first (only URI targets are writable),
  then arms the drag.

Self-test grew from 268 to 281 checks: rectangle ordering, reverse-drag
equivalence, off-page clamping, zoom-independent placement, that `InsertAnnot`
at an explicit rectangle persists that exact rectangle after save+reload, and
the arm/re-arm/cancel state transitions. Result: 281 passed, 0 failed. App
smoke-launches.

Known limits unchanged: internal page-destination links still cannot be created
(PDFium exposes no destination-writing API), and the visual feel of the
rubber-band drag itself is verified by hand, not headless.

## v0.12.0 - find / search (2026-10-01)

A Find feature, since the two remaining roadmap items are hard blockers: static
single-exe still needs PDFium built from source with the static CRT, and internal
page-destination links still need a destination-writing API PDFium does not
export. Text search is fully supported by the bundled runtime, so it is the
highest-value unblocked addition.

- New commands `ID_FIND` / `ID_FIND_NEXT` / `ID_FIND_PREV` (Edit menu;
  `Ctrl+F`, `F3`, `Shift+F3`). `FindOpen` reuses `PromptText` to ask for the
  term, `FindStep` walks the collected matches with wraparound.
- `CountFindHits(doc, query, hits)` walks every page's text layer via
  `FPDFText_LoadPage` / `FPDFText_FindStart` (case-insensitive, flags 0) /
  `FPDFText_FindNext` / `FPDFText_GetSchResultIndex` / `FPDFText_GetSchCount` and
  records each match (page, char index, length) in document order.
  `FindHighlightRects` turns one match back into PDF-space rectangles with
  `FPDFText_CountRects` / `FPDFText_GetRect`.
- Canvas paint draws the match highlights: the active match is a filled amber box
  with a darker border, the other matches on the same page are outlined only so
  the underlying text stays legible. The status bar shows
  `Match i of N for "term"`.
- `CloseDoc` clears the query, hits and active index so tab switches never leave
  stale highlights.
- Self-test grew from 281 to 292 checks (11 new `find:` checks, including the
  multi-page ordering merge). Result: 292 passed, 0 failed. App smoke-launches.
