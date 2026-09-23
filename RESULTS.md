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

Result: **185 passed, 0 failed** (exit code 0).

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
column.
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
  multi-size icon (16/24/32/48/256, generated programmatically as
  `resources/app.ico`, compiled via `resources/app.rc`).

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
| Stitchup.exe   | 451,584 B | 67E2460D5FA295264D1AF3F8F10090A9329E79D0D85E8F9D3E8194A87410FB30 |
| pdfium.dll     | 7,375,360 B | 55E7EBEF29A1EC9523D1ADB8B260A73E7DFB0F64D3F0285121D20ECD6148EF18 |
| example.pdf    | 1,034 B   | B276682EFD75780E462C17489176D710AD1339D84E69C6218AD3C2F8E60FC132 |
| app.ico        | 20,597 B  | 5FB009C7A83254FBCD81C205492E03A683EDB0BA65B6E44962614B06A240CA44 |

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
Self-test 185/185. User verdict pending — open `dist\Stitchup.exe file.pdf` and
try Home > Annotate, click any PDF link, Home > Pages (Extract / Split /
Auto-Crop), File > Export Text / Export CSV, File > Watermark, File > Save As
Encrypted, and open a password-protected PDF.
