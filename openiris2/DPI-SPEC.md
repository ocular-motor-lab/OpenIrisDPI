# The DPI plugin for OpenIris 2: dual Purkinje imaging

**Status:** draft, 2026-11-09. Not yet checked on real DPI images (none are available yet). Since
2026-11-12 it lives here, with the plugin, in a fork of Ryan's repository, and is built outside
OpenIris 2 with its plugin kit ([OpenIris 2's PLUGINS-SPEC 22](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#22-plugins-built-outside-openiris-2-draft)).
Links to OpenIris 2's documents (PLUGINS-SPEC, SPEC) are to its repository.

**Credit.** The method is **Ryan Ressmeyer's**: the OpenIrisDPI plugin for OpenIris
(`ryan-ressmeyer/OpenIrisDPI` on GitHub, branch `master`, commit `71785d4`; Horwitz lab, University
of Washington, and Yates lab, UC Berkeley), described in

> Ressmeyer, R. A., et al. "OpenIrisDPI: An open-source digital dual Purkinje image eye tracker for
> visual neuroscience." *Journal of Neuroscience Methods* (2026): 110693.

The stages here are a **new implementation** in OpenIris 2's style, written from a reading of that
plugin's code and its wiki (*Algorithm and Parameters*). No code was copied. His C# plugin is next
to this folder, in `../OpenIrisDPI`: this folder is part of a fork of his repository
(`ocular-motor-lab/OpenIrisDPI`), under his license, GPL-3.0.

## 1. What DPI measures

A light on the eye makes four reflections, the **Purkinje images**: P1 from the front of the
cornea, P2 from its back, P3 and P4 from the front and back of the lens. P1 is the bright corneal
reflection every infrared tracker sees (the "glint"). P4 is about 100 times dimmer, a little smaller,
and is seen through the pupil. When the eye **translates**, P1 and P4 move together; when it
**rotates**, they move by different amounts. So **P4 − P1** measures eye rotation while ignoring
small head or camera movement. That is what makes DPI precise (Ryan reports sub-arcminute precision
at 500 Hz).

Ryan's plugin finds, per eye and per frame: the pupil (to know where P4 can be), P1 and P4. It
doesn't calibrate: the P4 − P1 signal is turned into degrees downstream (in his setup, gains,
offsets and a rotation in a separate analog-output program).

## 2. Ryan's method, step by step

In his order (the wiki's "Algorithm and Parameters" page and the C# code agree):

1. **Smooth** the eye image with a Gaussian blur (`BlurRadius`, 2 px).
2. **Rough pupil center:** pixels darker than `PupilThreshold` (30); centroid of those pixels on a
   downsampled image.
3. **Pupil fit:** in a circle of `PupilSearchRadius` (200 px) around the rough center, find the edge
   pixels of the dark region and take their **convex hull**. The hull bridges the notches and holes
   that P1 and P4 cut into the dark pupil. The pupil is the ellipse with the same first and second
   moments as the filled hull (`Moments`, the default), or an ellipse fitted to the hull's points
   (`EllipseFit`).
4. **P1:** in the same circle around the pupil, the centroid of pixels brighter than `P1Threshold`
   (245) on a downsampled image gives a rough P1. A square of half-width `P1RoiRadius` (40 px)
   around it gives the exact position: the centroid of brightness above the threshold, and the
   spot's size from its second moments. P1 is "not found" when the spot's smaller diameter is below
   `P1MinDiameter` (5 px).
5. **P4:** blank out the P1 square, keep only the inside of the pupil (the hull, eroded by
   `PupilMaskErodeRadius`, 3 px, so bright iris pixels at the edge don't count), and take the
   **brightest pixel** as the rough P4. Refine it in a square of half-width `P4RoiRadius` (15 px):
   centroid of brightness above the **pupil threshold**. P4 is "not found" when its smaller
   diameter is below `P4MinDiameter` (0.5 px), or when P1 wasn't found.

Outputs: the pupil ellipse, P1 and P4 (center, size, angle), and a quality of 50 % per
reflection found. The wiki warns that P4 is the brittle step: any pixel in the pupil brighter than
P4 (a stray reflection, iris peeking in at the edge, P1's halo) is taken for P4.

## 3. In OpenIris 2: three steps

Ryan's plugin is one function. Here it becomes three stages, each a step of the profile's eye
pipeline ([PLUGINS-SPEC 18](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#18-steps-and-connections-not-slots)), connected to
the others by what it reads, and each replaceable on its own
([PLUGINS-SPEC 2](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#2-one-rule-inputs-and-outputs)). Together they are the `dpi`
plugin ([this folder](README.md)), built against OpenIris 2 with its plugin kit and loaded at
start-up like the others ([PLUGINS-SPEC 17](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#17-loading-plugins), 22). It needs
nothing beyond OpenIris 2's core, where
the pupil and glint thresholds it shares with the core's stages stay. The steps, in the order
they run:

| Step | Ryan's step | Requires (can use) | Provides |
|---|---|---|---|
| `pupil.centroid.approx` (the core's) | 2. rough center | — | `oi/pupil_approx/center`, `oi/pupil_approx/size` |
| **`glints.p1`** | 4. P1 | (`oi/pupil_approx/center`) | **`dpi/p1`**, `oi/glints` (P1, labeled) |
| **`pupil.hull`** | 3. pupil fit | (`oi/pupil_approx/center`) | `oi/pupil/center`, `oi/pupil/size`, `oi/pupil/ellipse` |
| **`dpi.p4`** | 5. P4 | `dpi/p1`, `oi/pupil/center`, `oi/pupil/size` (`oi/pupil/ellipse`) | **`dpi/p4`**, **`dpi/p4_p1`** |

- **The rough center** is the existing first-pass stage: the same idea (centroid of dark pixels on
  a coarse grid), and it shares the pupil threshold.
- **P1 is a glint,** found before the refined pupil (neither reads the other, so the profile's
  order decides). It searches around the rough pupil center instead of the fitted one; with a
  200 px search circle the difference doesn't matter. It also writes P1 as an ordinary glint
  (`oi/glints`, one item, labeled `P1`), so everything that knows glints works with no
  change: the app draws it, and a pupil-to-glint calibration can use it.
- **The pupil** is Ryan's convex-hull method, as its own pupil stage. It provides the core pupil
  facets, so everything after it (calibration, the Monitor, recordings) sees an ordinary pupil.
- **P4 comes last,** because it reads both the pupil (where to look) and P1 (what to ignore): its
  connections put it after them. It reads the pupil through its outputs, not through the hull
  stage, so it works with any pupil step it's connected to: with an ellipse it uses the ellipse,
  otherwise a circle of the pupil's diameter.
- **The DPI signal,** P4 − P1, is its own small output, so the Monitor can plot it and remote
  clients can take it by ID without computing it.

Each stage smooths only the part of the image it reads. A shared blurred image waits for image
outputs ([PLUGINS-SPEC 3.2](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#32-images)).

## 4. The `dpi/` outputs

Definitions in [outputs/dpi/](outputs/dpi/), generated with the
plugin (`dpi/outputs.hpp`: `oi::out::dpi::P1`, `P4` and `P4P1`) and registered when its module
loads; recordings and calibration files carry them, so they read without the plugin
([PLUGINS-SPEC 18.10](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#1810-output-definitions-load-with-their-module)). All per
view, in image pixels.

| ID | Fields | Status |
|---|---|---|
| `dpi/p1` | `p1` (point, px), `size` (px) | `good`; `failed` when no spot is big enough |
| `dpi/p4` | `p4` (point, px), `size` (px) | `good`; `failed` when no spot, or P1 missing; the pupil's status (`no_pupil`…) without a pupil |
| `dpi/p4_p1` | `p4_p1` (point, px): P4 minus P1 | `good` when both are; else the first one's problem |

- **Position:** the centroid of the spot's brightness above the background level (P1's threshold
  for P1, the pupil threshold for P4), sub-pixel. Image coordinates, pixel centers at integers
  ([SPEC 9.1](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/SPEC.md)).
- **Size:** the diameter of the disk with the area of the spot's moment ellipse,
  √(major × minor), where each axis is 4 √λ of the brightness-weighted second moments. For a
  uniform disk this is its diameter; for a Gaussian spot of width σ, 4σ.
- Recording columns: `left_dpi_p1_x`, `left_dpi_p1_size`, `left_dpi_p4_x`, `left_dpi_p4_p1_x`…
- Ryan's spot angle and elongation are not kept (nothing used them); a later facet can add them.

## 5. Settings

In pixels, like the other built-in stages for now (PLUGINS-SPEC 10 asks for mm; that waits for
every view's scale). **Subject** settings are per eye and shown in Adjust; the rest are advanced.

| Setting | Ryan's | Default | Role | Used by |
|---|---|---|---|---|
| `pipeline.pupil.threshold` | PupilThreshold | 60 (profile: 30) | subject | approx, `pupil.hull`, `dpi.p4` (P4's background) |
| `pipeline.glints.threshold` | P1Threshold | 230 (profile: 245) | subject | `glints.p1` (shared with `glints.blob`) |
| `pipeline.dpi.search_radius` | PupilSearchRadius | 200 px | subject | `pupil.hull`, `glints.p1` |
| `pipeline.dpi.blur` | BlurRadius | 2 px | advanced | all three |
| `pipeline.glints.p1.roi_radius` | P1RoiRadius | 40 px | subject | `glints.p1` |
| `pipeline.glints.p1.min_diameter` | P1MinDiameter | 5 px | advanced | `glints.p1` |
| `pipeline.glints.p1.rough` | (new) | largest | advanced | `glints.p1`: where P1's rough position is (7) |
| `pipeline.dpi.p4.roi_radius` | P4RoiRadius | 15 px | subject | `dpi.p4` |
| `pipeline.dpi.p4.min_diameter` | P4MinDiameter | 0.5 px | advanced | `dpi.p4` |
| `pipeline.dpi.p4.max_diameter` | (new) | 0 (none) | advanced | `dpi.p4`: a wider spot (P3) isn't P4 (7) |
| `pipeline.dpi.p4.edge_margin` | PupilMaskErodeRadius | 3 px | advanced | `dpi.p4` |
| `pipeline.dpi.p4.p1_exclusion` | (P1RoiRadius) | 40 px | advanced | `dpi.p4` |

Shared paths are one setting: the pupil threshold the user tunes for the pupil is also P4's
background level, as in Ryan's plugin. The five subject settings are the five sliders of Ryan's
quick settings.

## 6. The plugin's profiles

In [profiles/](profiles/), one file each, put in the module and listed with OpenIris 2's own
when it loads ([PLUGINS-SPEC 22.4](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#224-profiles-from-plugins)).

**"DPI rig"**, for a real rig:

- **Source:** Teledyne cameras (Spinnaker), 500 Hz, one camera per eye (Ryan's "Spinnaker Dual
  Camera"). Which camera is which eye: `source.spinnaker.camera0.serial` and `camera1.serial`
  (empty: the order Spinnaker lists them); set them and save the profile under your own name.
- **Pipeline:** `pupil.centroid.approx` → `glints.p1` → `pupil.hull` → `dpi.p4`.
- **Settings:** Ryan's defaults (pupil threshold 30, P1 threshold 245, the rest as in 5).
- **Calibration:** `gaze.dpi`: P1 and P4 to degrees, 9 targets at ±8° (see 8).
- **Monitor:** P4 − P1 (`dpi/p4_p1`) and the pupil.
- **Adjust** hides the pupil stage's softness and maximum radius, which no DPI stage uses.

**"Synthetic eyes, DPI"**, to try the pipeline without a rig: the synthetic eyes with P4 turned on
(`source.synthetic.p4`), at 500 Hz. The synthetic eye's glint is P1; P4 is a dim spot near the
pupil's center that moves almost as much as the pupil, while P1 moves half as much, so P4 − P1
follows the eye's rotation (the synthetic eyes don't shift). The eyes are small (pupil radius 28 px, spots
about 3 px across, 25 px apart), so the profile sets smaller values than Ryan's: search radius
80 px, smoothing 1 px, P1 threshold 200 and minimum 1 px, P1 square 8 px, P4 square 6 px, P1
exclusion 8 px. Same pipeline, calibration, Monitor and Adjust as the rig's. P4 is off in the other synthetic
profiles (ordinary infrared trackers rarely see it), so their results don't change.

**"Rendered eyes, DPI"**, with exact ground truth: the rendered-eye plugin's DPI setup (ray-traced
eyes, a camera straight ahead at about 39 px/mm, one LED), so P1 to P4 come from the cornea and the
lens as in a real eye, and the eyes look at calibration targets. 100 Hz on a 432 × 336 region
per eye, which holds the pupil (about 190 px across) wherever it moves, at ±8° too.
At the renderer's default exposure (0.8 ms) P3 and P4 saturate in their centers, which Ryan's
method doesn't expect, so the profile halves it (0.4 ms): only P1 saturates, P4 peaks at about
150 gray levels, the iris is about 55 and the pupil about 10 (pupil threshold 30, Ryan's default).
Search radius 110 px, just above the largest pupil, keeps the lashes out of the hull. P3 stays as
bright as P4 and wanders across the pupil with the gaze, closer to P1 than P4 on some frames and
farther on others, so no P1 exclusion separates them; it's wider than P4 (about 10 px against 5), so P4's maximum
width (7 px) sets it aside, and the exclusion (20 px) only covers P1 and P2. Calibration:
`gaze.dpi`, as the rig's.

## 7. Differences from Ryan's plugin

Carried over: the whole method of 2, with Moments as the pupil fit, his defaults, and his rule that
P4 needs P1.

Changed, on purpose:

- **Split into stages** (3), each reading the others through outputs.
- **P1 searches around the rough pupil center,** not the fitted one (it runs first).
- **P1's rough position is in the largest bright region** (`pipeline.glints.p1.rough` = `largest`,
  the default), not the centroid of every bright pixel (`all`, Ryan's rule, still a choice). Any
  other reflection above the P1 threshold (a saturated P4, a second light) pulls Ryan's centroid
  away from P1, and the square where P1 is measured then misses it or cuts it (a synthetic eye
  with a saturated P4 and a 15 px square shows it). P1 is the largest reflection unless another
  saturates over a larger area: then nothing tells them apart by brightness, and the image should
  be darker (as on a real rig, where only P1 saturates).
- **Sub-pixel pupil edge.** The hull is built from each row's and column's outermost dark pixels,
  with the edge placed where the gray level crosses the threshold (interpolated), and its moments
  are computed exactly from the polygon. Ryan's uses the edge pixels (both sides of the edge) of
  the thresholded image and a downsampled filled hull. Same idea, less pixel noise.
- **Pupil quality:** the fraction of the edge points that lie on the hull's outline. A point more
  than 1 px inside the hull is the edge of something cutting into the pupil (P1 on its border, an
  eyelash), so the pupil's own edge is unseen there: the meaning of `oi/pupil/center`'s quality.
  Ryan's had none.
- **Second moments** use the standard eigenvalues; Ryan's formula drops a factor of 4 on the cross
  term, which only matters for tilted, elongated spots.
- **P4's mask** is the pupil ellipse shrunk by the edge margin, not the eroded hull polygon (the
  stage reads only outputs); P1 is excluded as a disk of `p1_exclusion` around P1, not a square.
- **No pupil, no P4:** with no pupil found, P4 takes the pupil's status (`no_pupil`…). Ryan's
  searched the whole circle instead.
- **No pupil, P1 anyway:** without a rough pupil center, P1 is searched in the whole eye region.
- **P4 can have a maximum width** (`pipeline.dpi.p4.max_diameter`, 0 by default: none, as
  Ryan's). Ryan's P4 is the brightest pixel in the pupil, and his wiki warns that anything brighter
  is taken for P4. P3, the reflection from the front of the lens, is larger than P4: with a maximum
  width, a brightest spot wider than it is set aside and the next brightest tried (up to four
  times). On the rendered eyes, where P3 is as bright as P4, this is what makes P4 reliable.
- **Smoothing:** a binomial kernel of the same width (radius 2: [1 4 6 4 1]/16, σ = 1 px against
  OpenCV's 1.1 px), with no OpenCV needed. P1's rough search uses block averages of the smoothed
  image, as his downsampling does: without the smoothing, one noisy pixel at the threshold pulled the
  rough position off P1 on the synthetic eyes.

Not carried over:

- **`EllipseFit`** (fitting an ellipse to the hull's points). Moments was his default and the more
  stable; an ellipse-fit pupil can come later as its own stage.
- **`DsFactor`** (trading precision for speed by downsampling). The stages here run at full
  resolution, except P1's coarse search (2 × 2 blocks at the default radius).
- **Debug images** (the overview, pupil, P1 and P4 views). They wait for debug image outputs
  ([PLUGINS-SPEC 3.2](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#32-images)).
- **The `Cropping` setting:** OpenIris 2's eye regions and camera ROI do this.
- **The UDP client** (Python): OpenIris 2's remote clients take `dpi/…` outputs by ID.
- **The IR safety script** in the repository: about the illuminator, not tracking.

## 8. What needs real DPI data

Tested only on synthetic images (`tests/test_dpi.cpp`): an elliptical pupil (210 × 190 px) with an
anti-aliased edge, a saturated P1 (about 8 px across) and a Gaussian P4 (σ 1.5 px) at known
sub-pixel positions, with and without noise. Worst errors without noise: pupil center 0.005 px,
P1 0.03 px, P4 0.06 px (the usual sub-pixel bias of a thresholded centroid on a spot a few pixels
wide); with noise of 3 gray levels, mean errors under 0.05 px.

On the synthetic eyes with P4 (profile "Synthetic eyes, DPI"), over 4 s of eye movements with
noise: pupil within 0.06 px of the true area centroid, P1 within 0.3 px (mean 0.06 px), P4 within
0.2 px (mean 0.09 px); P4 − P1 spans 24 px horizontally. P1 is worst when it sits on the pupil's
edge, half on the brighter iris, which pulls its centroid. Ryan's defaults wouldn't work on these
small eyes (a P1 under 3 px across, smoothed below a 245 threshold); they assume a P1 well over
5 px across.

On the rendered eyes (profile "Rendered eyes, DPI", `tests/test_dpi_rendered.cpp`),
the stages alone on 240 images (1.2 s of movements, both eyes): pupil within 0.45 px of the true
centroid seen through the cornea, P1 within 0.16 px, P4 within 0.06 px, none missed; at the 18
target positions of a ±8° and a ±5° grid, P4 within 0.05 px. Through the tracker with the profile,
the same within 0.35, 0.15 and 0.05 px. In these images P3
is as sharp as P1 and P4 (the renderer's camera is a pinhole); a real high-magnification camera
focused on the pupil would blur it.

To check on a real rig:

1. **Defaults:** thresholds, ROI radii and the 40 px P1 exclusion on real images, at both
   magnifications (Ryan's olDPI and ohDPI).
2. **P4 precision** against Ryan's plugin on the same video: the same frames through both, P4 − P1
   compared sample by sample. The sub-pixel hull and the ellipse mask should change the pupil a
   little and P4 not at all.
3. **Speed** at 500 Hz with real image sizes. On a synthetic 480 × 400 eye image with the default
   200 px search radius, the four stages take 0.8 ms per eye image (measured on the development
   Mac while it was heavily loaded; `openiris_tests "[.timing]"`). At 500 Hz two eyes have 2 ms.
   Not profiled yet.
4. **Failure cases:** P4 near the pupil edge, P1 crossing the pupil edge, eyelashes in the search
   circle, P1 and P4 overlapping (looking along the illuminator's axis).
5. **Calibration:** the three DPI profiles use `gaze.dpi`, this plugin's calibration
   ([PLUGINS-SPEC 8.6](https://github.com/ocular-motor-lab/OpenIris2/blob/main/docs/spec/PLUGINS-SPEC.md#86-one-regression-for-gaze)): P1 and P4 to degrees, as two
   points whose weights the fit finds (near +1 and −1, P4 − P1's gain, offset and rotation when they
   cancel), with a rule in the fit that both moving together isn't gaze; linear, more terms as
   options (PLUGINS-SPEC 8.10, decisions 8, 12, 13, 2026-11-11); 9 targets at ±8°. On left-out targets: 0.08–0.12° on the synthetic eyes, about 0.15° on the rendered eyes.
   Real data will tell which order wins and how far P4 stays visible. Ryan's calibrates outside
   the tracker.
6. **P4's maximum width** on real images, where P3 is large and dim; and whether a rig needs it.

## 9. Open questions

1. Whether `dpi/p1` should exist at all, given P1 is also written as a glint labeled `P1`
   (`oi/glints`). Kept for now: DPI consumers get a pair of facets with the same fields, and
   recordings get clear columns.
2. Whether a DPI rig wants a monocular profile (one camera, one eye).
3. The interface: drawing P4 and its search regions (see the hand-over notes for the UI session).
