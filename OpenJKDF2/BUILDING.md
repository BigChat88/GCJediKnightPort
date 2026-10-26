## Building (GameCube)

This tree builds only the Nintendo GameCube port, with devkitPPC and libogc2.
The other platforms of upstream OpenJKDF2 (desktop, Android, iOS, DSi,
Dreamcast, WebAssembly) have been removed.

With Docker, from this folder:

```
docker build -t openjkdf2-gamecube packaging/gamecube
docker run --rm -v "$PWD:/src" -w /src openjkdf2-gamecube ./build_gamecube.sh
```

The result is `build_gamecube/openjkdf2.dol` and `build_gamecube/apploader.img`.
Without Docker, `build_gamecube.sh` needs devkitPro with the GameCube toolchain
and libogc2 installed, plus `cogapp` for Python.

To turn the engine and your own copy of the game into disc images, see the
`README.md` at the root of the repository (`build.sh` / `build.cmd`). The
`packaging/gamecube/README.md` file covers the port itself.
