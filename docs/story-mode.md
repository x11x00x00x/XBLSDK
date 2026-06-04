# Story Mode (single-player campaign)

Story Mode is the Test Game's single-player campaign: an ordered run of
**10 levels** across two acts that escalate in difficulty, wrapped in a light
narrative ("The Courier" delivering a package across the city, then the long run
back). It sits next to free-play **Single Player** on the main menu.

It is built entirely on the existing, verified pieces — **no new sim maps,
boards, or server endpoints**. Each level is one of the built-in deterministic
maps, so every Story Mode score is verified by server replay and ranks on that
map's normal per-map leaderboard exactly like free play.

## The levels

Each act cycles the maps easy → hard. The built-in maps don't just start
harder — they **ramp at different rates** (see `DODGE_MAPS[]` in
`sim/dodge_sim.c`), so the difficulty curve keeps changing shape. On top of that
the **goal (par) climbs every level**: +120 frames (2.0 s) per step, from 5.0 s
on Level 1 up to 23.0 s on Level 10. Par is measured (and shown in the menu) in
**frames** — you must survive *more frames* than the previous level to clear it.

| # | Level | Map | Feel / ramp | Par (clear) |
|---|-------|-----|-------------|-------------|
| 1 | The Quiet Outskirts | Zen | Slowest ramp (speed/400) | 300 f (5.0 s) |
| 2 | The Cargo Yards | Wideload | Wide blocks, easy ramp (speed/320) | 420 f (7.0 s) |
| 3 | Downtown Drift | Classic | Steady mid ramp (speed/300) | 540 f (9.0 s) |
| 4 | Rush Hour | Rush | Hard, fast ramp (speed/240) | 660 f (11.0 s) |
| 5 | Whiteout Pass | Blizzard | Fastest ramp, narrowest gaps (speed/200) | 780 f (13.0 s) |
| 6 | The Long Way Back | Zen | Gentle ramp, far longer haul | 900 f (15.0 s) |
| 7 | Midnight Freight | Wideload | Wide lanes, endless night | 1020 f (17.0 s) |
| 8 | Neon Gridlock | Classic | Steady ramp, cruel clock | 1140 f (19.0 s) |
| 9 | Redline | Rush | Fast ramp, sustained | 1260 f (21.0 s) |
| 10 | The Last Mile | Blizzard | Fastest ramp, longest haul | 1380 f (23.0 s) |

"Par" is the frame count (survival time) you must reach to **clear** a level and
unlock the next one. The level-select list shows each level's goal in frames.

## Progression

- Level 1 is always unlocked. Clear a level (survive ≥ its par) to unlock the
  next.
- The level-select menu shows each level's goal in frames plus its status:
  `LOCKED`, a `best <frames>` for unlocked-but-uncleared levels, or `CLEARED`
  once you beat par.
- Progress is stored in the **native Xbox save** (the `PROFILE` slot, see
  [saves.md](saves.md)): the highest unlocked level (`story=`) and the best
  frames survived per level (`b0=`…`b9=`). Old profiles load fine — they simply
  start with Level 1 unlocked.

## Leaderboards

A Story Mode level is just a run on its map, so its score is submitted to that
map's existing `survival-<mapId>` board and is **replay-verified** server-side.
That means:

- Story Mode and free-play Single Player on the same map share one leaderboard
  (same deterministic gauntlet → same competition).
- The website already lists these boards per map under **Survival** on the game
  page — no website change is needed for Story Mode scores to appear.

## Flow (per level)

1. **Intro screen** — level title + two lines of story + the survival goal.
2. **The run** — opens a server run on the level's map, plays the deterministic
   dodger (every input recorded), submits the score to `survival-<mapId>`, and
   reports achievements — identical to free play.
3. **Result screen** — on a clear: the outro line and "Next level unlocked!"
   (or campaign-complete on Level 10). Otherwise: how long you lasted vs. par.

## Anti-cheat

Nothing special: because every level is a real map run, the
[anti-cheat](anti-cheat.md) model is unchanged. Unlocking the next level is a
**local** convenience gate (stored in the save); the score that lands on the
public board is still the one the server re-simulates and accepts. You can't
forge a leaderboard placement by faking campaign progress.
