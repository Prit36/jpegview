# Releasing JPEGView (practical notes)

This supplements `COMPILING.txt`. It records the **actually-working** release
flow on the dev machine used for the Prit36 fork, including the gotchas that
waste time if you forget them.

## 1. The toolchain is here — but not where you first look

Do **not** trust `C:\Program Files\Microsoft Visual Studio\{2022,18}` — those
directories are **empty shells**. The real, working install is:

- **VS2022 BuildTools** → `C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools`
  - `cl.exe` / `link.exe` / `MSBuild.exe` under `VC\Tools\MSVC\14.44.35207\...`
  - Dev prompt: `VC\Auxiliary\Build\vcvars64.bat`
- **Windows SDK** → `C:\Program Files (x86)\Windows Kits\10` (e.g. `10.0.26100.0\x64\rc.exe`, `mt.exe`)

No need to install anything to compile.

## 2. Build the binaries

`extras/scripts/build-release.ps1` expects the built EXE + codec DLLs at:

```
src\JPEGView\bin\x64\Release\
```

Build it (from a `cmd` with the VS env):

```bat
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
msbuild build\JPEGView.sln /p:Configuration=Release /p:Platform=x64 /t:JPEGView /m
copy /Y build\bin\Release\JPEGView.exe src\JPEGView\bin\x64\Release\JPEGView.exe
```

> Git Bash `cmd` quoting trap: `cmd //c "call \"C:\...\vcvars64.bat\""` does **not**
> work — MSYS2 escapes the inner quotes with backslashes and cmd can't parse them.
> **Write a `.bat` file and run `cmd //c that.bat` instead.**

## 3. Package (MSI + ZIP + checksums)

`build-release.ps1` is **self-bootstrapping**: it downloads `dotnet-install.ps1`,
installs .NET 8.0 user-locally, then `dotnet tool install wix` (WiX v6). It does
**not** require a pre-installed `dotnet`. It needs network access.

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File extras/scripts/build-release.ps1
```

Outputs to `release_out\v<version>\`:
- `JPEGView64_en-us_<version>.msi`
- `JPEGView_<version>.zip`
- `SHA256SUMS.txt`

(The version is auto-detected from `src/JPEGView/resource.h`.)

## 4. Publish to GitHub

`gh release create ... file1 file2` fails here because it wants the `workflow`
OAuth scope, and `gh auth refresh -s workflow` needs an interactive browser
(device login) which can't complete headlessly.

**Workaround (works with just the `repo` scope):**

1. Create the release via the REST API:
   ```bash
   gh api -X POST repos/Prit36/jpegview/releases \
     -f tag_name=v2.1.1 \
     -f name="JPEGView 2.1.1 - <short title>" \
     -f body=@release_notes.md -f draft=false -f prerelease=false -f make_latest=true
   ```
   Note the release **ID** (`id` in the response) — you need it for uploads.

2. Upload each asset with a direct API call to the release's upload URL:
   ```bash
   RID=<release-id>; D=release_out/v2.1.1
   for f in JPEGView64_en-us_2.1.1.msi JPEGView_2.1.1.zip SHA256SUMS.txt; do
     gh api "https://uploads.github.com/repos/Prit36/jpegview/releases/$RID/assets?name=$f" \
       -X POST -H "Content-Type: application/octet-stream" --input "$D/$f"
   done
   ```

> Gotcha: `gh release list` / `gh release view` (no `-R`) may resolve to the
> **upstream** `sylikc/jpegview` repo. Always use `--repo Prit36/jpegview` (or
> `gh api ... repos/Prit36/jpegview/...`).
