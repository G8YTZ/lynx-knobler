# Rebuilding the Browser-Install Binary

This is the reference for regenerating `knobler_merged.bin` — the single file the browser installer (`install.html` + `manifest.json`) flashes to the Dial. You'll need to redo this and republish whenever the firmware changes.

This is a maintainer task, not something end users need — punters just use the browser installer itself.

## Method A: Let Arduino IDE Do It (Recommended)

Newer ESP32 Arduino cores (3.x, which is what current M5Stack board packages pull in) automatically produce an already-merged binary as part of a normal export — no `esptool` install or manual merge command needed at all.

1. Open the sketch in Arduino IDE, with the board/port/Flash Size/Partition Scheme all set as normal (see `INSTALL.md` Option B, Step 6, if you need a refresher on those settings).
2. **Sketch → Export Compiled Binary.**
3. **Sketch → Show Sketch Folder**, then open `build/`, then the folder named after the board variant (e.g. `m5stack.esp32.m5stack_dial/`).
4. Look for a file named **`knobler.ino.merged.bin`**. If it's there, that's the file you want — skip straight to [Step 4: Publish It](#step-4-publish-it) below.

**About the file size:** this auto-generated version pads out to the *full declared flash size* (8MB, if that's what your Flash Size setting is) rather than trimming to just the used portion — so it'll be a fixed 8,388,608 bytes regardless of how big the actual firmware is. That's expected, not a bug; it also correctly includes `boot_app0.bin` (the OTA boot-selector data) at the right offset, which the manual method below doesn't handle. It'll take a bit longer to flash over Web Serial than a trimmed file would (still well under a couple of minutes), but it's the more complete, more correct image.

If `knobler.ino.merged.bin` **isn't** present in that folder (older Arduino IDE / core version), fall back to Method B.

## Method B: Manual Merge with esptool (Fallback)

Only needed if your Arduino IDE/core version doesn't auto-generate the merged file (see Method A above).

### Step 1: Export the Compiled Binary

Same as Method A, steps 1–3. You should see three files that matter here, instead of one merged file:
- `knobler.ino.bootloader.bin`
- `knobler.ino.partitions.bin`
- `knobler.ino.bin` (the actual app — the largest of the three, roughly 1MB+)

### Step 2: Install esptool (one-time setup)

**Recommended: standalone binary, no Python required.**

1. Go to <https://github.com/espressif/esptool/releases/latest>.
2. Download the asset matching your OS and CPU — e.g. `esptool-vX.Y.Z-macos-arm64.tar.gz` for Apple Silicon Macs, `-amd64` for Intel Macs, or the Windows/Linux equivalent.
3. Unpack it — you get a folder containing an `esptool` executable (or `esptool.exe` on Windows).
4. **macOS only:** right-click the `esptool` file → **Open** → confirm the Gatekeeper warning (needed once; a plain double-click will silently refuse to run it).
5. Confirm it works — in Terminal, `cd` into that folder and run:
   ```
   ./esptool version
   ```
   Should print a version number cleanly.

If you'd rather use the Python-based install instead (`pip install esptool`), that works too, but make sure whatever `python3`/`pip3` you're using is a reasonably current version (3.9+) — an old system Python can cause confusing "module not found" errors that have nothing to do with esptool itself.

### Step 3: Merge the Three Files

The M5Dial is **ESP32-S3** — this matters, because the bootloader offset differs from the classic ESP32 (`0x0` here, not `0x1000`).

From Terminal, with your working directory set to the `build/<board>/` folder from Step 1:

```
/path/to/esptool --chip esp32s3 merge-bin -o knobler_merged.bin \
  --flash-mode dio --flash-freq 80m --flash-size 8MB \
  0x0 knobler.ino.bootloader.bin \
  0x8000 knobler.ino.partitions.bin \
  0x10000 knobler.ino.bin
```

Replace `/path/to/esptool` with wherever you unpacked it in Step 2.

**Sanity-check the result** before moving on — a valid merged image should start with byte `0xE9` (the standard ESP32 image magic byte). Quick check with Python:
```
python3 -c "
with open('knobler_merged.bin', 'rb') as f:
    b = f.read(1)
print('First byte:', hex(b[0]), '- should be 0xe9')
"
```

Note this method produces a smaller, trimmed file (not padded to full flash size) and doesn't include `boot_app0.bin` — it's a reasonable fallback but Method A above is the more complete image if it's available to you.

## Step 4: Publish It

1. Copy your merged binary (`knobler.ino.merged.bin` from Method A, or `knobler_merged.bin` from Method B) into your local `lynx-knobler` repo folder, **replacing the existing `knobler_merged.bin`** at the repo root (same level as `manifest.json` and `install.html`) — rename it to `knobler_merged.bin` if it came from Method A.

2. **Bump the version** in `manifest.json` so the install page reflects the new build:
   ```json
   "version": "v32"
   ```
   (or whatever's next — this is just a label shown in the installer UI, not enforced against anything, but worth keeping honest.)

3. Commit and push:
   ```
   git add knobler_merged.bin manifest.json
   git commit -m "Update browser-install binary"
   git push
   ```

   > **Known gotcha:** `.gitignore` has a `*.bin` rule for build artifacts, with a specific `!knobler_merged.bin` exception carved out for this file. If `git status` doesn't show `knobler_merged.bin` as staged after `git add`, check that exception line is still intact in `.gitignore` — it's the kind of thing that's easy to accidentally lose if the file gets edited by hand later.

4. Give GitHub Pages a minute or two to redeploy after the push.

5. **Test it** — visit the install page in an incognito/private Chrome window (to rule out any cached old version) and confirm it flashes cleanly.
