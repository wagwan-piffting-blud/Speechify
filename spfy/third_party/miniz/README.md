# miniz 3.0.2 (vendored)

Upstream: <https://github.com/richgel999/miniz> -- MIT.

Two files, taken from the official 3.0.2 amalgamation release unmodified.
Compatible with this project's GPL-3.0-or-later.

## Why it is here

`spfy_synth --install-voice` downloads a voice from the repository's `voices`
release, and those assets are `ZIP_DEFLATED` archives. Nothing else in this
tree can inflate a DEFLATE stream - the only compression the engine itself
knows is the VDB's mu-law and the VIN's own packing, neither of which is
DEFLATE.

miniz was chosen over the alternatives because it answers **both** halves of
the problem in one dependency: `mz_zip_reader_*` reads the zip container, so
there is no hand-written central-directory parser to get subtly wrong, and
`tinfl` does the inflating. The glue in `spfy/src/update/upd_voice.c` is about
fifty lines as a result.

The rejected options, recorded so nobody re-opens this by accident:

- **zlib as a system dependency.** Fine on unix, awkward for the mingw32
  Windows build, and it would add a runtime dependency to a binary that
  currently has none beyond msvcrt.
- **`puff.c`**, zlib's own reference decoder. Public domain and tiny, but it is
  written for clarity and is roughly two orders of magnitude slower than a real
  inflate. On a 92 MB voice that is minutes of apparent hang.
- **Publishing raw, unzipped files as release assets.** Zero decompression code
  forever, but it roughly doubles release storage and makes every download
  ~28% bigger (crsmara is 92 MB zipped against 128 MB raw).
- **Republishing the zips as STORED.** No inflate needed, but the same ~28%
  growth, and it changes assets the desktop installer and the Android app
  already consume.

## Updating it

Replace both files from a newer upstream amalgamation and rebuild. Nothing in
this tree patches miniz, and nothing should: keeping it pristine is what makes
the next update a copy rather than a merge.

NOTE: Only `MINIZ_NO_STDIO`-free, in-memory and file APIs are used. If a future
change needs miniz's writer, note that `upd_voice.c` deliberately does not
compress anything - the engine never produces a zip.
