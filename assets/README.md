# assets/

## `projectdragoonarm-banner.{jpg,png}`

README hero image, 1376×768 — the same canvas and visual language as
ProjectDural's banner (dark technical backdrop, chrome title with tracking and
glow, letterspaced cyan detail lines, key art blended on the right, hardware
spec strip).

Both files are **generated**, never hand-edited:

```bash
python3 tools/make_banner.py                       # writes both files
python3 tools/make_banner.py --check               # layout metrics only
python3 tools/make_banner.py --title "…" --sub "…" --tag "…" --desc "…"
```

The JPEG is what the README references (346 KiB vs the PNG's 892 KiB). The PNG
is kept because it is lossless and is the better source for re-cropping into
storefront/cover art.

## `source/ref-01.jpg`

The input artwork: the Panzer Dragoon Saga promotional illustration
(Azel and Edge), as supplied for this project from
`anaitgames.com/images/uploads/2008/saga-panzer-dragoon/01.jpg` (580×624).

**Rights:** the illustration is Sega's, part of Panzer Dragoon Saga's
promotional material. It is used here to identify the game this port runs. It is
not covered by this repository's MIT licence, and no game data is redistributed
with it.

If you fork the project for a different game, replace this file and re-run the
generator — nothing else in the banner is game-specific except the strings.
