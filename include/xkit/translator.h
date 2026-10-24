#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace fbx::xkit {

// Lightweight message-catalog translator for app strings.
//
// Apps call tr("key", "English fallback"); the returned string is the catalog
// entry for the active locale, or the fallback (or the key itself if no fallback)
// when there's no translation. Catalogs are simple `key=value` text files named
// <lang>.lang in a base directory (default /usr/local/share/freebsdx/locale),
// e.g. fr.lang. setLocale("fr_FR.UTF-8") extracts the language ("fr") and loads
// fr.lang; the default/English locale needs no catalog (fallbacks are used).
//
// One process-wide instance via global(); the parsing/lookup is pure and does
// not depend on the singleton, so it is unit-testable directly.
class Translator {
public:
    // Parse a `key=value` catalog from text (public for testing). Lines that are
    // blank or start with '#' are ignored; the value runs to end-of-line and may
    // contain '='. Leading/trailing spaces around key and value are trimmed;
    // "\n" escapes in the value become newlines.
    static std::unordered_map<std::string, std::string>
        parseCatalog(std::string_view text) noexcept;

    // Reduce a locale string to its language subtag: "fr_FR.UTF-8" → "fr",
    // "en" → "en", "" → "".
    static std::string languageOf(std::string_view locale) noexcept;

    // Set the active locale and load its catalog from base_dir (default path if
    // empty). An empty/"C"/"POSIX"/"en*" locale clears the catalog (fallbacks).
    void setLocale(std::string_view locale, std::string_view base_dir = "") noexcept;

    // Look up key; returns the translation, else fallback, else the key.
    std::string tr(std::string_view key, std::string_view fallback = "") const noexcept;

    // Load a catalog directly from text (bypasses the file; for tests / embedding).
    void loadFromText(std::string_view text) noexcept;

    const std::string& language() const noexcept { return language_; }

    static Translator& global() noexcept;

private:
    std::string language_;
    std::unordered_map<std::string, std::string> catalog_;
};

// Convenience free function on the global translator.
inline std::string tr(std::string_view key, std::string_view fallback = "") noexcept {
    return Translator::global().tr(key, fallback);
}

}  // namespace fbx::xkit
