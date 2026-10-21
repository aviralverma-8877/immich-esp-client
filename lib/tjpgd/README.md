# TJpgDec R0.03 (patched)

ChaN's TJpgDec as shipped in Bodmer's TJpg_Decoder 1.1.0 (adds the `swap`
field), vendored so it can be patched for the JPEGs Immich produces:

- Huffman tables are looked up per component from the SOS header (up to 4
  per class) instead of assuming Y = table 0 and Cb/Cr = table 1. Many Immich
  previews give Cb and Cr their own tables.
- SOF1 (extended sequential, Huffman, 8-bit) is decoded like SOF0.
- 16-bit quantisation tables are accepted.

Original license terms are kept in the source headers.
- `tjpgd.h` always uses `<stdint.h>` (the old VC++ typedef fallback breaks MinGW host builds).
