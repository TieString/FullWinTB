# Sticker artwork

`sticker_atlas.bin` is the original v21.3 resource. It holds a 256-entry premultiplied BGRA palette followed by 13 row-major frames. Each frame is 112 × 64 and stores one palette index per pixel. Index 0 is transparent. The original baked shadow and translucent residue are part of the curl artwork.

The source is `sticker_art.svg`. The frames were rendered with the official [Sticker Forge embed module](https://sticker.oooo.so/embed/sticker-forge.es.js), SHA-256 `9deb1c0121df803f5f34dda599a2871a539ac7c00eb843485851d5b739be8612`. See `sticker-forge-LICENSE.txt` for its MIT license.

The renderer used a transparent 640 × 380 viewport with zero outline and tilt. The peel radius was 0.145, stiffness was 0.66, and the maximum angle was 3.14 radians. The light direction was `(-0.45, 0.5, 0.74)`, with intensity 1.08, ambient light 0.39 and softness 0.44. The satin backing used `#f2efe6`, with gloss 0.62 and roughness 0.34. The shadow used opacity 0.38, blur 18, distance 16 and angle 63. Audio was disabled.

The first frame is flat. The remaining 12 frames follow a pointer press at `(135, 158)` with successive increments of `(14, 7)`. Each capture uses the same crop `(74, 90, 566, 334)` and Lanczos resampling. Alpha values below 3 are cleared. The palette uses weighted median cut on premultiplied colors with separate opaque and translucent entries.
