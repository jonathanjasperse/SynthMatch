# Your own background tile

The background is one small image repeated across the window, brighter near
the light in the upper right. Without an image here, the plugin draws its own
metal pentagon. To use yours, put a PNG at `plugin/assets/tile.png` and
rebuild: it is built into the plugin, and also shown on the empty
"Drop a synth recording here" screen.

| File | What it is |
| --- | --- |
| `tile.png` | Your tile. Optional: delete it to go back to the drawn pentagon. |
| `backdrop.json` | Size, spacing, brightness and shadow of the tiles. |
| `blender/tile_scene.py` | Sets up a Blender scene for making `tile.png`. |

## The image

- **Square PNG with transparency**, 512 × 512 is plenty. Anything outside
  the shape should be fully transparent, so the dark background shows through.
- **Lit from the upper left**, like the rest of the interface.
- **Centred, with a little room around it.** Tiles are drawn about 60 to 130
  pixels wide, so fine detail disappears: bold shapes and clear highlights
  read best.
- No shadow baked in. The plugin adds a soft one (`shadow` below).

## Making it in Blender

1. Open Blender, go to the **Scripting** tab, **Open**
   `plugin/assets/blender/tile_scene.py` and press **Run Script** (▶). Or
   start Blender with it from the repository folder (Blender's installer
   doesn't put it on the PATH, so use its full path).

   PowerShell (change 5.2 to your Blender version):

   ```powershell
   & "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe" --python plugin\assets\blender\tile_scene.py
   ```

   macOS Terminal:

   ```bash
   /Applications/Blender.app/Contents/MacOS/Blender --python plugin/assets/blender/tile_scene.py
   ```

   This sets up a square transparent render, a top-down orthographic camera,
   a key light from the upper left, and a starter metal pentagon to reshape or
   delete. Up in the camera view is up in the plugin.

2. Model your tile. Metal looks right with a **Principled BSDF**, Metallic
   1.0, Roughness about 0.2 to 0.4. Bevel the edges (a Bevel modifier, small
   width, 2 or 3 segments) so they catch the light.

3. Press **F12** to render. Every render is saved straight to
   `plugin/assets/tile.png`; there's no need to save the image yourself.

4. Save your `.blend` anywhere you like. When you open it again, run the
   script again to turn the auto-save back on. Running it again is safe: it
   leaves your objects alone.

The script switches the colour management to **Standard** (Blender's default,
AgX, greys out highlights and the tile would look duller in the plugin) and
uses Eevee for quick renders. Cycles works too.

Without the script: Render Properties > Film > **Transparent**; Output
Properties > 512 × 512, PNG, **RGBA**, 8 bit; Color Management > View
Transform **Standard**; an orthographic camera looking straight at the shape;
then Image > Save As `plugin/assets/tile.png` after each render.

## Seeing it in the plugin while you work

Point `SYNTHMATCH_TILE` at your PNG and start the standalone app. It redraws
the background a second after every render, so you can keep Blender and the
plugin side by side.

PowerShell, from the repository folder:

```powershell
$env:SYNTHMATCH_TILE = "$PWD\plugin\assets\tile.png"
& ".\build\plugin\SynthMatch_artefacts\Release\Standalone\SynthMatch.exe"
```

macOS Terminal:

```bash
SYNTHMATCH_TILE="$PWD/plugin/assets/tile.png" "build/plugin/SynthMatch_artefacts/Release/Standalone/SynthMatch.app/Contents/MacOS/SynthMatch"
```

To try settings the same way, copy `backdrop.json` somewhere, point
`SYNTHMATCH_BACKDROP` at the copy, and edit it while the app is open.

## Tuning: backdrop.json

| Key | Default | What it does |
| --- | --- | --- |
| `size` | `0` | Tile width in pixels. `0` scales with the window: about a ninth of its height. |
| `spacing_x` | `1.3` | Distance between tile centres, in tile widths. `1.0` = touching. |
| `spacing_y` | `1.15` | The same, down the rows, in tile heights. |
| `offset_rows` | `true` | Shift every other row by half a tile, like bricks. Use `false` for a square grid. |
| `opacity` | `0.32` | How strongly the tiles far from the light show (0 to 1). |
| `highlight` | `0.45` | How much more strongly the tiles near the light show. |
| `shadow` | `0.6` | Darkness of the soft shadow under each tile. `0` = none. |
| `light_x`, `light_y` | `0.82`, `0.12` | Where the light is, as fractions of the window (0, 0 is the top left). |

`size`, the spacing, `offset_rows` and `shadow` apply to your tile; the drawn
pentagon keeps its own. The rest apply to both.

## Shipping it

Leave `tile.png` (and any `backdrop.json` changes) in this folder and build as
usual. CMake notices files being added or removed here by itself. Commit them
to share the look; they become part of the project, under its licence
(AGPLv3), so only use an image you made or have the rights to.
