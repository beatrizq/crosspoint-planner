#!/bin/bash

set -e

cd "$(dirname "$0")"

READER_FONT_STYLES=("Regular" "Italic" "Bold" "BoldItalic")
NOTOSERIF_FONT_SIZES=(12 14 16 18)
NOTOSANS_FONT_SIZES=(12 14 16 18)

for size in ${NOTOSERIF_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notoserif_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSerif/NotoSerif-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

for size in ${NOTOSANS_FONT_SIZES[@]}; do
  for style in ${READER_FONT_STYLES[@]}; do
    font_name="notosans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/NotoSans/NotoSans-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

# Arabic glyphs for UI text (menus, file browser titles). The built-in fonts
# must cover the *output* of MiniBidi's do_shape() — contextual presentation
# forms — not base letters, or shaped UI text silently drops glyphs.
# Curated for firmware-size budget: core Arabic (Presentation Forms-B,
# incl. the Lam-Alef ligature forms) plus the Farsi/Urdu extra letters'
# Presentation Forms-A blocks, the few characters shaping leaves at their
# base codepoint, Arabic punctuation, and both digit sets. No harakat and
# no Sindhi/Pashto/Kurdish forms — book text gets those from SD-card fonts.
ARABIC_INTERVALS=(
  --additional-intervals 0x060C,0x060C  # Arabic comma
  --additional-intervals 0x061B,0x061B  # Arabic semicolon
  --additional-intervals 0x061F,0x061F  # Arabic question mark
  --additional-intervals 0x0621,0x0621  # hamza (non-joining, never shaped)
  --additional-intervals 0x0640,0x0640  # tatweel
  --additional-intervals 0x0660,0x0669  # Arabic-Indic digits
  --additional-intervals 0x06BA,0x06BA  # noon ghunna base (initial/medial keep base cp)
  --additional-intervals 0x06D4,0x06D4  # Urdu full stop
  --additional-intervals 0x06F0,0x06F9  # extended Arabic-Indic digits (Farsi/Urdu)
  --additional-intervals 0xFB56,0xFB59  # peh (Farsi)
  --additional-intervals 0xFB66,0xFB69  # tteh (Urdu)
  --additional-intervals 0xFB7A,0xFB7D  # tcheh (Farsi)
  --additional-intervals 0xFB88,0xFB95  # ddal, jeh, rreh (Urdu), keheh, gaf (Farsi/Urdu)
  --additional-intervals 0xFB9E,0xFB9F  # noon ghunna isolated/final (Urdu)
  --additional-intervals 0xFBA6,0xFBB1  # heh goal, heh doachashmee, yeh barree(+hamza) (Urdu)
  --additional-intervals 0xFBFC,0xFBFF  # farsi yeh (Farsi/Urdu)
  --additional-intervals 0xFE80,0xFEFC  # Presentation Forms-B: core Arabic + Lam-Alef
)

# Emoji for notification text (the alerts shown on the Planner screen and the
# Alerts list/detail). The glyphs come from Noto Emoji (monochrome, SIL OFL --
# see builtinFonts/source/NotoEmoji/) and are appended LAST to the font stack, so
# every glyph a UI font already has is unchanged. A curated set of the commonly
# used emoji, not all of them (a few hundred, ~30-40 KB of flash per font).
# Only the Regular UI fonts get them: alert text is never drawn bold, and the
# bold family does not fall back to the regular glyphs. Variation selectors, ZWJ
# joiners and skin-tone modifiers are dropped when a notification is stored
# (BleNotificationQueue), so a sequence renders as its base emoji.
EMOJI_INTERVALS=(
  --additional-intervals 0x2600,0x2603  # sun, cloud, umbrella, snowman
  --additional-intervals 0x2614,0x2615  # umbrella with rain, hot beverage
  --additional-intervals 0x2639,0x263A  # frowning / smiling face
  --additional-intervals 0x2660,0x2667  # card suits
  --additional-intervals 0x267B,0x267B  # recycling
  --additional-intervals 0x2693,0x2693  # anchor
  --additional-intervals 0x26A0,0x26A1  # warning, high voltage
  --additional-intervals 0x26BD,0x26BE  # soccer, baseball
  --additional-intervals 0x26C4,0x26C5  # snowman, sun behind cloud
  --additional-intervals 0x26D4,0x26D4  # no entry
  --additional-intervals 0x2705,0x2705  # check mark button
  --additional-intervals 0x2708,0x270D  # airplane, envelope, fist/hand/victory/writing hand
  --additional-intervals 0x270F,0x270F  # pencil
  --additional-intervals 0x2714,0x2714  # check mark
  --additional-intervals 0x2716,0x2716  # multiply
  --additional-intervals 0x2728,0x2728  # sparkles
  --additional-intervals 0x274C,0x274C  # cross mark
  --additional-intervals 0x274E,0x274E  # cross mark button
  --additional-intervals 0x2753,0x2755  # question / exclamation marks
  --additional-intervals 0x2757,0x2757  # exclamation mark
  --additional-intervals 0x2763,0x2764  # heart exclamation, red heart
  --additional-intervals 0x2795,0x2797  # plus, minus, divide
  --additional-intervals 0x27A1,0x27A1  # right arrow
  --additional-intervals 0x2B05,0x2B07  # left / up / down arrows
  --additional-intervals 0x2B50,0x2B50  # star
  --additional-intervals 0x2B55,0x2B55  # hollow red circle
  --additional-intervals 0x1F308,0x1F308  # rainbow
  --additional-intervals 0x1F319,0x1F31F  # moon faces, glowing star
  --additional-intervals 0x1F337,0x1F33C  # flowers
  --additional-intervals 0x1F345,0x1F37F  # food and drink
  --additional-intervals 0x1F380,0x1F393  # party and celebration
  --additional-intervals 0x1F3A4,0x1F3B8  # music and entertainment
  --additional-intervals 0x1F3C0,0x1F3C6  # sports
  --additional-intervals 0x1F3E0,0x1F3E0  # house
  --additional-intervals 0x1F40D,0x1F43E  # animals
  --additional-intervals 0x1F440,0x1F450  # eyes, hands and gestures
  --additional-intervals 0x1F466,0x1F469  # people
  --additional-intervals 0x1F47B,0x1F480  # ghost, alien, skull
  --additional-intervals 0x1F483,0x1F483  # dancer
  --additional-intervals 0x1F48B,0x1F48B  # kiss mark
  --additional-intervals 0x1F493,0x1F49F  # hearts
  --additional-intervals 0x1F4A1,0x1F4A1  # light bulb
  --additional-intervals 0x1F4A3,0x1F4AF  # bomb, sleep, sweat, poop, flexed biceps, 100
  --additional-intervals 0x1F4DD,0x1F4DE  # memo, telephone
  --additional-intervals 0x1F4E7,0x1F4E7  # e-mail
  --additional-intervals 0x1F4F1,0x1F4F1  # mobile phone
  --additional-intervals 0x1F4F7,0x1F4F7  # camera
  --additional-intervals 0x1F511,0x1F512  # key, lock
  --additional-intervals 0x1F514,0x1F514  # bell
  --additional-intervals 0x1F525,0x1F525  # fire
  --additional-intervals 0x1F600,0x1F64F  # emoticons (faces and gestures, incl. folded hands)
  --additional-intervals 0x1F680,0x1F680  # rocket
  --additional-intervals 0x1F68C,0x1F68C  # bus
  --additional-intervals 0x1F695,0x1F697  # taxi, car
  --additional-intervals 0x1F6B2,0x1F6B2  # bicycle
  --additional-intervals 0x1F90D,0x1F92F  # white/brown heart, more faces and hands
  --additional-intervals 0x1F970,0x1F97A  # smiling face with hearts ... pleading face
  --additional-intervals 0x1F9E1,0x1F9E1  # orange heart
)

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="ubuntu_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Ubuntu/Ubuntu-${style}.ttf"
    hebrew_path="../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-${style}.ttf"
    arabic_path="../builtinFonts/source/NotoSansArabic/NotoSansArabic-${style}.ttf"
    # Ubuntu lacks the Latin Extended Additional block (U+1EA0-U+1EF9) used for
    # Vietnamese tone marks. Append a Vietnamese-only Ubuntu cut so those glyphs
    # are filled from it while every glyph Ubuntu already has stays unchanged
    # (fontstack is ordered by descending priority).
    viet_path="../builtinFonts/source/Ubuntu/Ubuntu-Vietnamese-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    emoji_stack=()
    emoji_args=()
    if [ "$style" = "Regular" ]; then
      emoji_stack=("../builtinFonts/source/NotoEmoji/NotoEmoji-Regular.ttf")
      emoji_args=("${EMOJI_INTERVALS[@]}")
    fi
    python fontconvert.py $font_name $size $font_path $hebrew_path $arabic_path $viet_path "${emoji_stack[@]}" \
      --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" "${emoji_args[@]}" > $output_path
    echo "Generated $output_path"
  done
done

python fontconvert.py notosans_8_regular 8 \
  ../builtinFonts/source/NotoSans/NotoSans-Regular.ttf \
  ../builtinFonts/source/NotoSansHebrew/NotoSansHebrew-Regular.ttf \
  ../builtinFonts/source/NotoSansArabic/NotoSansArabic-Regular.ttf \
  --additional-intervals 0x05D0,0x05EA "${ARABIC_INTERVALS[@]}" > ../builtinFonts/notosans_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/
