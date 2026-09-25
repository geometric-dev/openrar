// Unicode normalization + simple case folding runtime (v1.24.0 plan §3.3).
// Table data lives in unicode_tables.cpp (generated from Unicode 15.1.0 by
// tools/gen_unicode_tables.py); Hangul syllables are handled algorithmically
// per UAX #15, never table-stored.
#include "unicode_tables.hpp"

#include <algorithm>
#include <vector>

namespace openrar::unicode {

namespace {

// --- sorted-table lookup (binary search over (key, value) pairs) -----------

struct TableResult {
    bool found;
    core::uint32 value;
};

TableResult lookup_pair(const core::uint32* table, int count, core::uint32 key) {
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const core::uint32 k = table[mid * 2];
        if (k == key) return {true, table[mid * 2 + 1]};
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return {false, 0};
}

TableResult lookup_triple_first(const core::uint32* table, int count, core::uint32 key) {
    // triples (a, b, composite): match on the FIRST element (a) — used for
    // decomposition lookups where cp is the first element.
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const core::uint32 k = table[mid * 3];
        if (k == key) return {true, static_cast<core::uint32>(mid)};
        if (k < key)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return {false, 0u};
}

// --- Hangul (UAX #15 constants) ---------------------------------------------
constexpr core::uint32 kSBase = 0xAC00, kLBase = 0x1100, kVBase = 0x1161, kTBase = 0x11A7;
constexpr int kLCount = 19, kVCount = 21, kTCount = 28;
constexpr int kNCount = kVCount * kTCount;
constexpr int kSCount = kLCount * kNCount;
constexpr core::uint32 kSLast = kSBase + kSCount - 1;

bool is_decomposition_sentinel(core::uint32 b) {
    return b == 0; // singleton decomposition marker
}

// --- UTF-8 -------------------------------------------------------------------

// Decodes one codepoint. Returns bytes consumed (0 on invalid sequence); on
// invalid input the caller treats the lead byte as a literal.
size_t utf8_decode(const std::string& s, size_t i, core::uint32& cp) {
    const auto lead = static_cast<unsigned char>(s[i]);
    if (lead < 0x80) {
        cp = lead;
        return 1;
    }
    size_t len = 0;
    core::uint32 v = 0;
    if ((lead & 0xE0) == 0xC0) {
        len = 2;
        v = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        v = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        v = lead & 0x07;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    for (size_t k = 1; k < len; ++k) {
        const auto cont = static_cast<unsigned char>(s[i + k]);
        if ((cont & 0xC0) != 0x80) return 0;
        v = (v << 6) | (cont & 0x3F);
    }
    // overlong / surrogate / out-of-range rejection
    static const core::uint32 kMin[4] = {0u, 0u, 0x80u, 0x800u};
    if (v < kMin[len] || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF)) return 0;
    cp = v;
    return len;
}

void utf8_append(std::string& out, core::uint32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

int combining_class_of(core::uint32 cp) {
    static const core::uint32* table = nullptr;
    static int count = 0;
    if (table == nullptr) table = combining_class_table(count);
    const TableResult r = lookup_pair(table, count, cp);
    return r.found ? static_cast<int>(r.value) : 0;
}

// Canonical decomposition of one codepoint (Hangul algorithmic; table
// otherwise; unknown codepoints stand alone).
void decompose_one(core::uint32 cp, std::vector<core::uint32>& out) {
    if (cp >= kSBase && cp <= kSLast) {
        const core::uint32 s_index = cp - kSBase;
        const core::uint32 l = kLBase + s_index / kNCount;
        const core::uint32 v = kVBase + (s_index % kNCount) / kTCount;
        const core::uint32 t = kTBase + (s_index % kTCount);
        out.push_back(l);
        out.push_back(v);
        if (t != kTBase) out.push_back(t);
        return;
    }
    static const core::uint32* table = nullptr;
    static int count = 0;
    if (table == nullptr) table = decomposition_table(count);
    const TableResult r = lookup_triple_first(table, count, cp);
    if (r.found) {
        const core::uint32* row = table + static_cast<size_t>(r.value) * 3;
        const core::uint32 a = row[1];
        const core::uint32 b = row[2];
        out.push_back(a);
        if (!is_decomposition_sentinel(b)) out.push_back(b);
        return;
    }
    out.push_back(cp);
}

void canonical_decompose(core::uint32 cp, std::vector<core::uint32>& out) {
    // Single-level decomposition suffices: UnicodeData decompositions are
    // recursively closed (decomposition targets never decompose further).
    decompose_one(cp, out);
}

void canonical_order(std::vector<core::uint32>& seq) {
    // Canonical ordering: stable bubble of combining marks (CCC != 0) within
    // their starter blocks (UAX #15).
    for (size_t i = 1; i < seq.size(); ++i) {
        const int ccc_i = combining_class_of(seq[i]);
        if (ccc_i == 0) continue;
        size_t j = i;
        while (j > 0) {
            const int ccc_prev = combining_class_of(seq[j - 1]);
            if (ccc_prev == 0 || ccc_prev <= ccc_i) break;
            std::swap(seq[j - 1], seq[j]);
            --j;
        }
    }
}

bool compose_pair(core::uint32 a, core::uint32 b, core::uint32& out);

// Standard canonical composition (UAX #15): greedy forward scan; a
// non-starter composes with the LAST STARTER unless blocked by a character
// with CCC >= its own between them (canonical ordering makes those adjacent
// in class order, so tracking the last appended CCC suffices).
std::string compose_sequence(const std::vector<core::uint32>& seq) {
    std::vector<core::uint32> out;
    out.reserve(seq.size());
    long last_starter = -1; // index in out of the last starter
    int last_ccc = 0;       // CCC of the last appended (or composed-to) char
    for (const core::uint32 cp : seq) {
        const int ccc = combining_class_of(cp);
        const bool can_compose = last_starter >= 0 && (last_ccc == 0 ? true : ccc > last_ccc);
        core::uint32 composed = 0;
        if (can_compose && compose_pair(out[static_cast<size_t>(last_starter)], cp, composed)) {
            out[static_cast<size_t>(last_starter)] = composed;
            last_ccc = 0; // a primary composite is a starter (CCC 0)
            continue;
        }
        out.push_back(cp);
        if (ccc == 0) last_starter = static_cast<long>(out.size()) - 1;
        last_ccc = ccc;
    }
    std::string out_str;
    out_str.reserve(out.size() * 2);
    for (const core::uint32 c : out) utf8_append(out_str, c);
    return out_str;
}

bool compose_pair(core::uint32 a, core::uint32 b, core::uint32& out) {
    // Hangul composition first (algorithmic).
    if (a >= kLBase && a < kLBase + kLCount && b >= kVBase && b < kVBase + kVCount) {
        out = kSBase + ((a - kLBase) * kVCount + (b - kVBase)) * kTCount;
        return true;
    }
    if (a >= kSBase && a <= kSLast && (a - kSBase) % kTCount == 0 && b > kTBase &&
        b < kTBase + kTCount) {
        out = a + (b - kTBase);
        return true;
    }
    static const core::uint32* table = nullptr;
    static int count = 0;
    if (table == nullptr) table = composition_table(count);
    // composition triples are sorted by (a, b); find a's block then match b.
    int lo = 0, hi = count - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const core::uint32 ka = table[static_cast<size_t>(mid) * 3];
        if (ka == a) {
            // scan the a-block
            int lo2 = mid, hi2 = mid;
            while (lo2 > 0 && table[static_cast<size_t>(lo2 - 1) * 3] == a) --lo2;
            while (hi2 < count - 1 && table[static_cast<size_t>(hi2 + 1) * 3] == a) ++hi2;
            for (int k = lo2; k <= hi2; ++k) {
                if (table[static_cast<size_t>(k) * 3 + 1] == b) {
                    out = table[static_cast<size_t>(k) * 3 + 2];
                    return true;
                }
            }
            return false;
        }
        if (ka < a)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return false;
}

} // namespace

std::string simple_case_fold(const std::string& utf8) {
    static const core::uint32* table = nullptr;
    static int count = 0;
    if (table == nullptr) table = case_fold_table(count);
    std::string out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        core::uint32 cp = 0;
        const size_t len = utf8_decode(utf8, i, cp);
        if (len == 0) {
            out.push_back(utf8[i]); // invalid byte: literal passthrough
            i += 1;
            continue;
        }
        const TableResult r = lookup_pair(table, count, cp);
        utf8_append(out, r.found ? r.value : cp);
        i += len;
    }
    return out;
}

std::string nfc(const std::string& utf8) {
    std::vector<core::uint32> seq;
    seq.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        core::uint32 cp = 0;
        const size_t len = utf8_decode(utf8, i, cp);
        if (len == 0) {
            // Invalid byte: flush what we have (invalid bytes act as
            // starters/blockers), pass the byte through literally, and
            // normalize the remainder independently.
            canonical_order(seq);
            std::string out = compose_sequence(seq);
            out.push_back(utf8[i]);
            seq.clear();
            i += 1;
            std::string rest(utf8.begin() + static_cast<long>(i), utf8.end());
            out += nfc(rest);
            return out;
        }
        canonical_decompose(cp, seq);
        i += len;
    }
    canonical_order(seq);
    return compose_sequence(seq);
}

} // namespace openrar::unicode
