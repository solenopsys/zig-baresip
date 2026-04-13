# baresip Zig Wrapper

This wrapper builds upstream `baresip` as a shared library (`libbaresip.so`) via `zig build`.

The wrapper also builds upstream `re` (libre) as a static dependency to avoid external
`libre.so` runtime dependency.

## Build

Prerequisites:

- Zig `0.15.2+`
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

Build for all supported targets:

```bash
zig build -Dall=true -Doptimize=ReleaseFast
```

Single-target output:

- `zig-out/lib/libbaresip.so`
- `zig-out/include/baresip/baresip.h`

`-Dall=true` output:

- target-specific `.so` files are hashed and copied to `../../artifacts/libs`
- `current.json` contains target -> hash mapping

## Notes

- Wrapper builds `baresip` with `MODULES=""` for minimal/core shared library output.
- `re` is built static (`libre.a`) and linked into `libbaresip.so`.
