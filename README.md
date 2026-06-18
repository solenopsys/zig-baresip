# baresip Zig Wrapper

This wrapper builds:

- upstream `baresip` as shared library (`libbaresip.so`)
- runtime C-ABI helper `libbaresip_wrapper.so` (dlopen-based thin API layer)

The wrapper also builds upstream `re` (libre) as a static dependency to avoid external
`libre.so` runtime dependency.

## Build

Prerequisites:

- Zig `0.16.0+`
- `cmake` and `ninja`

Initialize vendor submodules:

```bash
git submodule update --init --recursive \
  native/wrapers/baresip/vendor/re \
  native/wrapers/baresip/vendor/baresip
```

Build for current target:

```bash
zig build -Doptimize=ReleaseFast
```

Build only the C-ABI helper (skip upstream core build):

```bash
zig build -Dffi_only=true -Doptimize=ReleaseFast
```

Build for all supported targets:

```bash
zig build -Dall=true -Doptimize=ReleaseFast
```

Single-target output:

- `zig-out/lib/libbaresip.so`
- `zig-out/lib/libbaresip_wrapper.so`
- `zig-out/include/baresip/baresip.h`
- `zig-out/include/baresip_wrapper.h`

`-Dall=true` output:

- target-specific `.so` files are hashed and copied to `../../artifacts/libs`
- `current.json` contains target -> hash mapping

## Notes

- Wrapper builds `baresip` with `MODULES=""` for minimal/core shared library output.
- `re` is built static (`libre.a`) and linked into `libbaresip.so`.

## Wrapper API Coverage

`baresip_wrapper.h` exposes C-ABI helpers for:

- Lifecycle (`init`, async `re_main` run, `stop`, running state, loop error).
- SIP UA operations (`add_ua`, optional register, `connect_call`, `answer_call`, `hangup_call`).
- Runtime module loading (`module_load` when symbol is available).
- Event bridge via `bevent_register` (`bsw_set_event_callback` with `event/ua/call/text` payload).
