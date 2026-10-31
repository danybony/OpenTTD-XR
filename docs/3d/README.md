# Optional 3D renderer

This fork adds an optional, presentation-only 3D view on top of OpenTTD. The
simulation, commands, saves and network protocol stay authoritative and
unchanged; 3D scene data is a disposable projection of the game state.

Build with `-DOPTION_3D_PROOF=ON`, then toggle it in game from the **3D**
toolbar button or with the console command `renderer_3d on|off`.
It requires the `win32-opengl` or `sdl-opengl` video driver and a 32bpp or
40bpp-anim blitter. Other configurations keep the classic renderer.

## OpenGL ES

The backend and the renderer also run on an OpenGL ES 3.0 context, which is the
base for an Android port. Select it on desktop with `-v sdl-opengl:gles`; Android
builds always use it. `src/video/opengl_es.cpp` hands out replacements for the
functions ES lacks and rewrites GLSL 1.50 shaders to GLSL ES 3.00, so call sites
stay shared. Two rules keep code compatible with it:

- Create textures that are rendered to as `GL_RGBA`. `GL_BGRA` textures sample
  through a red/blue swizzle, which only suits uploaded data.
- Write shaders valid in both GLSL dialects, for example `1.0` rather than `1`
  for a float.

## Provenance

The renderer, models and textures come from
[jordanmckin/OpenTTD_3D](https://github.com/jordanmckin/OpenTTD_3D)
(GPL-2.0), commit `d9e08a65d9c90c3755a62fd18bd48f05a0679ee0`, by Jordan McKinney,
which was based on OpenTTD 15.3. Its design notes, Blender sources and asset
tools live in that repository. `src/video/asset3d_art.h`, `asset3d_lod.h`,
`asset3d_generated.h` and `ground3d_textures.h` are generated from them.

## Changes from the original import

- Ported from OpenTTD 15.3 to current master (enum classes, typed bit sets,
  debug facilities).
- Enabled on the SDL2 OpenGL driver, including the middle mouse button.
- Added OpenGL ES 3.0 support and an Android build.
- Removed the trailer-recording console command (`film_cmd.cpp`).

## Android

`os/android/build.sh` builds a debug APK: SDL2 and liblzma, then the game as
`libmain.so`, then a thin activity that unpacks the game data on the first launch
of each version. The script lists the variables it needs. It requires a device
or emulator with OpenGL ES 3.0; the game uses the ES layer above.

Read logs with `adb logcat -s OpenTTD`. In the console, `debug_level driver=6`
prints the per-frame renderer counters.

When testing in the emulator, start it with `-gpu swangle_indirect`. With
`swiftshader_indirect` the 3D view shows only the clear colour and labels.

## Testing without a GPU

Mesa software rendering is refused by default. For a local test build, configure
with `-DCMAKE_CXX_FLAGS=-DGL_ALLOW_SOFTWARE_RENDERER`.
