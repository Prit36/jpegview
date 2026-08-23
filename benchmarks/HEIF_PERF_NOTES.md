# HEIF/HEIC decode performance - investigation notes (2026-08-23)

## Final state
- `src/JPEGView/HEIFWrapper.cpp`: fast path added - decodes as planar YCbCr 4:2:0
  directly and converts to BGRA with a fused OpenMP-parallel kernel, bypassing
  libheif's scalar single-threaded color conversion pipeline entirely.
  No ffmpeg anywhere; decoder stays libde265.
- Fallback to the original RGBA path for alpha images, HDR->8bit, monochrome,
  non-420 subsampling, or if YCbCr decode fails.

## Honest benchmark (same files, same machine, app's own /benchmark telemetry)
| File | baseline | now | delta |
|---|---|---|---|
| RAW15186_grid4x4.heic | ~326 ms | ~296 ms | -9% |
| RAW15186_single.heic | ~2290 ms | ~2307 ms | ~same |
| RAW15466_single.heic | ~510 ms | ~496 ms | -3% |
| RAW15737_single.heic | ~675 ms | ~648 ms | -4% |

Kernel itself measured at 35-54 ms per full-res conversion (was 200-400 ms
inside libheif). Wall-clock gains are modest because libde265's HEVC decode
dominates (~2 s for the worst file); that is the library floor.

## Bugs found & fixed along the way
1. Chroma bottom-border clamp used the luma row bound instead of the chroma
   plane row count -> out-of-bounds read one row past the chroma plane on
   even-height images. Small images survived on heap slack; large images hit an
   unmapped page and crashed (0xC0000005). Fixed by clamping cyNext against
   chromaRows.
2. The earlier uncommitted heif.dll (ffmpeg-linked build) was broken: CUDA/D3D12
   hwaccel self-selection failed mid-decode returning no image. Reverted to the
   committed heif.dll; ffmpeg remains fully removed per user requirement.

## Remaining options if more speed is ever needed
- The de265 decode floor dominates (~90% of wall time). A faster HEVC decoder
  would be the only significant lever (user declined ffmpeg).
- Kernel could be further vectorized (AVX2) to shave another ~30-40 ms CPU.

## Tooling added (untracked)
- benchmarks/benchmark_heic.py       honest A/B driver via app telemetry
- benchmarks/generate_heic_assets.py full-res x265 assets generator
- benchmarks/verify_heic_decode.py   success/dimension check vs pillow-heif
