# Undo / Redo — progress snapshot (2026-10-07)

Status: v0.12.8 released. Exploration for Undo/Redo DONE (results below). No
Undo/Redo code written yet. All repo changes are committed on `main`.

## v0.12.8 shipped (landed earlier today)
- Commit `c61753d` "v0.12.8: print preview (WYSIWYG, seeded paper, zoom/scroll)",
  pushed to main, tag `v0.12.8`, CI build run `37577318683` success.
- Release "Stitchup PDF Editor v0.12.8": https://github.com/Kafaferanterenous/stitchup-Win/releases/tag/v0.12.8
  Assets: Stitchup.exe 742,912 B, pdfium.dll 7,375,360 B,
  Stitchup-v0.12.8.zip 4,004,814 B (SHA-256 9D476A63E41927577657A23CBC37D8836652768CFB18E4AC8583E8446E577337).
- Self-test: 440 passed, 0 failed; exe version 0.12.8.0.
- archive/ has Stitchup-v0.12.8.zip + dated backup `2026_10_07 - 16_18_Stitchup.zip`.
- Legacy dist synced: `E:\opencode_work\027_2026_09_15_Stitchup_PDF_Editor\dist\`.

## Next goal (README "Remaining goals" order): Undo / Redo
Today's date line: v0.12.9.

## Code map (all in src/app.cpp, single TU, ~12,084 lines, everything static)

### Mutating edit paths (call `BeginEdit`/snapshot at top of each):
| DoCommand case | line | function | line |
|---|---|---|---|
| ID_DELETE | 7295 | DeleteSelectedPages | 3999 (`FPDFPage_Delete` at 4020) |
| ID_ADD | 7296 | AddPage | 4030 (`FPDFPage_New` 4038) |
| ID_ROTL / ID_ROTR / ID_ROT_ALL | 7297/7298/7303 | RotateSelectedPages(int) | 4052 (`FPDFPage_SetRotation` 4063) |
| ID_PAGE_CROP | 7352 | CropCurrentPageToContent | 4175 (`FPDFPage_SetMediaBox` 4198) |
| ID_IMPORT | 7294 | ImportPdf | 3063 (`FPDF_ImportPagesByIndex` 3085) |
| ID_WATERMARK | 7355 | WatermarkCurrentDoc -> ApplyWatermarkDoc | 2026 / 1678 (`FPDFPage_InsertObject` 1715) |
| drag reorder (ThumbsProc WM_LBUTTONUP 4947) | — | ReorderPagesTo(moving,to) | 4263 (rebuilds g.doc from scratch; takes ownership of old g.doc and closes it; rotation/selection/marks remapped manually) |
| annotations (ID_ANN_*) | 7333-7346 | SetAnnotTool -> InsertAnnotCurrent -> InsertAnnot | 6969 / 6953 / 6805 (`FPDFPage_CreateAnnot` 6829+) |
| object edit/delete/recolor | 7305-7307 | EditSelectedText / DeleteSelectedObject / ApplyRecolorSelected -> CommitEdits() | 5415 / 5127 / 5200 / 5117 (`FPDFPageObj_*`, `FPDFPage_RemoveObject`/`InsertObjectAtIndex`, `FPDFPage_GenerateContent`) |
| object drag-move | CanvasProc WM_LBUTTONUP 6020-6039 | -> CommitEdits | (transform 5983) |
| bookmarks (ID_BM_PAGE/VIEW 5847-5848) | — | AddUserBookmark | 5672 (writes g.marks only; folded into file at save via AppendOutlines 2489) |

g.dirty=true sites: 2057 watermark, 3092 import, 4021 delete, 4040 add, 4073 rotate,
4199 crop, 4338 reorder, 5701 bookmark, 6958 annot.
g.dirty=false: 1129 CloseDoc, 1212 CloseTab, 1433 LoadDoc, 1455 NewDoc, 2803 SaveAs, 2836 SaveInPlace.

NOTE: CommitEdits (5117) -> SaveInPlace (2812) -> CloseDoc + LoadDoc(target), so the
FPDF_DOCUMENT handle changes on every object edit and every Ctrl+S. SaveInPlace
serializes to bytes first (SerializeForSave 2713), writes tmp, then reloads.

### Page-order model: none beyond g.doc itself
- g.pageCount = FPDF_GetPageCount(g.doc) in RefreshState (1271-1274).
- Rotation lives in the PDF (/Rotate). No FPDF_MovePage exists; reorder rebuilds doc.
- Selection: g.pageSel vector<bool> (230), selAnchor (231), g.selected (224).

### No existing undo/history/backup code (checked exhaustively).

### Byte capture (already exists, ideal for snapshots):
- `SaveAsString(FPDF_DOCUMENT d, std::vector<unsigned char>& out)` — line 8847, pure
  FPDF_SaveAsCopy, no outline folding, doc-parameterized. USE THIS for snapshots.
- `SerializeForSave(out, interactive)` — 2713 (includes g.marks via AppendOutlines).
- Reload from bytes: `FPDF_LoadMemDocument(buf.data(), (int)size, nullptr)` (pattern ~40x in tests).

### Tab/lifecycle:
- TabDoc (299-311): doc,path,name,dirty,pageCount,selected,zoom,scrollX/Y,marks.
- g_tabs (312), g_curTab (313). SnapshotCurrentTab (1139), RestoreTab (1167),
  CloseTab (1192), CloseDoc (1114), LoadDoc (1393), NewDoc (1448), CheckLib (811).

### Wiring pattern:
- FrameProc WM_KEYDOWN ctrl branch at 8055-8078 (e.g. 'P'->shift?ID_PRINT_PREVIEW:ID_PRINT at 8066).
- WM_COMMAND dispatch 7983-7986 (ribbon buttons send WM_COMMAND).
- IsHandledCommand 7246 — flat switch 7250-7263; NEW IDs MUST be added here or the
  "menu: no dead items" self-test fails (10211).
- BuildMenu 7379; Edit popup built at 7408-7425 (natural place for Undo/Redo: top, before Find 7409).
- Ribbon: RibbonSpec 6390, BuildToolbar 6401, table 6403-6478.
- kAboutText 145-158; About self-test at 11325-11334.
- ID enum 41-118; ID_BM_PAGE/ID_BM_VIEW at 116-117 sit AFTER the contiguous
  ID_THEME_FIRST..ID_THEME_MAC radio block (105-113). New ID_UNDO/ID_REDO can go
  before line 105 or after 117.

### Self-test harness:
- SelfTest(cwd) 8865; check 8881 / checkEq 8886; SUMMARY 11628-11631; return fail.
- invoke: wWinMain 11946 `return SelfTest(cwd)==0?0:1`.
- Current total: 440 passed / 0 failed (test_result.txt, gitignored).
- AGENTS.md rule: only finished when test_result.txt ends "SUMMARY N passed, 0 failed".
- Doc-fixture swap pattern (from reorder test 10656-10686): save g.doc/pageCount/etc,
  assign scratch doc, call real function, restore globals. ReorderPagesTo UNUSable more
  than once per fixture (it closes g.doc).

## Recommended design (decide during implementation)
1. Snapshot = `SaveAsString(g.doc, bytes)` + copy of `g.marks`. Store per-tab
   (add undo/redo vectors to TabDoc, or a parallel g_undo/g_redo).
2. `BeginEdit()` called at the top of every mutating path (the 9 rows above) BEFORE
   the mutation — pushes current bytes+marks onto undo, clears redo.
   RotateSelectedPages/Reorder need the snapshot BEFORE they mutate/replace g.doc.
   Bookmark undo = restoring the marks list only.
   No-op guards: rotate with 0 touched, reorder dropped on itself — snapshot then
   discard if no-op, or guard before BeginEdit (push after checking, but bytes must be
   taken before mutation... simplest: BeginEdit then pop on no-op change, or have the
   mutators return bool touched and the caller pop).
3. Depth cap ~20 entries. No need for text/op composition.
4. Restore: take g_undo back, parse bytes with FPDF_LoadMemDocument, then follow the
   ReorderPagesTo ownership pattern: keep path/name, CloseDoc(), g.doc = new, restore
   marks/selection/pageCount, RefreshState(), ClearCanvasCache/ClearThumbCache,
   set g.dirty = true (doc now differs from disk), invalidate canvas+thumbs.
   Selection: pageSel may be sized against old pageCount — call PrunePageSel (1001).
5. Undo/Redo are always-enabled menu items; no-op (and no stack change) when empty.
6. Wire: ID_UNDO/ID_REDO; IsHandledCommand cases; DoCommand cases; BuildMenu at top of
   Edit popup ("Undo\tCtrl+Z", "Redo\tCtrl+Y"); WM_KEYDOWN Ctrl 'Z' and Ctrl 'Y';
   kAboutText line + About checks.
7. Self-tests (new block, before SUMMARY): build e.g. a 3-page scratch doc via
   SaveAsString -> mutate (rotate/delete/add via real fns or direct FPDF calls) ->
   restore the snapshot bytes -> assert pageCount/rotation/content round-trip
   (GetPageRotation, GetPageCount); assert undo pops in LIFO order; redo re-applies.
   Follow the sel: swap pattern (10501+). Expect ~10-20 new checks.
8. Check g.readOnly (read-only mode) exists — if present, disable Undo/Redo when set
   (verify before implementing).

## Risks / caveats
- FPDF_LoadMemDocument round-trip of annotations/content streams is standard SaveAsCopy,
  fine. Marks are separate (capture alongside).
- Object edit path already rebuilds g.doc (SaveInPlace); snapshots are bytes so
  unaffected, but restore must not assume g.doc identity survives.
- Reorder test fixtures are one-shot (g.doc closes); build fresh fixtures per test.
- Keep RESULTS.md + README updated: new README feature bullet, remove "Undo / Redo" from
  Remaining goals, Ctrl+Z/Ctrl+Y in shortcut list; VERSION bump v0.12.9; RESULTS.md
  section documenting the feature + check-count growth.
- Release flow for v0.12.9 (same as v0.12.8): Build.cmd + self-test (kill Stitchup
  first) -> VERSION -> README/RESULTS -> commit -> push -> CI (keep waiting) -> tag ->
  gh release create with dist exe+dll + archive zip (Compress-Archive dist exe,dll ->
  archive\Stitchup-v0.12.9.zip, Get-FileHash SHA256) -> dated backup zip ->
  legacy dist sync.
- PowerShell/.NET quirk: System.IO.File relative paths resolve against
  C:\Users\default.DESKTOP-4CM6H2V\Documents\Default Project — always use absolute paths.