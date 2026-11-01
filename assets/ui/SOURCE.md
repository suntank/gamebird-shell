# UI background

`systemback.png` is copied unchanged from the user-supplied Pixel TFT theme's
`art/systemback.png`. GameBird Shell scales it to fill its 240×240 menus, with
white labels and blue controls. See [the original theme credits and artwork
license](../system-art/SOURCE.md) (attribution, share alike, noncommercial use).

## Game Bird boot splash

`splash.png` is the original, unchanged artwork supplied by the Game Bird
project owner for the boot splash. It is separate from the Pixel TFT theme
wallpaper above; the theme's attribution and licence do not describe this
image.

`../boot/splash.rgb565` is generated from that PNG by
`tools/prepare-boot-splash.py`: the complete image is fitted to 240×240 with
nearest-neighbour sampling and encoded as row-major, little-endian RGB565.
The raw asset is committed so the console does not need Pillow or a PNG decoder
to display it during early boot. Regenerate it from the repository root with:

```sh
python3 tools/prepare-boot-splash.py assets/ui/splash.png assets/boot/splash.rgb565
```

Pillow is a developer-only requirement for that command. Add `--check` to
verify the committed asset without changing it.
