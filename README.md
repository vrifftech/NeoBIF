# NeoBIF 1.0.0

NeoBIF browses a KotOR **KEY/game-directory session**. Open `chitin.key`, choose a game directory, or supply BIFs for a KEY. ERF/MOD/SAV/HAK/NWM/RIM files discovered beneath that game directory remain browsable using NeoShared's ERF core.

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


## Extraction

Select one or several resources/branches. **Extract N matches** and **Save selected matches as ZIP** operate on the visible filter matches, deduplicating overlapping selections. **Extract entire selected branch** and **Extract all** explicitly ignore the filter. A confirmation shows the actual count. Unavailable/inconsistent entries are identified and not silently exported as healthy files.

Folder extraction preflights names and offers **Skip existing** (default), **Replace**, or explicit **Keep both**. Duplicate incoming names are held unless Keep both is chosen. Conflicting paths are not silently renamed. Review the detail pane for held items and requested renames. Native extraction can proceed with the independently safe rows after confirmation.

Native single-resource, folder, and ZIP exports share the staged-write implementation. It rejects destination aliases of open KEY/BIF/game-directory archives and destination links/reparse points below the selected output root. It does not truncate an old destination before the new payload is completely read, checked, flushed, and ready to replace it. Staging files are exclusively created with unpredictable names. Failures and cancellation retain completed files and preserve pre-existing destinations for unfinished items.

The reader records identity, size, and filesystem modification/change metadata when indexing and checks it around extraction. In-place editing or replacing an archive requires **Rescan (F5)**. These are conservative filesystem revision checks, not a cryptographic promise against a privileged adversary modifying filesystem metadata.

If a BIF cannot be found, **Add / Relocate BIF** associates the chosen file with a particular KEY table entry. File size alone never authorizes substitution. Multiple matching candidates require a choice. Structural/type inconsistencies are reported as issues.


## Building and tests

Use NeoShared with the archive-revision safety update and browser API 11 or later as a sibling named `neoshared`, or set `NEOSHARED_ROOT`. NeoBIF 1.3.0 introduces no further NeoShared changes. Existing shell/PowerShell build entrypoints and packaging workflows are retained. No NeoERF application change is required.

```sh
cmake -S . -B build -DNEOSHARED_ROOT=../neoshared \
  -DNEOBIF_BUILD_WX_GUI=ON -DNEOBIF_REQUIRE_WX_GUI=ON -DBUILD_TESTING=ON
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
