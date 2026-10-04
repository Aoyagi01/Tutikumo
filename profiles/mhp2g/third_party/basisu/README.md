# Basis Universal transcoder

Unmodified from https://github.com/BinomialLLC/basis_universal release v2_50
(commit 9bebe16): `transcoder/` and the single-file Zstandard decoder
`zstd/zstddeclib.c` with its headers. Apache License 2.0 (`LICENSE`);
Zstandard is BSD licensed (`zstd/LICENSE`).

Used by host/gpu/texture_pack.cpp to read KTX2 (Basis Universal UASTC/ETC1S)
images in HD texture packs.
