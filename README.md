# NeoBIF 1.0.0

NeoBIF browses a KotOR **KEY/game-directory session**. Open `chitin.key`, choose a game directory, or supply BIFs for a KEY. ERF/MOD/SAV/HAK/NWM/RIM files discovered beneath that game directory are browsable using NeoShared's ERF core.

## Search

The **Search** bar above the resource tree searches the whole indexed session. **Ctrl+F** (Command+F on macOS) focuses it. Search is ASCII-case-insensitive for resource names/types; Enter applies immediately and Escape or the clear button removes the query. The match count includes unavailable indexed resources, whose status remains visible.

| Type in Search | Result |
|---|---|
| `p_bastila` | Name fragment (also searches source paths, IDs, and status) |
| `p_bastila01.tpc` | Filename search |
| `.tga`, `tga`, or `*.tga` | Exact TGA resource type |
| `.tpc`, `tpc`, or `*.tpc` | Exact TPC resource type |
| `p_*.tpc` | Filename wildcard (`*` matches any sequence, `?` one character) |

## Export by file type to ZIP

Choose **Export → Export by file type…**, select a type such as **.tga** or **.tpc**, then choose the ZIP destination. The alphabetized picker lists the types actually present, with total/available counts. A type already in Search is preselected when possible.


## Extraction

Select one or several resources/branches. **Extract N matches** and **Save selected matches as ZIP** operate on the visible filter matches, deduplicating overlapping selections. **Extract entire selected branch** and **Extract all** explicitly ignore the filter. A confirmation shows the actual count. Unavailable/inconsistent entries are identified and not silently exported as healthy files.


## Build

This repository consumes shared code from the separate `neoshared` repository. Clone the repositories as siblings:

```text
workspace/
  neoshared/
  NeoBIF/
```

CMake automatically detects `../neoshared`. For another layout, pass `--neoshared-root /path/to/neoshared` to `build.sh` or set `NEOSHARED_ROOT` directly.

Linux GUI build:

```sh
./scripts/build.sh --wx ON --require-wx ON --jobs "$(nproc)"
```

Linux CLI/core-only build:

```sh
./scripts/build.sh --wx OFF --jobs "$(nproc)"
```

