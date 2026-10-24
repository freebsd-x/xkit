#include "xkit/translator.h"

#include <fstream>
#include <sstream>

namespace fbx::xkit {

namespace {
std::string_view trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r')) --e;
    return s.substr(b, e - b);
}
std::string unescape(std::string_view v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            char n = v[i + 1];
            if (n == 'n')      { out += '\n'; ++i; continue; }
            if (n == 't')      { out += '\t'; ++i; continue; }
            if (n == '\\')     { out += '\\'; ++i; continue; }
        }
        out += v[i];
    }
    return out;
}
}  // namespace

std::unordered_map<std::string, std::string>
Translator::parseCatalog(std::string_view text) noexcept {
    std::unordered_map<std::string, std::string> map;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                                              : nl - pos);
        pos = (nl == std::string_view::npos) ? text.size() + 1 : nl + 1;

        std::string_view t = trim(line);
        if (t.empty() || t[0] == '#') continue;
        size_t eq = t.find('=');
        if (eq == std::string_view::npos) continue;
        std::string_view key = trim(t.substr(0, eq));
        std::string_view val = trim(t.substr(eq + 1));
        if (key.empty()) continue;
        map[std::string(key)] = unescape(val);
    }
    return map;
}

std::string Translator::languageOf(std::string_view locale) noexcept {
    size_t end = 0;
    while (end < locale.size() && locale[end] != '_' && locale[end] != '.' &&
           locale[end] != '@')
        ++end;
    return std::string(locale.substr(0, end));
}

void Translator::loadFromText(std::string_view text) noexcept {
    catalog_ = parseCatalog(text);
}

void Translator::setLocale(std::string_view locale, std::string_view base_dir) noexcept {
    language_ = languageOf(locale);
    catalog_.clear();
    // The default/English locale ships no catalog — fallbacks are the source text.
    if (language_.empty() || language_ == "C" || language_ == "POSIX" ||
        language_ == "en")
        return;
    std::string dir = base_dir.empty() ? "/usr/local/share/freebsdx/locale"
                                       : std::string(base_dir);
    std::ifstream f(dir + "/" + language_ + ".lang");
    if (!f.is_open()) return;   // no catalog → fallbacks
    std::stringstream ss;
    ss << f.rdbuf();
    catalog_ = parseCatalog(ss.str());
}

std::string Translator::tr(std::string_view key, std::string_view fallback) const noexcept {
    auto it = catalog_.find(std::string(key));
    if (it != catalog_.end()) return it->second;
    return fallback.empty() ? std::string(key) : std::string(fallback);
}

Translator& Translator::global() noexcept {
    static Translator instance;
    return instance;
}

}  // namespace fbx::xkit
