# anvil-shared

Shared C++/iceoryx2 wrapper code used by both
[Anvil](https://github.com/Beerlesklopfer/Anvil) (PLC runtime / publisher) and
[Bellows](https://github.com/Beerlesklopfer/Bellows) (HMI / subscriber).

## Contents

- `anvil_wrapper/` — small C++ shim around the iceoryx2 C-FFI:
  - `anvil_iox_wrapper.{h,cpp}` — public API used by both servers
  - `i_anvil_data_sink.h` — data sink interface
  - `anvil_data_sinker.hpp` — sinker implementation
- `iceoryx2_src/` — pinned [eclipse-iceoryx/iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2)
  submodule. The Anvil and Bellows build pipelines build the FFI-C library from
  this source tree at compile time.

## Usage

This repo is **not** built standalone. It is consumed as a git submodule by
both Anvil and Bellows under the path `shared/`:

```bash
git submodule add git@github.com:Beerlesklopfer/anvil-shared.git shared
git submodule update --init --recursive
```

The consuming repo's CMake build is responsible for compiling iceoryx2 (via
`cargo build --release` inside `shared/iceoryx2_src/iceoryx2-ffi/c/`) and for
linking `anvil_wrapper/anvil_iox_wrapper.cpp` into its binaries.

## License

AGPL-3.0-or-later — see [LICENSE](LICENSE).
