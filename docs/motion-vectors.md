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

1. **Capture `t33` per frame** (done, `object_motion.cpp`): RenderDoc shows
   one `Map`/`Unmap` of the whole buffer per frame, at the start of the
   frame (3.4 MB, 10240 records), and no GPU write
   (`tools/rd_buffer_usage.py`). The add-on copies the first 32 bytes of each
   record at `Unmap`, with SSE4.1 streaming loads (the mapping is
   write-combined memory, slow to read otherwise).
2. **Match records across frames by content** (done, analysis only): a
   record with a bit-identical previous position is still (or spinning if
   its rotation changed); otherwise it takes the nearest previous record of
   the same kind (rigid: bone base 0, or skinned) and scale within 32 m.
   Matching must be many-to-one: the game writes several records for one
   object (one per pass) and their number changes between frames, so a
   one-to-one match leaves ~1600 records unmatched in the cockpit pair.
   Bone base indices are reallocated every frame. Offline check on the
   capture pairs:

   | Pair | Records | Still | Spinning | Moving | Vehicle | Unmatched |
   |---|---|---|---|---|---|---|
   | Cockpit turn | 7241 | 21 | 7028 | 86 | 62 | 44 |
   | SRV | 1163 | 1046 | 0 | 1 | 114 | 2 |

   "Vehicle" = within 20 m of the camera and moving about like it (any
   motion when the camera is still: animated cockpit parts). Asteroids only
   spin in place, so they are "spinning", not "moving". The
   developer overlay shows these counts live plus the camera reprojection
   error at moving objects on screen (debug view "Moving objects"), to
   decide whether step 3 is worth it.
   Live results (developer overlay, 2560x1440):

   | Scene | Records | Moving on screen | Camera reprojection error |
   |---|---|---|---|
   | Asteroid field, ship still or flying | 3300-4600 | 11-22 (slow drift) | max 0.0-0.3 px |
   | Station concourse on foot, NPCs walking | 9000-9500 | 7-63 | max 0.1-0.9 px |

   Walking NPCs move ~2 cm per frame: their root transform is within 1 px
   of camera reprojection. Their limbs are skinned (bones in `t38`, not
   analysed here), which is where remaining ghosting would come from.
   Asteroids only spin in place. Checked in game: no visible ghosting on
   walking NPCs with the TAA. **Decision (2026-10-11): step 3 is not
   needed for now**; the analysis stays as a developer diagnostic. Revisit
   if ghosting shows on ships passing close.
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
