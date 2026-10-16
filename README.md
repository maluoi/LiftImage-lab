# Lift Image lab

The measuring side of [Lift Image](https://github.com/maluoi/LiftImage): the test corpus, the benchmark, comparisons against other codecs, the test suites that need real images, and the history of every measured change. The codec itself stays small and image-free in its own repository, which this one clones inside itself:

```
git clone https://github.com/maluoi/LiftImage-lab.git
cd LiftImage-lab
git clone https://github.com/maluoi/LiftImage.git
make -j8
```

`LiftImage/` is ignored here, so the two repositories stay independent: commit codec changes there and measurements here. Each checkpoint records the codec commit it measured.

## Measuring

```
build/bench corpus/examples                      # size, quality and speed against JPEG and PNG
python3 tools/checkpoint.py <name> "<change>"    # a full checkpoint, appended to checkpoints/log.md
python3 tools/examples.py                        # comparison images into examples/
```

`build/bench [-v] [-d dump_dir] [-c out.csv] <images or dirs>` compares against libjpeg-turbo when `pkg-config` finds it. Checkpoints bench `corpus/examples`, `corpus/modes` and `corpus/hdr` with gcc and clang, count lines and binary sizes, and score SSIMULACRA2 and Butteraugli when libjxl's `ssimulacra2` and `butteraugli_main` tools are installed. For steady timings, pin runs to one core with `taskset`, and keep the machine otherwise idle.

## Comparing

The codec README's tables come from here:

```
make compare MOZJPEG=/opt/mozjpeg && tools/compare.py --metric ssimulacra2   # stb, libjpeg-turbo and mozjpeg
make -C modern JXLSRC=<libjxl source> JPEGLI=<libjpegli-static.a> && modern/run_timing.sh   # WebP, JPEG XL, AVIF and jpegli
```

`make compare` needs libjpeg-turbo, and `MOZJPEG` points at a [mozjpeg](https://github.com/mozilla/mozjpeg) install for its rows. Without the `ssimulacra2` tool, `tools/compare.py` compares at equal PSNR. `modern/Makefile` lists what the second harness needs, including jpegli built from libjxl's source. Both pin themselves or should be pinned, as in `taskset -c 3 tools/compare.py`.

## Testing

```
make test     # the codec's vectors, then sanitized prefix, progressive and layer tests over the corpus
make repro    # encodes from gcc and clang at several flag sets must match byte for byte
MSVC_ENV=~/Tools/msvc-env.sh make repro     # and from MSVC, at SSE2 and AVX2, under Wine
```

After a format change, `make lfi && tools/make_vectors.py` re-encodes the codec's test vectors from `tests/vectors` and writes their hashes into its `docs/format.md`.

## Corpus

`corpus/` holds lossless images only: Poly Haven textures and HDRIs (CC0), the Kodak suite, and two StereoKit screenshots. [corpus/README.md](corpus/README.md) credits each one.

`tools/make_logo.py` draws the codec's icon and README header into its `docs/images`.
