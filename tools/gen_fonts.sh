#!/usr/bin/env bash
# Regenerates the committed font C files.
#   tools/gen_fonts.sh <inter-dir> <spleen-dir>
# inter-dir : unpacked https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip
#             (sha256 9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e)
# spleen-dir: unpacked https://github.com/fcambus/spleen/releases/download/2.1.0/spleen-2.1.0.tar.gz
#             (sha256 8b47c56f1a6eb858fbcf9e34530557404b02fbb3455e38e64fb84473fd0c372f)
# Needs python3 with Pillow for the anti-aliased Inter atlases.
set -eu
INTER="$1"; SPLEEN="$2"
OUT=kernel/gfx
TTF="$INTER/extras/ttf"
python3 -I tools/ttf2c.py "$TTF/Inter-Regular.ttf"         13 afont_ui      > $OUT/afont_ui.c
python3 -I tools/ttf2c.py "$TTF/Inter-Medium.ttf"          13 afont_ui_med  > $OUT/afont_ui_med.c
python3 -I tools/ttf2c.py "$TTF/Inter-SemiBold.ttf"        13 afont_ui_bold > $OUT/afont_ui_bold.c
python3 -I tools/ttf2c.py "$TTF/Inter-Regular.ttf"         11 afont_small   > $OUT/afont_small.c
python3 -I tools/ttf2c.py "$TTF/InterDisplay-SemiBold.ttf" 24 afont_title   > $OUT/afont_title.c
for s in 8x16 12x24 16x32; do
    python3 -I tools/bdf2c.py "$SPLEEN/spleen-$s.bdf" font_$s > $OUT/font_$s.c
done
cp "$INTER/LICENSE.txt" assets/fonts/LICENSE.inter
cp "$SPLEEN/LICENSE" assets/fonts/LICENSE.spleen
echo "fonts regenerated"
