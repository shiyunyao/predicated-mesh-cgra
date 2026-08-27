# CGRA-Bench T019 V0 Pin

- Repository: https://github.com/tancheng/CGRA-Bench
- Pinned commit: `6729aaf225d0320e4e0d3b419e20483069a5a69b`
- License: BSD-3-Clause (see `third_party/CGRA-Bench/LICENSE`)
- Corpus scope: `third_party/CGRA-Bench/kernels/` only.
- Denominator: kernel directories, C/C++ translation units, and candidate
  innermost loops are all reported independently.
- `Streaming-Bench`, `evaluation`, and `miscellaneous` are outside the T019 V0
  denominator. The nested `Streaming-Bench` submodule is intentionally not
  initialized.

The submodule must be clean and at the pinned commit before an audit run.
