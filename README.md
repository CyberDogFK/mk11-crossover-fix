# Mortal Kombat 11 on Mac (CrossOver): faces and cutscenes fix

Unofficial workaround for two problems with **Mortal Kombat 11** under CrossOver on Apple silicon:

- **Broken faces.** Character faces render garbled, stretched or broken. Bodies, stages and menus
  look fine.
- **Broken cutscenes.** Cutscene video is decoded in the wrong format and plays choppy and jerky
  while the audio is fine. Above 1080p, story cutscenes are skipped entirely.

One small DLL fixes the faces and the cutscene video. Skipped cutscenes above 1080p need a
one-line folder link instead; see [Cutscenes above 1080p](#cutscenes-above-1080p-movies_4k).

## Status

Works on the setup it was built on. Please report results for other setups in the issues.

| | Tested |
|---|---|
| Mac | M4 Pro, macOS 27.2 |
| CrossOver | CrossOver Preview 27.0 |
| Graphics backend | DXMT |
| Game executable | `MK11.exe` (DirectX 11) |
| Resolution | 2560x1600, with the `Movies_4k` link |

## Faces

### What is wrong

MK11 reads per-vertex face data from typed buffers in `DXGI_FORMAT_R32G32B32_FLOAT` format, three
32-bit floats (12 bytes) per element. Metal has no 3-component 32-bit texel buffer format, and
every graphics backend in CrossOver mishandles it in a different way:

| Backend | What happens |
|---|---|
| DXVK | Refuses to create the view (`D3D11: Cannot create shader resource view`, format 6). Faces get no data. |
| DXMT | Silently treats the buffer as `R32_FLOAT`. Element *i* reads one float at offset `4i` instead of three at `12i`. |
| D3DMetal | Logs `R32G32B32 typed buffers not supported, using R32G32 instead`. Element *i* reads two floats at offset `8i`. |

In all three cases the face shaders get wrong data and the faces come out garbled.

### What the fix does

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

## Cutscenes

MK11's cutscenes are H.264 videos in `Movies/*.scx`. The game demuxes them itself and decodes
them with the Windows Media Foundation H.264 decoder (`CLSID_CMSH264DecoderMFT`), which CrossOver
implements on top of GStreamer. Three separate things go wrong.

### 1. The decoder hides NV12 (wrong output format)

MK11 asks the decoder for its first available output format and renders it as NV12; that is the
only format its movie shader can draw. CrossOver's decoder deliberately leaves NV12 out of that
list on macOS (Wine log: `Skipping NV12 output format`), so the game gets YV12 frames and
draws them with its NV12 shader. The two formats lay out the color planes differently.

The decoder still produces correct NV12 when asked. The DLL hooks the game's `CoCreateInstance`
import, and for the H.264 decoder it puts NV12 back at the top of the output format list. If a
future CrossOver version lists NV12 itself, the hook steps aside.

### 2. Frames come out in decode order (choppy video)

H.264 stores B-frames out of display order, and a decoder has to reorder them. MK11 sends every
frame with timestamp 0, because the Windows decoder reorders by the frames' own picture order
count (POC). CrossOver's decoder needs timestamps to reorder, so it returns frames in decode order
(0, 3, 1, 2, 7, 5, 4, 6, ...). The frame rate is a steady 30 fps, but the picture jumps back and
forth every few frames.

The DLL reads each frame's POC from its H.264 slice header as it goes into the decoder. It holds
back the last three decoded frames and hands the game whichever comes first in display order, as
the Windows decoder does. This adds about 0.1 s of delay before the first frame. If a video uses
H.264 features the parser does not handle (field coding, POC types 1 and 2, scaling matrices),
the DLL logs it and leaves those frames as decoded.

### Cutscenes above 1080p (`Movies_4k`)

This one is not a CrossOver bug and the DLL does not fix it. When the game runs above 1080p
(seen at 2560x1600), MK11 loads story cutscenes from a `Movies_4k` folder instead of `Movies`.
`Movies_4k` holds the optional 4K cutscenes, which many installs do not have. If the folder is
missing, the game does not fall back to `Movies`; it skips the story cutscenes. A diagnostic
build that logged file access showed the game opening `..\Movies_4k\MK11_001_003.scx`, getting
"path not found", and moving straight on to the fight.

Fix: make `Movies_4k` point at `Movies`, so the game plays the 1080p versions. In Terminal, in
the game folder (the one that contains `Binaries`, `Movies` and `X64TOC.txt`):

```sh
cd "<bottle>/drive_c/Program Files (x86)/Mortal Kombat 11"   # adjust to your install
[ -e Movies_4k ] || ln -s Movies Movies_4k
```

This only adds a link; nothing is copied or overwritten. Delete `Movies_4k` to undo it. If you
own the real 4K cutscenes, keep your `Movies_4k` folder as it is. Running the game at 1920x1080
or lower should also avoid the problem without the link, but that has not been tested.

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

6. If you play above 1080p, add the `Movies_4k` link described in
   [Cutscenes above 1080p](#cutscenes-above-1080p-movies_4k).

### Manual install

If you prefer not to run the script, you can do the same steps by hand. Copying the DLL is not
enough on its own: Wine loads its builtin XInput unless the bottle is told to prefer the native
DLL.

1. Quit Mortal Kombat 11.
2. Copy `xinput9_1_0.dll` from the zip into the game's `Binaries/Retail` folder, next to
   `MK11.exe`. In a default install that is
   `<bottle>/drive_c/Program Files (x86)/Mortal Kombat 11/Binaries/Retail`. In CrossOver,
   **Open C: Drive** in the bottle's menu gets you to `drive_c`.
3. Add the DLL override. In CrossOver, select the bottle and open **Wine Configuration**:
   1. On the **Applications** tab, click **Add application...** and choose `MK11.exe` in
      `Binaries/Retail`. Keep `MK11.exe` selected.
   2. On the **Libraries** tab, type `xinput9_1_0` in **New override for library** and click
      **Add**.
   3. Check that the entry reads `xinput9_1_0 (native, builtin)`. If it does not, select it,
      click **Edit...** and choose **Native then Builtin**.
   4. Click **OK**.

   Selecting `MK11.exe` first limits the override to the game. If you add it under **Default
   Settings** instead, it applies to every program in the bottle. That works too, but there is no
   reason to do it.
4. Set the bottle's **Graphics Backend** to **DXMT** and launch **`MK11.exe`**, as in steps 4 and
   5 above.
5. If you play above 1080p, add the `Movies_4k` link.

To undo a manual install, delete `Binaries/Retail/xinput9_1_0.dll`. You can also remove the
override in Wine Configuration, but without the DLL it has no effect.

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

After a cutscene has played, the log should also contain:

```
hooked CoCreateInstance import
H.264 decoder does not offer NV12, adding it as type 0
movie: <frames> frames, <count> returned in a different order than decoded
```

A `movie: cannot read frame order` line means a video uses H.264 features the reordering does
not handle; that video plays as before.

## Uninstall

```sh
./uninstall.sh "Mortal Kombat 11"
```

Deleting `Binaries/Retail/xinput9_1_0.dll` by hand is also enough. Without the file, Wine falls
back to its builtin XInput. The uninstaller does not remove a `Movies_4k` link; delete it by hand
if you added one.

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
- **The real fixes belong upstream.** DXMT and D3DMetal could emulate `R32G32B32` typed
  buffers themselves. CrossOver's H.264 decoder could offer NV12 on macOS and reorder frames by
  POC when samples have no timestamps. Once they do, this workaround is no longer needed.
- **Use at your own risk.** Not affiliated with NetherRealm Studios, Warner Bros. Games,
  CodeWeavers or Apple.

## License

MIT. See [LICENSE](LICENSE).
