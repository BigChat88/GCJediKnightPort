# gamedata/

Put your own CD images of the games here. Nothing in this folder is committed.

* **Jedi Knight: Dark Forces II** -- both discs.
* **Mysteries of the Sith** -- its one disc (optional).

One folder per disc, holding the data track and its `.cue`, as ripped "one file
per track" (Redump style):

```
gamedata/
  Jedi Knight Disc 1/   Track 01.bin, Track 02.bin, ..., Jedi Knight Disc 1.cue
  Jedi Knight Disc 2/   Track 01.bin, ..., Jedi Knight Disc 2.cue
  Mysteries of the Sith/ Track 01.bin, ..., Mysteries of the Sith.cue
```

Folder and file names don't matter: `tools/pack_iso.py` recognizes each game by
the files on the disc, and takes the Jedi Knight discs in name order (disc 1
first). The audio tracks are the soundtrack; a plain 2048-byte `.iso` of the data
track works too, without music. Extract `.zip`/`.rar`/`.7z` archives first.
