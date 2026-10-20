# Camera motion reference

The user supplied [SMOOTH COUNTERSTRIKE MOVEMENT](https://youtu.be/zSz6gUeE1XI)
as a reference for camera behavior, explicitly not as a request to reproduce its
edit. The complete public video was downloaded to
`references/zSz6gUeE1XI/reference.mp4`: 1920x1080, approximately 146.734 seconds,
59.94 FPS, VP9 video and Opus audio. A contact sheet samples the cited opening.

Observed examples: 1–2 s use a low, nearly locked shot with foreground depth;
3–4 s cut to another low view of the moving character; around 9–10 s a separate
third-person T-model shot follows the intervening first-person footage. These
are older Counter-Strike footage, not evidence that the same camera commands
are registered in the current CS2 build.

The user clarified the intended shot: an independent third-person camera,
either locked in world space or travelling on an authored route. The player
moves within that frame. Automatic player tracking is not part of this brief.

The implementation goal is a reusable movement system:

- Static XYZ + independent pitch/yaw, or a fixed world point to look at.
- True 3D travel through geometric anchors, with position distinct from aim.
- Constant distance speed along a centripetal Catmull-Rom path and optional
  smooth acceleration/deceleration over the entire shot.
- An orbit around a fixed world point.

The current geometry module also contains optional player-targeting primitives
from an earlier interpretation. They are not used by the cinematic examples
or required for this shot. The Silo path uses a fixed world-space look-at point.

Motion is sampled in advance at export frame times and handed to the native
frame updater. No screen crop/zoom simulates camera travel. The examples use
one principal spatial movement per shot. Control points describe geometry;
constant-speed reparameterization does not promise timestamps at intermediate
anchors. Explicit point orientation uses shortest-yaw interpolation; use
an explicit fixed-point look-at for a camera aimed at one part of the scene.

Collision, automatic visibility/occlusion, depth of field and FOV animation
are outside motion ABI 2. A path can enter a wall: placement must be checked in
the actual game.

The supplied `/home/po1ntov/Downloads/editing_minimal.cfg` is an HLAE authoring
configuration, not a saved camera path. `Q` adds the current camera/time with
`mirv_campath add`, `Z` enables `mirv_input camera`, and `O` enables the path.
It contains no saved XYZ keyframes. `r_nearz 1` changes near clipping, not height.
The Windows recording directory and `mirv_*` commands cannot be used directly
by the native Linux client.

The Linux implementation now uses a late main-view override. The installed
client's `cl_demoviewoverride` references led to an IsPlayingDemo call site in
view setup; local disassembly establishes the live view pointer in r14, float
origin at 0x4a0, angles at 0x4b8, and FOV at 0x498. These are pinned to GNU build-ID
`d5bacd2f5df4a3a68fa8bee8692337993548e14a` and verified instruction bytes; they are
not assumed portable to another Linux build. HLAE's Windows implementation was
a behavioral reference, not a source of Linux addresses.

Before this hook, console commands and even immediate `getpos_exact` readbacks
could disagree with the eventual image. Testing console modes 1..6 did not
establish persistent one-shot origin control. The first verified late-hook run
is `.runs/20261018T134719Z-244d3857`: 120 planned poses, 120 host callbacks, 120
late view writes, 120 raw images, 60 output frames; the reviewed images show an
elevated shot with both players rather than the floor. Recorded original origin
was (492.214,-1862.131,-380.116), overridden with (-273.296,-2236.369,209.000).

The user explicitly rejected the intermediate actor-derived parallel route.
That mode and its example were removed. The current CT flyby has two authored
world-space XYZ keys and two independent rotation keys. It neither reads actor
coordinates nor references a player. A separate fixed-position pan example
uses two rotation keys only. Position and orientation have independent easing
and timestamps; intermediate export samples do not become authoring points.

Motion ABI 2 adds roll to the native final-view writes. Legacy pitch/yaw inputs
default roll to zero. Keyframe orientations accept XYZW quaternions or Source
pitch/yaw/roll (Rz(yaw)*Ry(pitch)*Rx(roll)); interpolation uses normalized
shortest-arc SLERP. Vertical keyframe orientations use a matrix-equivalent Euler
representation at the pole.

Motion ABI 3 adds optional constant FOV (30..140) to each compiled pose. Absence
preserves the game lens. The Linux view hook writes CViewSetup's FOV field and
counts lens writes separately; encoding a requested lens requires matching
write counts and the requested value. This is the engine's lens parameter,
not a promise of an aspect-independent horizontal angle.

The wide variant retains two world-space position keys. A fixed
`point_of_interest` authors two rotation keys at those positions, rather than
reading any actor trajectory. Intermediate rotations use the separate rotation
easing and shortest-arc SLERP, so this is not a per-frame look-at lock even to
the reference point. The point can move slightly within the frame during travel.

The subsequent CT-to-T shot extends the authoring plan to three stages, as
requested: approach the airborne CT, reach the AWP hit at tick 80283, then
translate/tilt toward the fixed T impact area. `hold_last: true` holds the third
world pose from 3.875 to 6 seconds, without a fourth duplicate key. The falling
CT's subsequent coordinates are never sampled by the camera compiler.

The FOV control run `.runs/20261018T140730Z-46541225` keeps the old camera route
and angles but changes the lens from 90 to 105. It confirms 120/120 FOV writes.
At the same demo time, cropping the wider image by tan(90/2)/tan(105/2)=0.7673
and scaling it back matches the old image much more closely than an unchanged
crop (mean RGB error 3.29 versus 35.16 on downsampled frames). This independently
checks the change in rendered projection rather than just the field readback.

The close variant uses a separate lens track: 35 degrees through the hit at
3 seconds, then smootherstep to 45 by 5 seconds, followed by a hold. Constant
`fov` and `fov_keys` are mutually exclusive. The transport already carries FOV
per frame in ABI 3; runtime validation now checks its observed minimum, maximum,
last value and write count. Camera positions remain the same three authored
anchors. One fixed rotation key at 1.5 seconds reframes the jump apex for the
narrow lens; it does not read a live actor or attach the camera to one.

The subsequent `camera-ct-hit-t-cinematic-push.json` removes the requested
pull-back after the user reviewed it. The compiled poses and lens through
3 seconds are identical to the close variant. The lens then stays at 35.
After revealing the T impact area at 3.875 seconds, a fourth position key at
6 seconds brings the elevated camera closer to the dead T; the last pose
holds until 7 seconds (tick range 80091..80539). Position and rotation retain
independent smoothstep timing. The final reference point is authored in world
space, not obtained from an actor at playback time. Its yard floor declaration
is -408 rather than the earlier roof-level plane of 0; the final camera Z=-120
still leaves 288 world units above that plane. This is a height check, not a
collision trace.

The later `camera-ct-playful-safe.json` reauthors the full shot after the user
requested more CT coverage and objected to the victim reaching the bottom edge
at the hit. Five sparse world-space positions form a curved approach and reveal,
followed by the final push-in. Optional Hermite interpolation uses timed
neighbor-derived velocities, continuous across interior anchors and zero at
both endpoints; its linear key time does not mean constant spatial velocity.
The existing segment easing remains the default for older plans. Unknown
interpolation names and Hermite combined with segment easing are rejected.

At the hit the authored camera is (-100,-2120,220), pitch/yaw/roll
(30.7271,28.6139,1.2), FOV 35. A conservative world-axis actor box of +/-20 in
XY and -2..76 above the origin projects inside the central 80% for both actors.
The CT box is approximately x=.219..421, y=.125..655; the T box is
x=.615..681, y=.578..776. This uses the 4:3-reference lens interpretation
inferred from the previous rendered framing, not independently extracted
projection matrices or skinned model bounds. The complete CT trajectory until
the hit also passes an 8% geometric margin check. Visibility and actual image
composition still require rendered review. These actor samples inform authoring
only: the exported motion uses no live player targeting.

On 2026-10-20 the latest local demo was
`match730_003849381187264970947_1107093056_187.dem`. The screenshot at 89413
preceded the requested Zeus kill by 279 ticks: `cat :3` (76561198782272071)
kills `AURA` (76561199075450896) with `taser` at 89692. The attacker exits Hut,
jumps toward the victim, then is crouched directly over them at the hit.
`attackerinair=false` describes the hit instant rather than the preceding jump.

The user subsequently requested a floor-level operator move behind the CT,
then a push-in that retains the complete action. Astra was consulted through
Tonworld (`gpt-6-astra`, xhigh; response `codex-native/gpt-6-astra`, finish stop,
not truncated). It reviewed coordinate snapshots and overview metadata, not
rendered pixels. AURA's earlier Mini-to-A transition established the Main-side
direction, but actual visibility was checked in the client.

The elevated probe (840,-1410,-250) showed Hut and the Zeus pair. Lowering that
same XY to Z=-382 put the camera inside opaque geometry. A further probe at
(840,-1510,-382) was partly blocked by a pillar/entrance construction. The
successful floor-level point is (740,-1450,-382), slightly into the open A hall
from Main, with 34 units above the declared floor Z=-416. This demonstrates why
a floor-plane check cannot replace collision/visibility review.

`camera-nuke-main-zeus-human.json` records 89624..89768 (2.25 seconds), with
Zeus at 1.0625 seconds. Three world-space positions make a small, slow push
toward (724,-1458,-378.5). Six authored rotation keys use roughly seven ticks
of aim lag as a staging preference while full-body framing takes priority.
The lens stays at 56 through the jump/kill and eases to 49 after the hit.
The actor AABB/approximate ragdoll projection passes a 7% margin check at every
demo tick; rendered review remains necessary because those are not skinned
mesh bounds and the dead player's origin does not track their ragdoll.
An initial longer preview exposed a vent obscuring the departing CT near
tick 89800. The final shot ends before that obstruction, after the Zeus,
ragdoll fall and the completed push-in, rather than delivering a blocked tail.
