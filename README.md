# Mortal Kombat 11 on Mac (CrossOver): broken faces fix

Unofficial workaround for **Mortal Kombat 11** rendering garbled, stretched or broken character
faces when run through CrossOver on Apple silicon. Bodies, stages and menus look fine; only faces
are wrong.

## Status

Works on the setup it was built on. Please report results for other setups in the issues.

| | Tested |
|---|---|
| Mac | M4 Pro, macOS 27.2 |
| CrossOver | CrossOver Preview 27.0 |
| Graphics backend | DXMT |
| Game executable | `MK11.exe` (DirectX 11) |

## What is wrong

MK11 reads per-vertex face data from typed buffers in `DXGI_FORMAT_R32G32B32_FLOAT` format, three
32-bit floats (12 bytes) per element. Metal has no 3-component 32-bit texel buffer format, and
every graphics backend in CrossOver mishandles it in a different way:

| Backend | What happens |
|---|---|
| DXVK | Refuses to create the view (`D3D11: Cannot create shader resource view`, format 6). Faces get no data. |
| DXMT | Silently treats the buffer as `R32_FLOAT`. Element *i* reads one float at offset `4i` instead of three at `12i`. |
| D3DMetal | Logs `R32G32B32 typed buffers not supported, using R32G32 instead`. Element *i* reads two floats at offset `8i`. |

In all three cases the face shaders get wrong data and the faces come out garbled.

## What this does

A small DLL, installed next to `MK11.exe` as `xinput9_1_0.dll`, which the game already loads.
Its XInput exports are forwarded to Wine's `xinput1_3`, so controllers keep working.

When the game creates its Direct3D 11 device, the DLL hooks a few methods of the device and its
contexts. Whenever the game creates an `R32G32B32` buffer view, the DLL:

1. creates a shadow buffer in which every 12-byte element is padded to 16 bytes (`w = 1`, which is
   what a 3-component load returns anyway),
2. gives the game an `R32G32B32A32` view of the shadow instead. Every backend supports that format,
   and the shaders only read `.xyz`, so they get exactly the values they expect,
3. mirrors later writes to the original buffer (`UpdateSubresource`, `Map`/`Unmap`, copies) into
   the shadow.

No game files are modified, and CrossOver itself is not touched, so CrossOver updates do not
remove the fix. In testing, MK11 created about 4,700 of these buffers at load time, all filled once
and never updated; the shadows use about 190 MB of GPU memory.

## Install

1. Download `mk11-crossover-face-fix-<version>.zip` from the
   [Releases](../../releases) page and unzip it.
2. Quit Mortal Kombat 11 and CrossOver.
3. Run the installer with your bottle's name, or the full path to the bottle if it is not in the
   default location:

   ```sh
   cd mk11-crossover-face-fix-<version>
   ./install.sh "Mortal Kombat 11"
   # or: ./install.sh "/Volumes/External/Bottles/Mortal Kombat 11"
   ```

4. In CrossOver, open the bottle settings and set **Graphics Backend** to **DXMT**.
5. Launch **`MK11.exe`**, not `MK11_DX12.exe`. The fix hooks Direct3D 11 only.

If macOS blocks the scripts because they were downloaded, run `xattr -dr com.apple.quarantine .`
in the unzipped folder first.

The installer copies the DLL into `Binaries/Retail` and adds a Wine DLL override
(`xinput9_1_0 = native,builtin`) for `MK11.exe` only. It backs up the bottle's `user.reg` as
`user.reg.bak-mk11-face-fix` first.

### Checking that it works

The DLL writes `Binaries/Retail/mk11_rgb32fix.log`. After reaching a fight or character select
it should contain lines like:

```
hooked D3D11CreateDevice import
D3D11CreateDevice(flags 0) -> 0
stats: shadows 4663 (190982 KB), redirected views 4663, ... untracked copies 0, gpu-writable 0
```

`WARNING` lines mean the game wrote to a buffer in a way the fix does not mirror; please attach
the log to an issue if you see them together with face glitches.

## Uninstall

```sh
./uninstall.sh "Mortal Kombat 11"
```

Deleting `Binaries/Retail/xinput9_1_0.dll` by hand is also enough. Without the file, Wine falls
back to its builtin XInput.

## Build from source

Requires [mingw-w64](https://www.mingw-w64.org/) (`brew install mingw-w64`).

```sh
make          # build/xinput9_1_0.dll
make dist     # dist/mk11-crossover-face-fix-<version>.zip for a release
```

`scripts/install.sh` picks up `build/xinput9_1_0.dll` when run from a source checkout.

## Things to know

- **Online play:** the DLL hooks the game's rendering. It does not touch game logic or memory, but
  use online modes at your own risk.
- **Game updates** could change how faces are drawn. If faces break again, check the log.
- **The real fix belongs in the backends.** DXMT and D3DMetal could emulate `R32G32B32` typed
  buffers themselves; once they do, this workaround is no longer needed.
- **Use at your own risk.** Not affiliated with NetherRealm Studios, Warner Bros. Games,
  CodeWeavers or Apple.

## License

MIT. See [LICENSE](LICENSE).
