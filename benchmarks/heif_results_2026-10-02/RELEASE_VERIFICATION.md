# JPEGView 2.8.0 release verification

- Release x64 build succeeded; app and MSI version are 2.8.0.0.
- The standalone asset, EXE extracted from the portable ZIP, and EXE extracted
  from the MSI have identical SHA-256:
  `cf7e5fc7348bd385ae4f7470bad1d59841b03801527ce636e6ed9c8f8aee5e9b`.
- MSI and ZIP configuration and runtime DLL hashes match. ZIP contains only
  JPEGView.exe as an executable, with no benchmark probes or object/PDB files.
- Both extracted packages opened all 24 corpus files successfully: one warmup
  plus two measured launches per file per package (96 measured, 48 warmups).
- [Raw package smoke results](release_package_smoke_2026-10-02.json) label MSI
  as `baseline` and ZIP as `candidate`; they are the same release build, **not**
  a pre/post optimization comparison. Percentage differences there are run noise
  and must not be presented as an optimization improvement.
- MSI payload was extracted and verified without installing it into the user's
  machine. Installer UI and upgrade behavior were not exercised.
- The performance and pixel reports in this directory were captured before
  bumping version resources from 2.7.0.0 to 2.8.0.0. Decoder/conversion logic is
  unchanged by that version bump. Artifact checksums are in the release's
  SHA256SUMS.txt. Builds are unsigned.

Raw measurements were copied out of temporary storage before cleanup. Temporary
file deletion was blocked by execution policy, so local benchmark caches and
packaging verification directories remain. None are committed or shipped in the
release packages. Test-image inputs were preserved, not added to this release commit.
