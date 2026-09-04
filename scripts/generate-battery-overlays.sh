#!/bin/sh
set -eu

output=${1:-config/retroarch-overlays/battery}
mkdir -p "$output"

make_panel() {
  source=$1
  panel=$2
  # Descriptor images are stretched to their hitbox. Keep a tightly cropped
  # panel as well as the full 240x240 image used by GameBird Shell previews.
  convert "$source" -crop 68x31+169+8 +repage "$panel"
}

for percent in 100 95 90 85 80 75 70 65 60 55 50 45 40 35 30 25 20 15 10 5 0; do
  fill_end=$((179 + (18 * percent / 100)))
  color='#55d98b'
  outline='#dbe9ff'
  if [ "$percent" -le 20 ]; then
    color='#ef5350'
    outline='#ef5350'
  fi
  convert -size 240x240 xc:none \
    -fill 'rgba(0,0,0,0.78)' -stroke '#55799d' -strokewidth 1 \
    -draw 'roundrectangle 170,8 236,38 4,4' \
    -fill none -stroke "$outline" -draw 'rectangle 176,14 199,26 rectangle 200,18 202,22' \
    -fill "$color" -stroke none -draw "rectangle 178,16 ${fill_end},24" \
    -fill '#ffffff' -font DejaVu-Sans -pointsize 9 -gravity NorthWest \
    -annotate +205+16 "${percent}%" "$output/battery-${percent}.png"
  make_panel "$output/battery-${percent}.png" "$output/panel-${percent}.png"
done

convert -size 240x240 xc:none \
  -fill 'rgba(0,0,0,0.78)' -stroke '#55799d' -strokewidth 1 \
  -draw 'roundrectangle 170,8 236,38 4,4' \
  -fill none -stroke '#8fa6bd' -draw 'rectangle 176,14 199,26 rectangle 200,18 202,22' \
  -fill '#aebdca' -font DejaVu-Sans -pointsize 9 -gravity NorthWest \
  -annotate +205+16 'N/A' "$output/battery-na.png"
make_panel "$output/battery-na.png" "$output/panel-na.png"
