# anvil-shared

[![License](https://img.shields.io/badge/license-AGPL--3.0--or--later-blue.svg)](LICENSE)

Shared C++ glue code and pinned [iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2)
sources used by the two server-side components of the
[ForgeIEC](https://forgeiec.io) PLC ecosystem:

- **[Anvil](https://github.com/Beerlesklopfer/Anvil)** — the IEC 61131-3 PLC
  runtime daemon (`anvild`) and its protocol bridges (`tongs-modbustcp`,
  `tongs-ethercat`, …). Anvil is the **publisher** side of the IPC channel.
- **[Bellows](https://github.com/Beerlesklopfer/Bellows)** — the OPC UA
  gateway / HMI bridge (`bellowsd`). Bellows is the **subscriber** side.

This repository exists so that both servers can be built and released
independently, without depending on the monolithic `ForgeIEC-Studio`
parent repository, while still sharing exactly the same wrapper code and
the same iceoryx2 revision.

## Why a separate repository?

The two servers used to live as git submodules of `ForgeIEC-Studio` and
referenced `core/anvil_wrapper/` and `utils/iceoryx2_src/` in their parent
directory via relative paths in their `build.rs` scripts. That worked when
checked out under the studio repo but made the servers impossible to
release on their own. Splitting the shared bits into this repository lets
each server pin **the same revision of both `anvil_wrapper` and iceoryx2**
while remaining standalone.

## Repository layout

```
anvil-shared/
├── anvil_wrapper/             ← thin C++ shim around the iceoryx2 C-FFI
│   ├── anvil_iox_wrapper.h    ← public C API (used by anvild + bellowsd)
│   ├── anvil_iox_wrapper.cpp  ← implementation
│   ├── i_anvil_data_sink.h    ← data sink interface
│   └── anvil_data_sinker.hpp  ← sinker template implementation
├── iceoryx2_src/              ← git submodule, pinned upstream iceoryx2
├── LICENSE                    ← AGPL-3.0-or-later
└── README.md                  ← this file
```

### `anvil_wrapper/`

A small (~600 lines) C++17 façade around the iceoryx2 C FFI. It exposes a
stable C ABI (`anvil_node_t`, `anvil_pub_t`, `anvil_sub_t`,
`anvil_publish`, `anvil_receive`, …) so that consuming code never sees
iceoryx2 types directly. This insulates the runtime from upstream
breaking changes and makes it possible to swap iceoryx2 for another
shared-memory IPC library if ever needed.

The auto-generated bridge code that anvild compiles at runtime
(`bridge_gen.rs` in Anvil) uses **only** symbols from
`anvil_iox_wrapper.h`. It never includes any iceoryx2 header.

### `iceoryx2_src/`

Pinned git submodule pointing at upstream
[eclipse-iceoryx/iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2).
The pin is updated deliberately and atomically: bumping the submodule
SHA in this repository, then in both `Anvil` and `Bellows`, ensures
that the publisher and subscriber are always linked against the exact
same iceoryx2 binary layout. Mismatched versions across publisher and
subscriber lead to silent shared-memory corruption.

## Consuming this repo (as a submodule)

Both `Anvil` and `Bellows` add this repository as a git submodule under
the directory `shared/`:

```bash
git submodule add https://github.com/Beerlesklopfer/anvil-shared.git shared
git submodule update --init --recursive
```

Their respective `build.rs` scripts then reference the wrapper sources
and the iceoryx2 source tree under `shared/anvil_wrapper/` and
`shared/iceoryx2_src/`. The CMake-driven CI in each consumer builds
`iceoryx2-ffi-c` from `shared/iceoryx2_src/` once and links the resulting
`libiceoryx2_ffi_c.so` against `anvil_wrapper`-derived object code.

## Building (for inspection / testing)

This repository on its own does not produce any binaries — it is purely
a content store for the consumer repos. There is no `Cargo.toml` or
`CMakeLists.txt` at the root.

To build the iceoryx2 C-FFI library by hand, just like the consumer CI
does:

```bash
git clone --recurse-submodules https://github.com/Beerlesklopfer/anvil-shared.git
cd anvil-shared/iceoryx2_src
cargo build --release -p iceoryx2-ffi-c
ls target/release/libiceoryx2_ffi_c.so
ls target/release/iceoryx2-ffi-c-cbindgen/include/iox2/iceoryx2.h
```

## Updating the iceoryx2 pin

```bash
cd iceoryx2_src
git fetch
git checkout <new-sha-or-tag>
cd ..
git add iceoryx2_src
git commit -m "iceoryx2: bump to <new-sha-or-tag>"
git push
```

After pushing, both `Anvil` and `Bellows` need to bump their `shared`
submodule SHA in lockstep:

```bash
cd <Anvil-or-Bellows>/shared
git pull
cd ..
git add shared
git commit -m "shared: bump anvil-shared (iceoryx2 → <new-sha-or-tag>)"
```

**Never bump iceoryx2 in only one of the two consumer repositories.** The
shared-memory binary protocol must match exactly between publisher
(Anvil) and subscriber (Bellows).

## License

[AGPL-3.0-or-later](LICENSE) — same license as the rest of the ForgeIEC
ecosystem (Anvil, Bellows, ForgeIEC-Studio).

The vendored iceoryx2 source tree is licensed under
[Apache-2.0](https://github.com/eclipse-iceoryx/iceoryx2/blob/main/LICENSE-APACHE-2.0)
or [MIT](https://github.com/eclipse-iceoryx/iceoryx2/blob/main/LICENSE-MIT)
at the user's option, by upstream.

## Related repositories

- [Anvil](https://github.com/Beerlesklopfer/Anvil) — PLC runtime (publisher)
- [Bellows](https://github.com/Beerlesklopfer/Bellows) — OPC UA gateway (subscriber)
- [eclipse-iceoryx/iceoryx2](https://github.com/eclipse-iceoryx/iceoryx2) — upstream IPC library
- [ForgeIEC-Studio](https://forgeiec.io) — IDE / orchestration layer
