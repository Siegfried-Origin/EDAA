# Recording for replay

Developer builds record the inputs of the anti-aliasing so that it can run
offline on exactly the same frames: DLAA with other presets, sharpening or
exposure, the EDAA TAA, FSR, XeSS, and changed motion vectors, compared side by
side with numbers instead of one more flight each.

## Recording

`Ctrl+Shift+R` (or "Start recording" in the developer section of the overlay)
starts and stops; a high tone marks the start, a lower one the end of the
writing (a long low tone: not saved, see the status line). A line in the top
left corner of the game shows the same, also with the overlay closed: "REC in
3" (countdown), "REC 1.4 s", "saving 40 %", then "saved" (or the error) for
five seconds. "Start delay (s)" (developer section, default 0) waits that
long after Ctrl+Shift+R before recording, to get moving first; pressing again
during the countdown cancels. Status:
the line next to the button ("REC 2.1 s, 126 frames, 2.4 GB of 4.0").

* What: per frame, at the resolve (`passes.cpp`, after the motion vectors and
  the reactive mask, before any anti-aliasing): the jittered HDR scene (the
  game's format, R11G11B10 in Odyssey), the depth (R32_FLOAT copy,
  reverse-Z), the motion vectors (RG16F), the vehicle mask and the reactive
  mask (R8), Elite's tonemap constants (`cb2`, 256 bytes) and adapted
  luminance as captured at the previous frame's tonemap
  (`docs/reverse-engineering.md`, section 10), and the frame's parameters
  (`recorder::frame_params`: jitter, cameras, vehicle motion, the motion
  pass's constants, the AA settings).
* Where: the centre 1600x900 of the frame, or the whole frame ("Whole
  frame"). 13 bytes per pixel: about 19 MB per frame for the centre, 75 MB for
  a 3200x1800 frame.
* Until: the stop, the memory limit ("Memory limit (MB)", 4096 by default),
  or a change of the render size or format. The frames stay in memory while
  recording and are written after the stop, into
  `<capture folder>\recordings\<date-time>\`, if the disk has room for them
  plus 512 MB.
* The game never waits: the inputs are copied into a ring of 8 staging
  texture sets; 2 frames later they are mapped with `DO_NOT_WAIT` (retried
  next frame while the GPU is busy), a worker thread below normal priority
  copies them into memory, and the render thread unmaps them. With the ring
  full the frame is dropped; the next recorded frame carries `flag_gap` and
  replays reset their history there.
* Cost while recording: the copies on the GPU, one depth copy draw, and the
  memory. Nothing while idle besides one call per frame.

## Files

| File | What |
|---|---|
| `record.json` | format `edaa-record-1`, add-on version and build, frames, dropped frames, render size, crop, plane formats |
| `params.bin` | one `recorder::frame_params` per frame (`params_bytes` in `record.json`) |
| `tonemap.bin` | per frame 256 bytes of `cb2` and 16 bytes of the luminance texture (texel 0 = L); valid when `flag_tonemap` is set |
| `color.bin`, `depth.bin`, `motion.bin`, `mask.bin`, `reactive.bin` | per frame the crop, rows top to bottom, tightly packed; `reactive.bin` is zero where `flag_reactive` is not set |

`tools/record_check.py <folder>` checks a recording and prints its frames.
