# Stitchup PDF Editor

Portable Windows PDF editor + viewer. Nitro Pro v7-style layout: toolbar, page
thumbnails, page navigation, zoom, and editing (add/delete/rotate/reorder/import
pages, save, save-as).

## Technology

- UI: Native Win32 API (single window, custom toolbar, thumbnails panel,
  canvas, status bar, splitter), DPI aware
- PDF engine: **PDFium** (Chromium, BSD-3-Clause), prebuilt release
  `chromium/8057` for Windows x64
- Build: Microsoft C++ (MSVC), CMake + NMake; `/MT` static CRT, `/W4 /WX`
- Portable package: `Stitchup.exe` + `pdfium.dll` in one folder, no install

## Build

```
call Build.cmd
```

Produces `dist\Stitchup.exe` + `dist\pdfium.dll`.

## Run

- `Stitchup.exe` - launch the editor GUI
- `Stitchup.exe file.pdf` - open a PDF at startup
- `Stitchup.exe --self-test` - headless self-test, writes `test_result.txt`
- `Stitchup.exe --demo out.pdf [pages]` - generate a sample multi-page PDF

## Features

- Nitro Pro v7-style shell: menu bar `File Edit Pages Annotate View Home Tools
  Sidebar Help`, a compact ribbon below it that shows the Home groups by default
  (Document, Pages, Annotate, Content), and a left navigation pane (Page
  Thumbnails, Bookmarks with outline tree), splitter, canvas, status bar
- Home and Tools top-level menus switch the ribbon tab (Home = Document /
  Pages / Annotate / Content groups; Tools = Security / Export). The View
  tab's controls (Zoom, Navigate, Panes) live under the View menu, and Sidebar
  (F8) appears both in View and as its own top-level item
- Multi-document tabs: open several files at once and switch between them
  (Ctrl+Tab / Ctrl+Shift+Tab, New Tab Ctrl+T, Close Tab Ctrl+Shift+F4, or click
  the strip at the top of the window); each tab keeps its own page, zoom and
  scroll position
- View: two-page side-by-side spread (`F5` or View > Two-Page Spread) and a
  toggleable left navigation pane (`F8` or View > Sidebar) for extra canvas
  room; pages are centered horizontally in the main display
- File menu: New, Open, Save (atomic: temp file + replace), Save As, Import PDF
  (appends the chosen file's pages, single or multi-page, and scrolls to the
  first imported page)
- Edit: rotate page CW/CCW, delete page, add blank page (Letter/A4), drag pages
  in the thumbnails panel to reorder
- View: zoom in/out, 100%, fit width, fit page, previous/next page, and a
  Dark Mode toggle (View > Dark Mode, `Ctrl+D`, remembered between runs)
- Modern flat UI: flat ribbon (hover/pressed states, accent brand strip,
  labelled group cards), Segoe UI text, embedded multi-size app icon (a
  parchment scroll with an inked quill), and a light/dark color theme
- Bookmarks pane lists the PDF outline (nested); clicking a bookmark jumps to
  its page
- Find (Edit > Find... / Find Next / Find Previous, `Ctrl+F` / `F3` /
  `Shift+F3`): case-insensitive text search across the document using PDFium's
  text API; every match is collected in page order, the active match is shown
  as a filled amber box and the other matches on the same page are outlined, and
  F3 / Shift+F3 step through the matches with wraparound
- Annotate (Home > Annotate group or Edit > Annotate menu): pick highlight,
  underline, sticky note, text box, shape, stamp or link, then **drag on the
  page to draw its rectangle** (a plain click uses the kind's default
  placement; Esc cancels); the page re-renders immediately so the new annotation
  is visible without changing zoom; annotations persist through Save and keep
  their rendered appearance
- Links: clicking an internal link jumps to its destination page; clicking a
  URI link opens the URL in the default browser (hand cursor over link areas)
- Link creation (Home > Annotate group "Link..." or Edit > Annotate > Link...):
  the dialog asks for a web address, then a drag on the page draws the clickable
  /Link rectangle. A link is created with the annotation API
  (`FPDFPage_CreateAnnot` + `FPDFAnnot_SetURI`), which the bundled PDFium already
  supports, so no newer runtime is required. Only web (URI) targets are
  supported — PDFium can read page destinations but offers no API to write one,
  so internal page jumps must come from the source document
- Content object editing (Edit > Select / Move Content Object, or the Home >
  Edit group Select tool): click any content object — text run, vector path or
  image — on the current page to select it (accent bounding box + handles), then
  drag to move it, Ctrl+Del (or Menu > Delete Selected Object) to remove it,
  RecColor to recolor the fill, or Edit Text (double-click) to replace the text
  run; edits persist through Save and reload (4 object tests added)
- Double-clicking a line of text enters edit mode on it: the run is selected and
  the Select tool is armed, and the Edit Text dialog opens with that run's text
  preselected and the caret ready, so the line can be retyped immediately
- Page management (Home > Pages): Extract the current page into its own PDF,
  Split a document into one file per page, and Auto-Crop the current page to
  its content (white margins trimmed via an ink-bounding-box render pass)
- Export Text (File > Export Text): saves every page's extractable text as a
  UTF-8 (BOM) .txt file
- Export CSV (File > Export CSV): writes a UTF-8 (BOM) `.csv` with one
  `Page,Width (pt),Height (pt),Text chars,Annotations` row per page
- Watermark (File > Watermark): stamp every page with custom text in filler
  gray at 50% opacity. The dialog prefills "Confidential", with a 16/24/36/
  48/64/96 pt size selector and Center (diagonal, 45 degrees) / Top center /
  Tiled placements; the document is saved in place and reloaded
- Security: password-protected PDFs are detected and unlocked through a modal
  password dialog (up to 3 attempts, then an error box); wrong passwords are
  rejected by PDFium
- Security: Save As Encrypted (File > Save As Encrypted...) writes a
  password-protected copy of the open document (PDF revision 3 / 128-bit RC4
  standard security handler, user + optional owner password). The saved file
  re-opens against the bundled PDFium only with the correct password; the open
  document is left unchanged
- Drag & drop a PDF onto the window to open it; open via command-line argument
- Shortcuts: Ctrl+N/O/S, Ctrl+Shift+S, Ctrl+R (rotate), Ctrl+[/Ctrl+], Delete,
  Ctrl+0/1/2 (100%/fit width/fit page), Ctrl+F (find), F3 / Shift+F3 (find
  next / previous), Ctrl+D (dark mode), PgUp/PgDn, Ctrl+wheel to zoom, F5
  (two-page spread), F8 (toggle sidebar), Ctrl+T (new tab),
  Ctrl+Tab/Ctrl+Shift+Tab (next/previous tab)

## Third-party components

| Component | Version | License        |
|-----------|---------|----------------|
| PDFium    | chromium/8057 | BSD-3-Clause (see `third_party/pdfium/LICENSE`) |

`pdfium.dll` is the unmodified binary release from the `pdfium-binaries`
project (github.com/bblanchon/pdfium-binaries). Non-GPL build.

## Known limits / roadmap

- PDFium is currently a DLL (portable folder, not single exe). A static
  single-exe build is planned but first requires building PDFium with the
  static CRT from source (the prebuilt import lib is not static).
- Shell + annotations + link activation + page management (extract/split/
  auto-crop) + text export + CSV export + password-unlock + watermarks +
  encrypt-on-save are done. Rendering, page model,
  editing operations, ribbon navigation, the bookmarks pane, annotation
  creation/persistence, link navigation, link *creation* and page
  extract/split/crop are functional.
- Link *creation* works on the bundled PDFium: a hyperlink is a /Link
  annotation, and PDFium exposes `FPDFPage_CreateAnnot` plus
  `FPDFAnnot_SetURI` for writing the target. A newer `pdfium.dll` is therefore
  not required; the checked `chromium/8076` build exports the same set.
- Annotation placement is drag-to-draw: pick a tool, drag the rectangle on the
  page. A plain click still uses a sensible default rectangle, and Esc cancels
  the armed tool. (The earlier build placed every annotation at a fixed spot.)
- Tools menu/ribbon tab hosts the Security (Save As Encrypted) and Export
  (Export Text / Export CSV) groups.