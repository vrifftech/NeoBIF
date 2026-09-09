# NeoBIF 1.0.0

NeoBIF browses a KotOR **KEY/game-directory session**. Open `chitin.key`, choose a game directory, or supply BIFs for a KEY. ERF/MOD/SAV/HAK/NWM/RIM files discovered beneath that game directory remain browsable using NeoShared's ERF core. Opening those files independently belongs to **NeoERF** and is deliberately not added here.

## Search

The **Search** bar above the resource tree searches the whole indexed session. **Ctrl+F** (Command+F on macOS) focuses it. Search is ASCII-case-insensitive for resource names/types; Enter applies immediately and Escape or the clear button removes the query. The match count includes unavailable indexed resources, whose status remains visible.

| Type in Search | Result |
|---|---|
| `p_bastila` | Name fragment (also searches source paths, IDs, and status) |
| `p_bastila01.tpc` | Filename search |
| `.tga`, `tga`, or `*.tga` | Exact TGA resource type |
| `.tpc`, `tpc`, or `*.tpc` | Exact TPC resource type |
| `p_*.tpc` | Filename wildcard (`*` matches any sequence, `?` one character) |

Type searches match the indexed resource type, **not** a parent archive path containing the same text. Small results are revealed automatically; large sets retain lazy pagination. Searching is not limited to nodes already expanded. Selecting a branch and using the existing **Extract selected matches** still respects Search.

## Export by file type to ZIP

Choose **Export → Export by file type…**, select a type such as **.tga** or **.tpc**, then choose the ZIP destination. The alphabetized picker lists the types actually present, with total/available counts. A type already in Search is preselected when possible.

**Scope: every resource of that type in all indexed BIFs and game-directory archives, regardless of Search, current selection, or expanded pages.** An unavailable/missing payload requires confirmation before it is omitted. Even a single matching resource is saved as a ZIP. The default name is `game_tpc_resources.zip` (or the chosen extension).

Original names are retained under the existing source-archive/type hierarchy inside the ZIP. Distinct copies of the same ResRef in different archives are all exported, not resolved as overrides. Exact path collisions still require explicit Keep both confirmation; the existing staged-write, changed-input checks, source protection, ZIP limits and cancellation apply.

This is **extraction, not conversion**: selecting TGA does not convert TPCs, and resources/sidecars of other types are not silently included. The scope is the current archive session, not a scan of arbitrary loose textures on disk. Selecting a loose archive independently remains NeoERF's job.

## Extraction

Select one or several resources/branches. **Extract N matches** and **Save selected matches as ZIP** operate on the visible filter matches, deduplicating overlapping selections. **Extract entire selected branch** and **Extract all** explicitly ignore the filter. A confirmation shows the actual count. Unavailable/inconsistent entries are identified and not silently exported as healthy files.

Folder extraction preflights names and offers **Skip existing** (default), **Replace**, or explicit **Keep both**. Duplicate incoming names are held unless Keep both is chosen. Conflicting paths are not silently renamed. Review the detail pane for held items and requested renames. Native extraction can proceed with the independently safe rows after confirmation.

Native single-resource, folder, and ZIP exports share the staged-write implementation. It rejects destination aliases of open KEY/BIF/game-directory archives and destination links/reparse points below the selected output root. It does not truncate an old destination before the new payload is completely read, checked, flushed, and ready to replace it. Staging files are exclusively created with unpredictable names. Failures and cancellation retain completed files and preserve pre-existing destinations for unfinished items.

The reader records identity, size, and filesystem modification/change metadata when indexing and checks it around extraction. In-place editing or replacing an archive requires **Rescan (F5)**. These are conservative filesystem revision checks, not a cryptographic promise against a privileged adversary modifying filesystem metadata.

If a BIF cannot be found, **Add / Relocate BIF** associates the chosen file with a particular KEY table entry. File size alone never authorizes substitution. Multiple matching candidates require a choice. Structural/type inconsistencies are reported as issues.

## Navigation and long operations

The tree has no global 50,000-resource display cutoff. Resource rows are populated on expansion in groups of at most 512. Expand archive/type branches does not expand every resource page. Filter changes preserve matching selections/expanded branches; successful rescanning clears old numeric selections because the new index may differ.

Native indexing/export uses one joined worker with progress and cancellation checkpoints. Ordinary resource extraction and ZIP writing stream bounded chunks. Classic ZIP limits (65,535 entries and below 4 GiB total) are checked before writing; use folder extraction or a narrower selection for larger jobs. ZIP64 is not added.

The tree supports multi-selection, copying ResRefs/source paths, locating source folders, remembered extraction directories, the existing NeoShared zoom, and the supplied BIF logo.

## Browser

The browser uses retained source files and the matched **NeoShared browser API 11**. Directory extraction skips existing files by default; Replace and Keep both require explicit choices. A small progress/cancel panel accompanies exports. Cancelling keeps completed directory members and aborts the active staged write.

Source handles are checked against destinations. Browsers/import paths that do not expose source handles cannot safely verify replacement aliases: replacing an existing host file is refused, with an explanation to use a separate folder, Skip/Keep both, or a download. Handle-based source size and modification time are rechecked. The browser handle API does not supply POSIX-level exclusive creation or crash-atomic multi-file transactions. Changes by other applications and cleanup failures are reported where detectable; do not run concurrent writers against the extraction directory. A save-file picker can itself leave an empty newly chosen ZIP placeholder when cancelled or aborted.

## Building and tests

Use NeoShared with the archive-revision safety update and browser API 11 or later as a sibling named `neoshared`, or set `NEOSHARED_ROOT`. NeoBIF 1.3.0 introduces no further NeoShared changes. Existing shell/PowerShell build entrypoints and packaging workflows are retained. No NeoERF application change is required.

```sh
cmake -S . -B build -DNEOSHARED_ROOT=../neoshared \
  -DNEOBIF_BUILD_WX_GUI=ON -DNEOBIF_REQUIRE_WX_GUI=ON -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The real wxWidgets smoke test requires a desktop display; Linux CI uses `xvfb-run -a ctest ...`. Without wxWidgets, use `NEOBIF_BUILD_WX_GUI=OFF` for the core/CLI safety tests. Node is used only for engineering tests of the actual browser helper with mocked handles, not by the installed application.

`tests/reference_corpus.cpp` is an optional external-corpus comparison probe. It does not contain or redistribute game archive payloads. See the delivery validation report for which tests were actually executed.
