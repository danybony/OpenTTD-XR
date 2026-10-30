# Optional 3D renderer

This fork adds an optional, presentation-only 3D view on top of OpenTTD. The
simulation, commands, saves and network protocol stay authoritative and
unchanged; 3D scene data is a disposable projection of the game state.

Build with `-DOPTION_3D_PROOF=ON`, then toggle it in game from the **3D**
toolbar button or with the console command `renderer_3d on|off`.
It requires the `win32-opengl` or `sdl-opengl` video driver and a 32bpp or
40bpp-anim blitter. Other configurations keep the classic renderer.

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
- Removed the trailer-recording console command (`film_cmd.cpp`).

## Testing without a GPU

Mesa software rendering is refused by default. For a local test build, configure
with `-DCMAKE_CXX_FLAGS=-DGL_ALLOW_SOFTWARE_RENDERER`.
