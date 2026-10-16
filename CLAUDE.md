# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Lift Image lab

This repository measures [Lift Image](https://github.com/maluoi/LiftImage), the codec, which is cloned inside it at `LiftImage/` as an independent repository. Codec changes are made and committed there; `LiftImage/CLAUDE.md` holds the codec's budgets, coding guidelines and the list of ideas already tried and rejected. Read it before touching codec code. This repository holds the corpus, the benchmark, codec comparisons, the image-based test suites and the checkpoint history.

## How we work

- Prototype in the scratchpad, and land only what measures. Every landed codec change gets a checkpoint: `python3 tools/checkpoint.py <name> "<description>"` builds, counts LoC and binary size, benches `corpus/examples`, `corpus/modes` and `corpus/hdr` with gcc and clang, records the codec commit, and appends to `checkpoints/log.md`. Report LoC and binary size deltas alongside speed and size.
- A change that shouldn't alter output must match the previous checkpoint's CSVs exactly in every non-timing column, across all three corpora, and pass `make test`.
- Encodes must stay byte-identical across compilers and CPU levels: run `make repro`, with `MSVC_ENV=~/Tools/msvc-env.sh` to include MSVC under Wine.
- Timing noise is about 3% on this machine (CPU pinned near 2 GHz). Use best-of-N, alternate A/B runs, and confirm close calls with `perf stat` cycle counts. Decode runs near 4 IPC, so instruction count is usually the lever; `perf record` and `perf annotate` find it. Never time while other CPU-heavy jobs run.
- A format change updates the codec's `docs/format.md`, then `make lfi && tools/make_vectors.py` regenerates its vectors and hashes from `tests/vectors`.

## Layout

- `Makefile` builds `build/bench` from `LiftImage/src` and `bench/bench.c`, and the codec's `lfi` tool with this machine's `-march=x86-64-v3`. `make compare` builds `build/jpeg_compare`, with `MOZJPEG=<install prefix>` for mozjpeg.
- `tools/checkpoint.py` measures checkpoints, `tools/examples.py` writes comparison images to `examples/` (only `examples/compare/parrot.png`, which the codec README shows, is committed), and `tools/compare.py` runs `build/jpeg_compare` for the codec README's rows against stb, libjpeg-turbo and mozjpeg. `tools/make_vectors.py` regenerates the codec's test vectors, and `tools/make_logo.py` its icon and header SVGs.
- `modern/` compares against WebP, JPEG XL, AVIF and jpegli; `modern/Makefile` lists its dependencies.
- `tests/` holds the sanitized prefix, progressive and layer tests (`safety.sh`), the reproducibility check (`repro.sh`, on `enchash.c`), and the sources of the codec's test vectors and layer tests.
- `corpus/` holds lossless test images only, since images that were once JPEGs skew encoder comparisons against JPEG. `checkpoints/` holds the local measured history, which isn't committed. `corpus/README.md` credits every image, so a new image adds its source there.
