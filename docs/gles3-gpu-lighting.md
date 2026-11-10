# Dynamic lighting on the GPU (Android, OpenGL ES 3.0)

This describes the lighting equations of the CPU renderer as they were before GPU lighting,
how the OpenGL ES 3.0 feature level evaluates the same equations in the vertex shader, and
which paths stay on the CPU.

## Existing CPU lighting

All colors are on a 0-255 scale until they are written as `ColorRGBA`.
`GLOBAL_LIGHT_FACTOR` is 0.85.

Light parameters (`EERIE_LIGHT`, copied into `ShaderLight` for models):

* `rgb` is the light color, flickering lights get a new `rgb` every frame in
  `TreatBackgroundDynlights()`, faded lights lose `rgb` there as well.
* `RecalcLight()` derives `rgb255 = rgb * 255` and `falldiffmul = 1 / (fallend - fallstart)`.
  It runs every frame for the culled dynamic lights (`PrecalcDynamicLighting()`) and the culled
  static lights (`PrecalcIOLighting()`).

Attenuation of one light at distance `d` (shared by both equations below):

    a(d) = intensity * GLOBAL_LIGHT_FACTOR                       if d <= fallstart
           max((fallend - d) * falldiffmul, 0) * intensity * GLOBAL_LIGHT_FACTOR   otherwise

Lambert term: `cos = dot(normal, normalize(light.pos - position))`, lights with `cos <= 0`
are skipped. Normals are used as they are (models: bone or entity rotation applied, not
renormalized).

### Background polygons: `ApplyTileLights()`

Per polygon vertex, every frame, for the visible opaque polygons
(`ARX_PORTALS_Frustrum_RenderRoomTCullSoft()`):

    c = staticColor(vertex)                                  // baked level lighting, ep->v[j].color
    c += sum over tile lights: rgb255 * infra * cos * a(d) * 0.5
    color = (clip(int(c.r)), clip(int(c.g)), clip(int(c.b)), 255)

* The tile lights are the culled dynamic lights within `fallend + 60` (XZ) of the tile center
  (`ComputeTileLights()`). Static lights are baked into `staticColor`.
* `infra` is `(4, 1, 1)` with infravision (`player.m_improve`), `(1, 1, 1)` otherwise.
* The result is written into the room vertex buffer, which is mapped every frame.
* Glowing polygons (`POLY_GLOW`) are written white, transparent polygons keep the color
  `ComputePortalVertexBuffer()` gave them. With infravision the lit red channel is turned
  into the infravision palette.

### Models: `ApplyLight()`

Per vertex (animated models: `Cedric_ApplyLighting()`) or per face vertex (static models and
linked objects: `DrawEERIEInter_Render()`):

    c = ambient                                              // ColorMod::ambientColor
    c += sum over the nearest MAX_LLIGHTS lights: rgb255 * cos * a(d) * materialDiffuse
    c = c * factor + term                                    // ColorMod: special color, infravision, highlight
    color = (clip(int(c.r)), clip(int(c.g)), clip(int(c.b)), 255)

* The lights come from `UpdateLlights()`: the nearest culled static and dynamic lights, at most
  `MAX_LLIGHTS` (6, 10 or 15 by the level of detail setting).
* `materialDiffuse` is 0.5 for `IO_ANGULAR` objects (face normals), 1 otherwise.
* Only opaque faces show the result: transparent faces and invisible entities use a gray
  transparency color, glowing faces are white, items with an active halo take the halo color
  and infravision recomputes the color from its red channel.

`GetColorz()` (how visible the player is, a gameplay value) uses the same terms and is not a
rendering path.

## GPU lighting

Used when the context runs the OpenGL ES 3.0 feature level, the lighting program variants
build and the device limits are sufficient (`GL_MAX_UNIFORM_BLOCK_SIZE` >= 16 KiB, at least 2
vertex uniform blocks and 8 vertex attributes). Otherwise, and for every path listed below,
the CPU code above runs unchanged.

The vertex shader evaluates one generic form of both equations (`arxLighting()` in
`OpenGLRenderer.cpp`):

    c = set.ambient + set.vertexColorWeight * vertexColor * 255
    c += sum over set lights: lightColor * cos * a'(d) * set.diffuse
    color = floor(clamp(c * set.factor + set.term, 0, 255)) / 255, alpha 1

with `lightColor = rgb255 * intensity * GLOBAL_LIGHT_FACTOR` and `a'` the attenuation above
without the intensity factor. `floor()` reproduces the integer truncation of the CPU code.

* Background: `vertexColorWeight = 1` (the vertex buffer keeps the static colors), `ambient = 0`,
  `diffuse = 0.5`, `factor = 1`, `term = 0`. The lights are the culled dynamic lights whose
  `fallend` sphere touches the room bounds, captured in `ARX_SCENE_Update()` like the CPU path.
  Glowing polygons are white in the vertex buffer, so adding light keeps them white.
* Models: `vertexColorWeight = 0`, `ambient`, `factor` and `term` from `ColorMod`,
  `diffuse = materialDiffuse`, the lights from `UpdateLlights()` (same selection and limit).

Light data lives in two `std140` uniform blocks: `ArxLights` (one entry per distinct light of
the scene render) and `ArxLightSets` (one entry per room or model, with up to 32 light indices).
They are filled append-only with unsynchronized maps and orphaned when the sets are reset at
the start of `ARX_SCENE_Render()`. Room draws select their set with a constant vertex
attribute, model triangles carry it per vertex because one batch mixes many models.

### What stays on the CPU

* The OpenGL ES 2.0 feature level and the desktop renderer.
* Light selection: culling, flickering, `UpdateLlights()`, `ComputeTileLights()`
  (still needed for `GetColorz()` and the CPU fallback).
* Infravision (background and items), items with an active halo, invisible or transparent
  models (their color does not depend on light), glowing faces.
* Rooms with more than 32 dynamic lights, rooms touched by a light with `fallend < fallstart`,
  frames with more distinct dynamic lights than the uniform block holds, and models after the
  per-scene set capacity is used up.
* Models rendered outside of `ARX_SCENE_Render()` (book, necklace, spell effects): their
  batches can be drawn after the light sets were reset.

### Known difference

The CPU background path only adds the lights listed for a polygon's tile (`fallend + 60`
around the tile center, horizontally). The GPU adds every light that can reach the room, so a
vertex near a tile corner that is inside `fallend` of a light whose list radius ends just short
of the tile center receives the small remaining contribution instead of nothing. This is the
value the attenuation formula defines (it removes a seam at tile borders). For vertices inside
their 100x100 tile it only happens within ~11 units of `fallend`, where the attenuation is
close to 0.

Rounding: the GPU multiplies the same values in a slightly different order (the intensity is
premultiplied into the light color) and uses `inversesqrt`, so a channel can differ by one
step of 255 where the CPU result lies exactly on an integer boundary.
