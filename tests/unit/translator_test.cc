// Unit tests for the i18n message-catalog Translator (pure logic).

#include "xkit/translator.h"
#include <gtest/gtest.h>
#include <fstream>
#include <cstdio>

using fbx::xkit::Translator;

// ── languageOf ────────────────────────────────────────────────────────────────

TEST(TranslatorTest, LanguageOfExtractsSubtag) {
    EXPECT_EQ(Translator::languageOf("fr_FR.UTF-8"), "fr");
    EXPECT_EQ(Translator::languageOf("de_DE"), "de");
    EXPECT_EQ(Translator::languageOf("ja"), "ja");
    EXPECT_EQ(Translator::languageOf("en_US.UTF-8@euro"), "en");
    EXPECT_EQ(Translator::languageOf(""), "");
}

// ── parseCatalog ──────────────────────────────────────────────────────────────

TEST(TranslatorTest, ParsesKeyValues) {
    auto m = Translator::parseCatalog("greeting=Bonjour\nbye=Au revoir\n");
    EXPECT_EQ(m.at("greeting"), "Bonjour");
    EXPECT_EQ(m.at("bye"), "Au revoir");
}

TEST(TranslatorTest, IgnoresBlanksAndComments) {
    auto m = Translator::parseCatalog("# a comment\n\n  \nkey=val\n# another\n");
    EXPECT_EQ(m.size(), 1u);
    EXPECT_EQ(m.at("key"), "val");
}

TEST(TranslatorTest, TrimsAndAllowsEqualsInValue) {
    auto m = Translator::parseCatalog("  spaced  =  a = b  \n");
    EXPECT_EQ(m.at("spaced"), "a = b");   // key/value trimmed; '=' kept in value
}

TEST(TranslatorTest, UnescapesNewline) {
    auto m = Translator::parseCatalog("multi=line1\\nline2\n");
    EXPECT_EQ(m.at("multi"), "line1\nline2");
}

TEST(TranslatorTest, LastLineWithoutNewline) {
    auto m = Translator::parseCatalog("a=1\nb=2");
    EXPECT_EQ(m.at("b"), "2");
}

// ── tr() lookup + fallback ────────────────────────────────────────────────────

TEST(TranslatorTest, TrReturnsTranslationWhenPresent) {
    Translator t;
    t.loadFromText("save=Enregistrer\n");
    EXPECT_EQ(t.tr("save", "Save"), "Enregistrer");
}

TEST(TranslatorTest, TrFallsBackToProvidedText) {
    Translator t;
    t.loadFromText("save=Enregistrer\n");
    EXPECT_EQ(t.tr("open", "Open"), "Open");    // key absent → fallback
}

TEST(TranslatorTest, TrFallsBackToKeyWhenNoFallback) {
    Translator t;
    EXPECT_EQ(t.tr("missing.key"), "missing.key");
}

// ── setLocale ─────────────────────────────────────────────────────────────────

TEST(TranslatorTest, EnglishLocaleUsesFallbacks) {
    Translator t;
    t.loadFromText("save=SHOULD_NOT_SURVIVE\n");
    t.setLocale("en_US.UTF-8");                 // clears catalog
    EXPECT_EQ(t.language(), "en");
    EXPECT_EQ(t.tr("save", "Save"), "Save");
}

TEST(TranslatorTest, PosixAndEmptyLocaleClearCatalog) {
    Translator t;
    t.loadFromText("k=v\n");
    t.setLocale("C");
    EXPECT_EQ(t.tr("k", "K"), "K");
    t.loadFromText("k=v\n");
    t.setLocale("");
    EXPECT_EQ(t.tr("k", "K"), "K");
}

TEST(TranslatorTest, MissingCatalogFileLeavesFallbacks) {
    Translator t;
    t.setLocale("xx_XX.UTF-8", "/nonexistent/dir");   // no file → fallbacks, no crash
    EXPECT_EQ(t.language(), "xx");
    EXPECT_EQ(t.tr("hello", "Hello"), "Hello");
}

TEST(TranslatorTest, LoadsCatalogFromDir) {
    // Write a temp catalog and load it via setLocale.
    std::string dir = "/tmp";
    std::string path = dir + "/zz.lang";
    { std::ofstream f(path); f << "hello=ZZ-HELLO\n"; }
    Translator t;
    t.setLocale("zz_ZZ.UTF-8", dir);
    EXPECT_EQ(t.tr("hello", "Hello"), "ZZ-HELLO");
    ::remove(path.c_str());
}
