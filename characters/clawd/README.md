# clawd

The pixel-art Claude Code mascot — an orange square-bodied critter
with two stubby legs and expressive tiny eyes. Clawd appears in the
Claude Desktop code tab and on official stickers; this pack repurposes
that art onto the M5StickC Plus.

Five base poses cover six behavioral states (plus rotated idle moods):

| Source sticker       | Device state(s)       |
| -------------------- | --------------------- |
| holding a floppy disk | `busy`                |
| emitting sparkles     | `attention`           |
| skateboarding         | `celebrate`, idle mix |
| heart overhead        | `heart`, idle base    |
| `>_<` eyes shut       | `dizzy`, `sleep`      |

Animations are generated programmatically from the static stickers —
see `tools/clawd/build.py`. Re-run the script after tweaking any
source PNG to regenerate the pack.

    python3 tools/clawd/build.py

The sticker source art is not covered by this repository's MIT license;
it's official Claude Code promotional art. See the top-level `LICENSE`.

Flash over USB:

    python3 tools/flash_character.py characters/clawd

Or drag the folder onto the Hardware Buddy window over BLE.
