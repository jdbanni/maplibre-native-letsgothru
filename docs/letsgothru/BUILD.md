# Build commands — letsgothru fork of maplibre-native

Captured on macOS 26.3 (Darwin 25.3.0), Xcode 26.5, on an Apple Silicon
(M-series) Mac Studio. All commands run from the repo root unless noted.

## Prerequisites (brew packages)

```
brew install bazelisk cmake ninja ccache webp libuv jpeg-turbo glfw icu4c
brew link icu4c --force
```

`bazelisk` provides the `bazel` binary; the repo's `.bazelversion` pins
version (bazelisk fetched bazel 9.1.0 for this build).
`ccache` is required by all the cmake presets here — they hard-code
`CMAKE_CXX_COMPILER_LAUNCHER=ccache`.

Xcode 26.5 (the system Xcode at `/Applications/Xcode.app`) provides the
Metal toolchain, iOS SDK 18.x, macOS SDK 15.x.

## Source layout

```
/Users/jdbanni/Desktop/Claude/maplibre-native-letsgothru/
  .git              # 3.8 GB of pack data
  src/              # mbgl-core
  include/
  platform/ios/
  platform/macos/
  bin/mbgl-render.cpp
  vendor/           # ~30 submodules; populated via:
                    #   git submodule update --init --recursive
  CMakePresets.json
  BUILD.bazel
```

Submodules total ~1.5 GB once populated. We deliberately did **not** init
`platform/windows/vendor/vcpkg` (1+ GB, windows-only). Re-add if needed:

```
git submodule update --init platform/windows/vendor/vcpkg
```

## macOS Metal — primary build path for Phase 1

```
PATH="/opt/homebrew/bin:$PATH" cmake --preset macos-metal
PATH="/opt/homebrew/bin:$PATH" cmake --build build-macos-metal --target mbgl-render -j8
```

Output: `build-macos-metal/bin/mbgl-render`.

The `macos-metal` preset (defined in `CMakePresets.json`):
- generator: Ninja
- binary dir: `build-macos-metal/`
- C++ standard: as set by repo
- compiler launcher: ccache
- macOS deployment target: 14.3
- `MLN_WITH_METAL=ON`, `MLN_DARWIN_USE_LIBUV=ON`
- Debug config

Other macOS presets available (see `CMakePresets.json`):
- `macos-opengl` — OpenGL backend
- `macos-vulkan` — Vulkan via MoltenVK (requires Vulkan SDK install)
- `macos-metal-xcode` — same as `macos-metal` but generates an Xcode project

## mbgl-render — smoke test invocation

Once `build-macos-metal/bin/mbgl-render` exists:

```
build-macos-metal/bin/mbgl-render \
  -z 11 -x 1196 -y 765 \
  --style https://tiles.letsgothru.com/styles/outdoors.json \
  --output /tmp/render.png
open /tmp/render.png
```

Tile coords above are roughly Mt Olympos in southern Turkey at zoom 11 —
chosen for the Lycian Way validation context. Adjust to taste.

For a terrain-enabled smoke test, the style JSON needs a `"terrain": {...}`
block referencing a `raster-dem` source. PR #4190 expects standard Mapbox
terrain-rgb tile encoding. Our R2 DEM is currently distributed as PMTiles
(`terrain.pmtiles`) which mbgl-render *should* be able to read via the
PMTiles submodule, but the encoding scheme our DEM uses is an open
question for Phase 2 — see `PROGRESS.md`.

The web app currently fetches terrain from MapTiler
(`https://api.maptiler.com/tiles/terrain-rgb-v2/tiles.json?key=...`) for
live preview; we have not yet validated that our R2 DEM PMTiles file
matches that encoding.

## iOS XCFramework (Bazel) — for distribution

This was **not built in Phase 1** — left as a Phase 2 task. Reference
commands (from upstream developer guide):

```
# Configure Bazel for iOS
cp platform/darwin/bazel/example_config.bzl platform/darwin/bazel/config.bzl
# edit BUNDLE_ID_PREFIX, APPLE_MOBILE_PROVISIONING_PROFILE_NAME, Team ID

bazel build \
  --compilation_mode=opt \
  --features=dead_strip,thin_lto \
  --objc_enable_binary_stripping \
  --apple_generate_dsym \
  --output_groups=+dsyms \
  --//:renderer=metal \
  //platform/ios:MapLibre.dynamic \
  --embed_label=maplibre_ios_"$(cat VERSION)"
```

Expected runtime: 20-40 minutes on this hardware.

## iOS test app (Bazel)

```
bazel run //platform/ios:App --//:renderer=metal
```

## iOS XCFramework (CMake alternative, as of upstream Feb 2026)

```
cmake --preset ios-metal -DDEVELOPMENT_TEAM_ID=YOUR_TEAM_ID
xed build-ios/MapLibre\ Native.xcodeproj
```

## Generating an Xcode project for source navigation only

```
PATH="/opt/homebrew/bin:$PATH" cmake --preset macos-metal-xcode
xed build-macos-metal-xcode/MapLibre\ Native.xcodeproj
```

The Xcode project is read-only against the generated CMake build files;
edit C++ source in your normal editor and re-build with `cmake --build`.

## Cleaning

```
# wipe a single preset's output:
rm -rf build-macos-metal

# nuclear option (also wipes ccache hits for this tree):
rm -rf build-* ; ccache -C
```

## Disk footprint (current)

- repo: 5.7 GB (mostly .git + submodule history)
- build-macos-metal: ~600 MB after a full mbgl-render build
- ccache: configurable, default 5 GB max
