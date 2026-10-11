# Motion vectors: design

How EDAA builds the motion vectors used by the TAA, and the plan for
per-object motion. Background: [reverse-engineering.md](reverse-engineering.md)
sections 2 and 5.

## What moves on screen, and why

| Pixels | Screen motion comes from | Covered by |
|---|---|---|
| Static world (terrain, stations, asteroid positions, sky) | camera only | camera reprojection (done) |
| Player's vehicle (cockpit, canopy, SRV body) | none: attached to the camera (except head look / shake) | vehicle mask = zero motion (done, stencil) |
| Cockpit holograms / HUD | attached to the camera, no depth written | drawn after the TAA resolve (done) |
| Other moving objects (ships, NPCs, SRVs, spinning asteroids, rotating stations seen from outside) | their own transform | per-object motion (planned) |
| Particles, beams, transparents | own motion, no depth | history rejection / variance clip only |

## 1. Camera reprojection (done)

Per pixel: rebuild the camera-relative position from depth with the inverse
of the frame's main viewProj (`cb1 c270-c273`, jitter removed), add the
camera position delta (`c275`), project with the previous frame's viewProj.
Validated offline to 0.0000 px against static objects
(`tools/check_motion.py`).

## 2. Player's vehicle: stencil bit 0x80 (done)

The game tags its own vehicle in the scene stencil buffer. Read in the
captures at the bloom draw (`tools/rd_stencil.py`):

| Capture | Stencil values | Bit 0x80 |
|---|---|---|
| Cockpit in flight (`Cockpit turn`) | terrain/sky 0x05, asteroids 0x15, canopy struts 0x90/0x95, dashboard 0x94 | cockpit only |
| SRV (`SRV`) | outside 0x05, SRV interior 0x90/0x94/0x95 | SRV only |
| Docked (`Ship inside`) | outside 0x25, cockpit 0xB0/0xB4/0xB5 | cockpit only |
| On foot | 0x00 / 0x04 | none |
| External / free camera (`Coriolis`) | 0x20 / 0x24 / 0x30 / 0x34 | none |

So `stencil & 0x80` is an exact per-pixel "attached to the camera" mask,
present exactly when the camera is inside a vehicle. The add-on reads it
through an `X32_TYPELESS_G8X24_UINT` view of the scene depth and gives those
pixels zero motion. The old distance rule (pixels closer than N m) remains
as an option ("Vehicle mask: Distance to camera").

Limits: head look and camera shake move the cockpit slightly on screen
(the variance clip handles the small error).

## 3. Per-object motion (planned)

For objects that move by themselves. Data available (section 2 of
reverse-engineering.md): instanced models get their transform from the
`t33` structured buffer (336-byte records: position, packed quaternion,
scale; written by the CPU every frame), non-instanced draws from `cb0[4..7]`
(WVP) and `cb0[9..11]` (world). The game keeps no previous transform and the
`t33` record order changes every frame.

Plan:

1. **Capture `t33` per frame**: it is a dynamic buffer updated by the CPU
   (Map/Unmap); copy the records at Unmap like the camera buffer.
2. **Match records across frames by content**: static records keep a
   bit-identical position (7049/7241 in the cockpit pair); moving ones are
   matched by mesh (draw call: index buffer, first index, vertex buffer) and
   nearest previous position, within a speed limit.
3. **Velocity pass**: re-issue the matched instanced draws of the main
   camera into the motion texture with a velocity vertex/pixel shader that
   transforms each vertex with the current and the previous instance
   transform (the game's packed vertex format must be decoded as in its VS),
   depth test `EQUAL` against the scene depth. Non-instanced draws: same with
   the previous `cb0` matrices.
4. Pixels not covered keep the camera reprojection; the vehicle stencil mask
   keeps priority.

Cost and risk: re-issuing draws doubles part of the geometry work and needs
the game's vertex decoding; it is only worth it if moving objects show
visible ghosting with the variance clip. To be evaluated with the auto
captures (other ships passing by, NPCs on foot).
