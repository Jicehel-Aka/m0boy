#!/usr/bin/env python3
"""Play a Game Boy ROM in the gbl host harness with a local decision model.

The harness (<name>_profile --agent) stops at a VBlank, prints the IO
registers, OAM and VRAM, and waits for the next buttons.  This script turns
that into a short text picture of the screen (background and window tiles
classed by how full they are, sprites, the sprite that answers the
d-pad), asks Ollama's decision endpoint (/v1/systemone, models such as
tev1) what kind of screen it is and which button to press, and answers with
"<buttons>:<frames>" steps.  The steps are written as a gbl input file, so
the game replays exactly with --input.

The model only picks among buttons; the script adds what it cannot see:
when no new code has run for a while it mixes in more random choices, and
menus get short taps (press, release) while play gets held directions.

  ai_play.py --harness out/X/host/build/X_profile --out coverage-ai1.input \\
             --seconds 120 --seed 1 -- --coverage coverage-ai1.cov
  ai_play.py --check [--model tev1:0.8b]   exit 0 when the model answers
"""
import argparse
import json
import random
import subprocess
import sys
import time
import urllib.error
import urllib.request

ACTIONS = {
    "right": "move or go right",
    "left": "move or go left",
    "up": "move up, climb, or move a menu cursor up",
    "down": "move down, crouch, or move a menu cursor down",
    "a": "press A: jump, confirm, talk, or go to the next text",
    "b": "press B: attack, fire, run, or cancel",
    "start": "press Start: start the game from the title screen, or pause",
    "right+a": "jump to the right",
    "right+b": "run or attack to the right",
    "left+a": "jump to the left",
    "none": "wait without pressing anything",
}
SCREENS = {
    "title": "title screen or 'press start' screen",
    "menu": "menu, options, or a selection list with a cursor",
    "dialog": "a text box or message waiting for a button",
    "gameplay": "the player moves a character or object in the game world",
    "transition": "blank, black, loading, or a cutscene that plays by itself",
    "gameover": "game over, continue, or results screen",
}
DIRS = {"right": (1, 0), "left": (-1, 0), "up": (0, -1), "down": (0, 1)}


def ask(url, model, state, timeout=30):
    body = {
        "model": model,
        "state": state,
        "keep_alive": "10m",
        "questions": {
            "screen": {"type": "choice", "instructions": "What kind of Game Boy screen is this?",
                       "criteria": SCREENS},
            "action": {"type": "choice",
                       "instructions": "Which input moves the game forward: starts it, gets past "
                                       "text and menus, or advances the player to new places?",
                       "criteria": ACTIONS},
        },
    }
    req = urllib.request.Request(url.rstrip("/") + "/v1/systemone", data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)["answers"]


class Screen:
    """Text picture of one VBlank: 20x18 cells, one character each."""

    def __init__(self, io, oam, vram):
        self.io, self.oam, self.vram = io, oam, vram

    def tile_shades(self, index, signed):
        if signed:
            base = 0x1000 + (index - 256 if index >= 128 else index) * 16
        else:
            base = index * 16
        bgp = self.io[0x47]
        out = []
        for row in range(8):
            lo, hi = self.vram[base + row * 2], self.vram[base + row * 2 + 1]
            for bit in range(7, -1, -1):
                c = ((lo >> bit) & 1) | (((hi >> bit) & 1) << 1)
                out.append((bgp >> (c * 2)) & 3)
        return out

    def render(self, player_slots):
        lcdc, scy, scx = self.io[0x40], self.io[0x42], self.io[0x43]
        wy, wx = self.io[0x4A], self.io[0x4B] - 7
        signed = not (lcdc & 0x10)
        cells = []
        for r in range(18):
            for c in range(20):
                y, x = r * 8 + 4, c * 8 + 4
                if lcdc & 0x20 and y >= wy and x >= wx and wx < 160 and wy < 144:
                    m, ty, tx = (0x1C00 if lcdc & 0x40 else 0x1800), (y - wy) // 8, (x - wx) // 8
                    layer = "w"
                elif lcdc & 0x01:
                    m, ty, tx = (0x1C00 if lcdc & 0x08 else 0x1800), ((y + scy) & 255) // 8, ((x + scx) & 255) // 8
                    layer = "b"
                else:
                    cells.append(None)
                    continue
                cells.append(self.tile_shades(self.vram[m + ty * 32 + tx], signed))
        counts = [0, 0, 0, 0]
        for t in cells:
            if t:
                for s in t:
                    counts[s] += 1
        empty = counts.index(max(counts))
        grid = []
        for t in cells:
            if not t:
                grid.append(".")
                continue
            full = sum(1 for s in t if s != empty)
            grid.append("." if full == 0 else "-" if full < 16 else "=" if full < 44 else "#")
        sprites = 0
        if lcdc & 0x02 and lcdc & 0x80:
            tall = 16 if lcdc & 0x04 else 8
            for slot in range(40):
                y, x = self.oam[slot * 4] - 16, self.oam[slot * 4 + 1] - 8
                if not (-tall < y < 144 and -8 < x < 160):
                    continue
                sprites += 1
                mark = "P" if slot in player_slots else "o"
                for dy in range(0, tall, 8):
                    r, c = (y + dy + 4) // 8, (x + 4) // 8
                    if 0 <= r < 18 and 0 <= c < 20 and grid[r * 20 + c] != "P":
                        grid[r * 20 + c] = mark
        rows = ["".join(grid[r * 20:(r + 1) * 20]) for r in range(18)]
        blank = all(ch == "." for row in rows for ch in row)
        return rows, sprites, blank

    def sprite_positions(self):
        return [(self.oam[s * 4 + 1], self.oam[s * 4]) for s in range(40)]


class Player:
    def __init__(self, args):
        self.args = args
        self.rng = random.Random(args.seed)
        self.player_score = [0.0] * 40
        self.prev = None
        self.last_action = None
        self.last_dir = (0, 0)
        self.stale = 0          # decisions without new code
        self.same_screen = 0    # decisions with the same picture
        self.recent = []
        self.steps = []         # (frame, buttons)
        self.calls = 0
        self.cache = {}         # same picture and notes, same answer
        self.model_time = 0.0
        self.decisions = 0

    def player_slots(self):
        best = max(self.player_score)
        if best < 2:
            return set()
        return {i for i, s in enumerate(self.player_score) if s >= best * 0.6}

    def learn(self, screen, scroll):
        """Sprites that move with the d-pad (or a camera that does) are the player."""
        d = self.last_dir
        if not self.prev or d == (0, 0):
            return "", ""
        pos, prev_pos = screen.sprite_positions(), self.prev[0].sprite_positions()
        moved = False
        for s in range(40):
            dx, dy = pos[s][0] - prev_pos[s][0], pos[s][1] - prev_pos[s][1]
            if pos[s][1] == 0 or abs(dx) > 40 or abs(dy) > 40:
                continue
            agree = dx * d[0] + dy * d[1]
            if agree > 0:
                self.player_score[s] += 1
                moved = True
            elif agree < 0:
                self.player_score[s] -= 0.5
            self.player_score[s] *= 0.97
        sdx = ((scroll[0] - self.prev[1][0] + 128) & 255) - 128
        sdy = ((scroll[1] - self.prev[1][1] + 128) & 255) - 128
        cam = ""
        if sdx or sdy:
            cam = "The view scrolled %s." % ", ".join(
                p for p in (("right" if sdx > 0 else "left") if sdx else "",
                            ("down" if sdy > 0 else "up") if sdy else "") if p)
        return ("The player moved." if moved else "The player did not move."), cam

    def decide(self, frame, cov, fhash, screen):
        scroll = (screen.io[0x43], screen.io[0x42])
        moved, cam = self.learn(screen, scroll)
        new_code = cov - self.prev[2] if self.prev else cov
        self.stale = 0 if new_code > 0 else self.stale + 1
        self.same_screen = self.same_screen + 1 if self.prev and fhash == self.prev[3] else 0
        rows, sprites, blank = screen.render(self.player_slots())
        notes = []
        if self.last_action:
            notes.append("Last input: %s." % self.last_action)  # last of the plan
            notes += [n for n in (moved, cam) if n]
        notes.append("New game code ran: %s." % ("yes" if new_code > 0 else "no"))
        if self.same_screen >= 3:
            notes.append("The screen has not changed for several inputs.")
        state = ("Game Boy screen, 20 columns x 18 rows. '.' empty, '#' solid tile, '=' patterned tile, "
                 "'-' thin detail, 'P' player sprite, 'o' other sprite.\n" + "\n".join(rows) + "\n" + " ".join(notes))
        if state not in self.cache:
            t = time.monotonic()
            try:
                answers = ask(self.args.url, self.args.model, state)
                self.cache[state] = (answers["screen"]["choice"], dict(answers["action"]["probabilities"]))
            except (urllib.error.URLError, KeyError, ValueError, TimeoutError) as e:
                print("[ai] model error: %s" % e, file=sys.stderr)
                self.cache[state] = ("gameplay", {a: 1.0 for a in ACTIONS})
            self.model_time += time.monotonic() - t
            self.calls += 1
        kind, probs = self.cache[state][0], dict(self.cache[state][1])
        self.decisions += 1
        # A still picture with nothing that answers the d-pad is a title,
        # menu or text whatever the model says: it needs Start or A.
        if self.same_screen >= 3 and not self.player_slots() and self.stale >= 3:
            kind = "menu"
            for a in ("start", "a"):
                probs[a] = probs.get(a, 0) + 0.5
        # A sprite that follows the d-pad on a moving screen: the game is on,
        # even when the small model takes it for a title screen.
        elif self.player_slots() and self.same_screen < 3:
            kind = "gameplay"
        if kind == "gameplay":
            probs["start"] = probs.get("start", 0) * 0.02  # pause
        # Nothing new for a while: the model keeps asking for the same thing,
        # so mix in the inputs it has not tried lately.
        eps = min(0.85, 0.15 + 0.07 * self.stale)
        for a in ACTIONS:
            p = probs.get(a, 0.0)
            if self.stale > 3 and a not in self.recent[-6:]:
                p *= 2
            probs[a] = (1 - eps) * p + eps / len(ACTIONS)
        # One model call, several inputs drawn from its answer: the model is
        # the slow part, the game runs thousands of frames per second.
        plan = self.rng.choices(list(probs), weights=list(probs.values()), k=self.args.plan)
        self.recent += plan
        self.last_action = plan[-1]
        self.prev = (screen, scroll, cov, fhash)

        steps, dx, dy = [], 0, 0
        for action in plan:
            if blank or kind in ("title", "menu", "dialog", "gameover", "transition"):
                steps += [(action, 6), ("none", 10)]
            elif action in DIRS:
                steps += [(action, 24)]
            elif "+" in action:
                steps += [(action, 16), (action.split("+")[0], 8)]
            elif action == "none":
                steps += [("none", 16)]
            else:
                steps += [(action, 10), ("none", 4)]
            d = DIRS.get(action.split("+")[0], (0, 0))
            dx, dy = dx + d[0], dy + d[1]
        # What the player sprite should have done over the whole plan.
        self.last_dir = (dx > 0) - (dx < 0), (dy > 0) - (dy < 0)
        action = "+".join(plan) if len(set(plan)) > 1 else plan[0]
        at = frame
        for b, n in steps:
            self.steps.append((at, b))
            at += n
        if self.args.verbose:
            print("[ai] frame %u cov %u %s -> %s\n%s" % (frame, cov, kind, action, state), file=sys.stderr)
        return " ".join("%s:%d" % s for s in steps)


def check(args):
    try:
        ask(args.url, args.model, "Game Boy title screen.", timeout=120)
        return 0
    except Exception as e:
        print("[ai] %s : %s" % (args.model, e), file=sys.stderr)
        return 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--harness", help="<name>_profile built from the generated host project")
    ap.add_argument("--out", help="input file written for --input replays")
    ap.add_argument("--seconds", type=float, default=120)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--plan", type=int, default=4, help="inputs played per model call")
    ap.add_argument("--model", default="tev1:0.8b")
    ap.add_argument("--url", default="http://localhost:11434")
    ap.add_argument("--check", action="store_true")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("extra", nargs="*", help="more harness options (after --)")
    args = ap.parse_args()
    if args.check:
        return check(args)
    if not args.harness or not args.out:
        ap.error("--harness and --out are required")

    player = Player(args)
    proc = subprocess.Popen([args.harness, "--interp", "--agent", "--frames", "0",
                             "--max-seconds", str(args.seconds)] + args.extra,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1)
    last_frame = 0
    for line in proc.stdout:
        if line.startswith("agent "):
            _, frame, cov, fhash, io, oam, vram = line.split()
            frame, cov = int(frame), int(cov)
            screen = Screen(bytes.fromhex(io), bytes.fromhex(oam), bytes.fromhex(vram))
            try:
                proc.stdin.write(player.decide(frame, cov, fhash, screen) + "\n")
                proc.stdin.flush()
            except BrokenPipeError:
                break
        else:
            if line.startswith("frames "):
                last_frame = int(line.split()[1])
            sys.stdout.write(line)
    proc.wait()
    with open(args.out, "w") as f:
        f.write("# gbl input recording: <frame> <buttons> (ai_play %s seed %d)\n" % (args.model, args.seed))
        prev = None
        for frame, b in player.steps:
            if frame > last_frame:
                break
            if b != prev:
                f.write("%u %s\n" % (frame, b))
                prev = b
    if player.calls:
        print("[ai] %d decisions, %d model calls, %.0f ms per call, %u frames"
              % (player.decisions, player.calls, 1000 * player.model_time / player.calls, last_frame),
              file=sys.stderr)
    return proc.returncode


if __name__ == "__main__":
    sys.exit(main())
