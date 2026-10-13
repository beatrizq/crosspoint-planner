#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "Utf8.h"

namespace {

// Helpers to build NFD / expected byte sequences explicitly so the test does not
// depend on the encoding of this source file.
const std::string kCombGrave = "\xCC\x80";     // U+0300 COMBINING GRAVE ACCENT
const std::string kCombAcute = "\xCC\x81";     // U+0301 COMBINING ACUTE ACCENT
const std::string kCombCirc = "\xCC\x82";      // U+0302 COMBINING CIRCUMFLEX ACCENT
const std::string kCombDotBelow = "\xCC\xA3";  // U+0323 COMBINING DOT BELOW

}  // namespace

// ASCII and already-precomposed (NFC) text must pass through untouched (fast path).
TEST(Utf8ComposeNfc, PassesThroughAsciiAndNfc) {
  EXPECT_EQ(utf8ComposeNfc(""), "");
  EXPECT_EQ(utf8ComposeNfc("hello world"), "hello world");
  EXPECT_EQ(utf8ComposeNfc("caf\xC3\xA9"), "caf\xC3\xA9");  // é already U+00E9
}

// Single combining mark composes onto its base letter.
TEST(Utf8ComposeNfc, ComposesSingleMark) {
  EXPECT_EQ(utf8ComposeNfc("e" + kCombAcute), "\xC3\xA9");  // e + ́  -> é  (U+00E9)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombGrave), "\xC3\xA0");  // a + ̀  -> à  (U+00E0)
}

// Vietnamese letters carry two stacked marks; composition must accumulate them
// onto the intermediate precomposed character (this is the crux of the feature).
TEST(Utf8ComposeNfc, ComposesStackedVietnameseMarks) {
  // a + circumflex + acute -> ấ (U+1EA5)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombCirc + kCombAcute), "\xE1\xBA\xA5");
  // a + dot-below + circumflex (canonical order) -> ậ (U+1EAD)
  EXPECT_EQ(utf8ComposeNfc("a" + kCombDotBelow + kCombCirc), "\xE1\xBA\xAD");
}

// A combining mark with no composition for its base is left unchanged, and the
// base is preserved.
TEST(Utf8ComposeNfc, LeavesUncomposableMarksIntact) {
  const std::string in = "q" + kCombAcute;  // no precomposed "q with acute"
  EXPECT_EQ(utf8ComposeNfc(in), in);
}

// A leading combining mark (no preceding base) is emitted unchanged.
TEST(Utf8ComposeNfc, HandlesLeadingMark) { EXPECT_EQ(utf8ComposeNfc(kCombAcute), kCombAcute); }

// Marks embedded in a longer word compose while surrounding text is preserved.
TEST(Utf8ComposeNfc, ComposesWithinWord) {
  // "Ti" + e+circ+acute + "ng" -> "Tiếng"
  EXPECT_EQ(utf8ComposeNfc("Ti" + std::string("e") + kCombCirc + kCombAcute + "ng"), "Ti\xE1\xBA\xBFng");
}

// Emoji sequences are reduced to their base emoji: the invisible selectors,
// joiners and skin tones have no glyph in the device font.
namespace {
std::string stripped(const std::string& in) {
  std::string copy = in;
  utf8StripEmojiModifiers(copy.data());
  copy.resize(strlen(copy.c_str()));
  return copy;
}
}  // namespace

TEST(Utf8StripEmojiModifiers, LeavesPlainTextAndBareEmojiAlone) {
  EXPECT_EQ(stripped("hello"), "hello");
  EXPECT_EQ(stripped("caf\xC3\xA9 \xF0\x9F\x98\x80"), "caf\xC3\xA9 \xF0\x9F\x98\x80");  // café 😀
  EXPECT_EQ(stripped(""), "");
}

TEST(Utf8StripEmojiModifiers, DropsVariationSelectors) {
  EXPECT_EQ(stripped("\xE2\x9D\xA4\xEF\xB8\x8F"), "\xE2\x9D\xA4");  // ❤ + VS16 -> ❤
  EXPECT_EQ(stripped("\xE2\x98\xBA\xEF\xB8\x8E"), "\xE2\x98\xBA");  // ☺ + VS15 -> ☺
}

TEST(Utf8StripEmojiModifiers, DropsSkinTones) {
  EXPECT_EQ(stripped("\xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD"), "\xF0\x9F\x91\x8D");  // 👍🏽 -> 👍
  EXPECT_EQ(stripped("\xF0\x9F\x8F\xBB\xF0\x9F\x8F\xBF"), "");                  // bare modifiers 1F3FB, 1F3FF
}

TEST(Utf8StripEmojiModifiers, DropsJoinersLeavingMembersSideBySide) {
  // 👨 ZWJ 👩 ZWJ 👧 -> 👨👩👧
  EXPECT_EQ(stripped("\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"),
            "\xF0\x9F\x91\xA8\xF0\x9F\x91\xA9\xF0\x9F\x91\xA7");
}

TEST(Utf8StripEmojiModifiers, KeycapBecomesItsDigit) {
  EXPECT_EQ(stripped("1\xEF\xB8\x8F\xE2\x83\xA3"), "1");  // 1 + VS16 + keycap -> 1
}

// Other characters in the neighbouring ranges must survive: the arrow block next to
// the keycap's block, and a different 3-byte lead (EF) that is not a selector.
TEST(Utf8StripEmojiModifiers, DoesNotTouchLookalikeBytes) {
  EXPECT_EQ(stripped("\xE2\x80\x93"), "\xE2\x80\x93");          // en dash U+2013
  EXPECT_EQ(stripped("\xEF\xB8\x90"), "\xEF\xB8\x90");          // U+FE10, not a selector
  EXPECT_EQ(stripped("\xF0\x9F\x8F\xBA"), "\xF0\x9F\x8F\xBA");  // U+1F3FA (trumpet-ish), below the modifier range
}
