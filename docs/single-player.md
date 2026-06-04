# Single Player

From the main menu, **Single Player** opens a mode picker, then a map picker, then
plays one run on the chosen (mode × map). Each mode keeps its own per-map
leaderboard (board id `"<mode>-<mapId>"`) and is verified by re-simulation on the
server exactly like every other score (see [anti-cheat.md](anti-cheat.md)).

## Modes

| Mode | Board | Gameplay |
|------|-------|----------|
| **Survival** | `survival-<mapId>` | The classic 2D dodger: blocks fall down a top-down field; weave to last as long as possible. Longest run wins. |
| **Sprint** | `sprint-<mapId>` | The same 2D dodger, but a 900-frame (15.0 s) gauntlet — "clear" it by surviving to the finish. |
| **3D Mode** | `threed-<mapId>` | A **separate** forward-runner: bars rush toward you in perspective and you weave left/right to thread the gaps. New gameplay, its own sim + boards. |
| **Shooter Test** | `shooter-0` | A **separate** Halo-style free-roam arena: walk (left stick), look (right stick), jump (A), crouch (R3), fire (trigger). Recharging shields, seeker enemies, headshots + medals, motion-tracker HUD; shots blocked by cover. Highest **points** wins. Single map; **trust-based** board. |

Survival and Sprint share the 2D dodger simulation
([`../sim/sim-spec.md`](../sim/sim-spec.md)). **3D Mode** is a distinct
deterministic simulation ([`../sim/sim3d-spec.md`](../sim/sim3d-spec.md)), and
**Shooter Test** is another ([`../sim/shooter-spec.md`](../sim/shooter-spec.md)).

Pressing **B** on the map picker goes back to the mode picker (it does not start a
run). Shooter Test has a single map, so it skips the map picker and starts
immediately. During a run, **START** opens a pause overlay (Resume / Friends
Online / Exit Game); pausing advances no frames, so it is invisible to the
deterministic replay.

## 3D Mode

3D Mode reframes the dodger as a **tunnel runner**. Instead of blocks falling down
a 2D field, horizontal bars spawn far away and **close toward the player plane** in
a perspective view; the player occupies the near plane and weaves across the same
320-wide lane to line up with a gap before each bar arrives. It ramps faster than
the 2D game and rewards depth planning (several bars approaching at different
depths). Score is **frames survived** (capped at 3 minutes), so it reuses the same
run/submit/verify plumbing — only the per-frame rules differ.

Because it is a separate simulation, it is cheat-proofed the same way as everything
else: the C client (`sim/dodge3d_sim.c`) and the server
(`homebrew/dodge3d-sim.js`) run **byte-for-byte identical** logic. A run is
**sim-neutral** on the wire (`run/start` records only `seed + map_id`); the board
id `threed-<mapId>` on submit tells the server to verify with the 3D sim
(`simForBoard` in `homebrew/hb-routes.js`). The server replays the submitted
`(seed, input-log, map)` and accepts the score only if it matches, with the same
timing-plausibility check as the 2D path. Map binding is enforced — a `threed-N`
board only accepts a run recorded on map `N`.

Each 3D map (Classic, Rush, Wideload, Blizzard, Zen) is a difficulty preset with
its own leaderboard, mirrored to the website like the other boards (retail board
ids 41–45). The in-game **Leaderboard** browser includes the 3D boards alongside
the multiplayer modes.

> Achievements are reported from 2D Survival runs only; the achievement registry
> evaluates 2D run stats, so 3D Mode does not grant achievements in v1.

## Shooter Test (free-roam FPS)

Shooter Test is a **free-roam first-person arena shooter** (a Halo-style paintball
range). You stand in a large walled arena (`60×60`, with crates/pillars/barriers
for cover) and **walk** with the left stick (a brisk ~15 u/s), **look** freely
(full yaw + pitch) with the right stick, **fire** the paintball marker with a
**trigger**, **jump** with **A**, **crouch** by holding the **right-stick click
(R3)**, and **reload** with **X**. Energy-orb targets fly through the arena and bounce off the walls; you
shoot them by putting the centre reticle on them and pulling the trigger (a
forgiving aim-assist radius makes controller aiming feel good). Paint is **stopped
by the walls and cover** — you can't shoot through a crate or pillar, so flanking
and crouching behind cover matter.

It plays like Halo: you carry a recharging **energy shield** over a **health**
bar (top-centre HUD), watch a bottom-left **motion tracker** for threats, and earn
announcer **medals** for **headshots**, hit **streaks** and rapid multi-kills
(Double/Triple/Overkill/Killing Spree). Most flying **drones** are harmless score
orbs, but red **seekers** hunt you down and bite — let one reach you and it drains
your shield (a directional marker shows where it came from); enough hits and you're
**overrun** and respawn at your start after a moment. Smaller, faster drones are
worth more (1 / 2 / 4) and seekers are worth `3` (more with a headshot). The round
is a fixed **60 seconds**; the player with the **highest total points** wins.
There is a single map ("Arena"), so its board is simply `shooter-0`.

Unlike the dodgers, score is **points**, not frames — and unlike every other mode,
**this board is trust-based, not server-replayed.** Smooth analog free-look isn't
byte-for-byte reproducible, so the server accepts the client's score subject to
**plausibility caps** (a max-score ceiling and a minimum wall-clock for the fixed
60 s round; see [`../sim/shooter-spec.md`](../sim/shooter-spec.md)) and stores it
**unverified**. The board still ranks and displays those scores (in-game and on
the website, retail board id 51), they just don't carry the replay-verified mark.

> Like 3D Mode, Shooter Test uses its own sim and does not grant achievements
> (the achievement registry evaluates 2D run stats).

## Rendering

3D Mode is rendered with a perspective projection (`gui_renderGame3D` in
`game/game_ui.c`): a vanishing-point floor grid and lane rails for depth, each
approaching bar drawn as a depth-scaled slab (small at the horizon, growing and
brightening as it nears), and the player block large at the front. The projection
is **display-only** — it never feeds back into the deterministic simulation, so it
cannot affect scoring or verification. Colors come from the same per-map theme
table the 2D renderer uses.

Shooter Test is rendered by `gui_renderShooter` (`game/game_ui.c`) with a real
software **3D camera** (free yaw + pitch) driven by the FPS sim state: a textured
concrete floor drawn by per-row **floor-casting** with distance fog, the four
arena walls + ceiling as **near-plane-clipped 3D quads** shaded by orientation,
the static crates/pillars/barriers as **back-face-culled shaded boxes**, and the
flying targets as **depth-sorted billboarded energy orbs** with ground shadows.
Energy orbs get an additive **bloom** (a software fake of HDR light bleed); red
seekers pulse and glow. On top: the **true-3D** first-person paintball-marker
viewmodel (built from boxes + round prisms in view space, back-face culled, light-
shaded and depth-sorted, then projected with the same camera — real perspective,
not flat sprites; idle bob, recoil, muzzle bloom), a Halo **ring reticle** that
blooms on a hit, and the
Halo HUD — top-centre **shield + health** bars, a bottom-left **motion-tracker**
radar (sweep + red/amber blips rotated to the player's facing), **medal** popups,
a **directional damage** wedge, score/clock and the ammo readout + reload bar.
Targets, aiming, combat and scoring all happen in the sim's 3D world; the renderer
is display-only. The floor uses the baked concrete tile (`UI_TEX_CONCRETE`);
everything else is shaded in the
CPU framebuffer pass.

You also have a **body**: a low-poly humanoid (`shHumanoid` in `game/game_ui.c`),
built from articulated boxes in a body-local frame (transformed body→world→view,
back-face culled, world-lit and depth-sorted like the gun). It runs a real **walk
cycle** — swinging legs/arms with knee + elbow bend and a vertical bob, driven by
the sim's `stride`/`speed01` (which advance with the actual ground distance you
cover, so the animation stops when a wall blocks you). In first person you see your
own **legs/boots/hips** below the camera (head/torso omitted so they never block
the view); they plant on the floor while grounded and lift with you mid-jump. The
same function renders a **full body** (torso, shoulder pads, arms, helmet with a
gold visor) with per-call team colours — this is the avatar **online opponents will
render with** once Shooter Test becomes multiplayer (players shooting each other),
so the model and its animation are already in place. The model math is mirrored and
visually validated in `assets/preview_shooter.py`.
