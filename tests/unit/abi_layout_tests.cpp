// Native-side test for the FROZEN C ABI layout (docs/abi-freeze.md).
//
// Pins, at runtime, on every CI platform:
//   1. the compiled layout of the four public packed structs against the
//      committed golden tools/abi_layout.json (drift fails with a diff);
//   2. the C-mode compile + layout of the same header (the extern "C"
//      check implemented in abi_header_c_compat.c);
//   3. openrar_abi_features() returns EXACTLY the documented registry bits
//      (no unregistered bits, no undocumented gaps);
//   4. the version-probe contract of docs/dll-integration-spec.md §3.
//
// Compile-time pins live in src/dll/dll_api.cpp (absolute offsets, alignof,
// enum equivalence); this file is the cross-platform runtime net that also
// catches toolchain changes in Release/LTO builds where static_asserts and
// the shipped binary could otherwise drift apart silently.
//
// Golden rule (docs/abi-freeze.md): a field rename/removal here and in
// tools/abi_layout.json must land in the SAME change as the header edit.

#include <openrar/openrar_dll.h>

#include "test_support.hpp"

#include <cstddef>
#include <utility>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef OPENRAR_SOURCE_DIR
#define ABI_GOLDEN_DIR OPENRAR_SOURCE_DIR
#else
#define ABI_GOLDEN_DIR "."
#endif

static int fails = 0;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                    \
            ++fails;                                                                               \
        }                                                                                          \
    } while (0)

// Implemented in abi_header_c_compat.c (compiled as C).
extern "C" int abi_c_mode_version_probe(void);
extern "C" int abi_c_mode_layout_check(void);

// ── Minimal JSON reader for the flat golden file (no third-party deps) ───────
namespace {

struct FieldGolden {
    std::string name;
    long long offset = -1;
    long long size = -1;
};

struct StructGolden {
    std::string name;
    long long size = -1;
    long long align = -1;
    std::vector<FieldGolden> fields;
};

class JsonCursor {
public:
    explicit JsonCursor(const std::string& text) : t_(text) {}

    bool ok() const { return !failed_; }
    void fail(const char* why) {
        if (!failed_) {
            failed_ = true;
            std::fprintf(stderr, "abi_layout.json parse error at byte %zu: %s\n", pos_, why);
        }
    }

    void skip_ws() {
        while (pos_ < t_.size()) {
            char c = t_[pos_];
            if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
                ++pos_;
            else
                break;
        }
    }
    bool eat(char c) {
        skip_ws();
        if (pos_ < t_.size() && t_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }
    bool eat_literal(const char* lit) {
        skip_ws();
        size_t n = std::strlen(lit);
        if (t_.compare(pos_, n, lit) == 0) {
            pos_ += n;
            return true;
        }
        return false;
    }
    std::string read_string() {
        skip_ws();
        if (pos_ >= t_.size() || t_[pos_] != '"') {
            fail("expected string");
            return std::string();
        }
        ++pos_;
        std::string out;
        while (pos_ < t_.size() && t_[pos_] != '"') {
            if (t_[pos_] == '\\' && pos_ + 1 < t_.size()) ++pos_; // tolerate escapes
            out += t_[pos_++];
        }
        if (pos_ >= t_.size()) {
            fail("unterminated string");
            return out;
        }
        ++pos_; // closing quote
        return out;
    }
    long long read_number() {
        skip_ws();
        bool neg = false;
        if (pos_ < t_.size() && t_[pos_] == '-') {
            neg = true;
            ++pos_;
        }
        long long v = 0;
        bool any = false;
        while (pos_ < t_.size() && t_[pos_] >= '0' && t_[pos_] <= '9') {
            v = v * 10 + (t_[pos_] - '0');
            ++pos_;
            any = true;
        }
        if (!any) fail("expected number");
        return neg ? -v : v;
    }
    // Reads the whole "structs": [ ... ] array of the golden file.
    std::vector<StructGolden> read_structs() {
        std::vector<StructGolden> out;
        if (!eat_literal("{")) {
            fail("expected '{' at document start");
            return out;
        }
        if (!eat_string_key("comment") || !skip_value()) return out;
        if (!eat(',')) {
            fail("expected ',' after comment");
            return out;
        }
        if (!eat_string_key("structs") || !eat('[')) return out;
        while (ok()) {
            if (eat(']')) break;
            if (!eat('{')) {
                fail("expected struct object");
                break;
            }
            StructGolden s;
            while (ok()) {
                if (eat('}')) break;
                std::string key = read_string();
                if (!eat(':')) break;
                if (key == "name") {
                    s.name = read_string();
                } else if (key == "size") {
                    s.size = read_number();
                } else if (key == "align") {
                    s.align = read_number();
                } else if (key == "fields") {
                    if (!read_fields(s)) return out;
                } else {
                    skip_value();
                }
                eat(',');
            }
            out.push_back(s);
            eat(',');
        }
        return out;
    }

private:
    bool read_fields(StructGolden& s) {
        if (!eat('[')) return false;
        while (ok()) {
            if (eat(']')) break;
            if (!eat('{')) {
                fail("expected field object");
                return false;
            }
            FieldGolden f;
            while (ok()) {
                if (eat('}')) break;
                std::string fk = read_string();
                if (!eat(':')) return false;
                if (fk == "name") {
                    f.name = read_string();
                } else if (fk == "offset") {
                    f.offset = read_number();
                } else if (fk == "size") {
                    f.size = read_number();
                } else {
                    skip_value();
                }
                eat(',');
            }
            s.fields.push_back(f);
            eat(',');
        }
        return true;
    }
    bool eat_string_key(const char* key) {
        std::string k = read_string();
        if (!ok() || k != key) {
            fail("unexpected key");
            return false;
        }
        return eat(':');
    }
    // Skips one value: string, number, object, array, or literal.
    bool skip_value() {
        skip_ws();
        if (pos_ >= t_.size()) return fail("unexpected end"), false;
        char c = t_[pos_];
        if (c == '"') {
            read_string();
            return ok();
        }
        if (c == '{' || c == '[') {
            char open = c, close = (c == '{') ? '}' : ']';
            int depth = 0;
            while (pos_ < t_.size()) {
                char d = t_[pos_];
                if (d == '"') { // skip strings wholesale
                    read_string();
                    if (!ok()) return false;
                    continue;
                }
                ++pos_;
                if (d == open) ++depth;
                if (d == close && --depth == 0) return true;
            }
            return fail("unbalanced value"), false;
        }
        read_number();
        return ok();
    }

    const std::string& t_;
    size_t pos_ = 0;
    bool failed_ = false;
};

std::string read_file_or_empty(const char* path) {
    std::FILE* f = std::fopen(path, "rb");
    if (!f) return std::string();
    std::string out;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

struct FieldActual {
    const char* name;
    long long offset;
    long long size;
};

void report_and_check(const StructGolden& s, long long actual_size, long long actual_align,
                      const std::vector<FieldActual>& actual) {
    std::printf("[abi] %-26s size=%lld align=%lld (golden size=%lld align=%lld)\n", s.name.c_str(),
                actual_size, actual_align, s.size, s.align);
    CHECK(actual_size == s.size);
    CHECK(actual_align == s.align);
    CHECK(s.fields.size() == actual.size());
    for (const FieldGolden& f : s.fields) {
        const FieldActual* match = nullptr;
        for (const FieldActual& a : actual)
            if (std::strcmp(a.name, f.name.c_str()) == 0) match = &a;
        if (!match) {
            std::fprintf(stderr, "FAIL golden field '%s.%s' has no C-struct mapping\n",
                         s.name.c_str(), f.name.c_str());
            ++fails;
            continue;
        }
        std::printf("[abi]   %-16s offset=%-3lld size=%lld\n", f.name.c_str(), match->offset,
                    match->size);
        CHECK(match->offset == f.offset);
        CHECK(match->size == f.size);
    }
}

template <typename T, typename Member>
FieldActual field_of(const char* name, Member T::* /*tag*/, long long offset_of,
                     long long size_of) {
    return FieldActual{name, offset_of, size_of};
}

#define ABI_F(T, member)                                                                           \
    FieldActual {                                                                                  \
        #member, static_cast<long long>(offsetof(T, member)),                                      \
            static_cast<long long>(sizeof(std::declval<T&>().member))                              \
    }

void check_entry_t(const StructGolden& s) {
    using T = openrar_archive_entry_t;
    std::vector<FieldActual> a = {
        ABI_F(T, path_offset),  ABI_F(T, path_len), ABI_F(T, is_dir), ABI_F(T, method),
        ABI_F(T, is_encrypted), ABI_F(T, crc32),    ABI_F(T, size),   ABI_F(T, packed_size),
        ABI_F(T, mtime),        ABI_F(T, _pad),
    };
    report_and_check(s, static_cast<long long>(sizeof(T)), static_cast<long long>(alignof(T)), a);
}

void check_entry_ex_t(const StructGolden& s) {
    using T = openrar_entry_ex_t;
    std::vector<FieldActual> a = {
        ABI_F(T, attrs),    ABI_F(T, host_os),    ABI_F(T, mtime_ft),
        ABI_F(T, ctime_ft), ABI_F(T, atime_ft),   ABI_F(T, flags),
        ABI_F(T, win_size), ABI_F(T, redir_type), ABI_F(T, version_needed),
    };
    report_and_check(s, static_cast<long long>(sizeof(T)), static_cast<long long>(alignof(T)), a);
}

void check_entry_owner_t(const StructGolden& s) {
    using T = openrar_entry_owner_t;
    std::vector<FieldActual> a = {ABI_F(T, uid), ABI_F(T, gid), ABI_F(T, flags)};
    report_and_check(s, static_cast<long long>(sizeof(T)), static_cast<long long>(alignof(T)), a);
}

void check_archive_info_t(const StructGolden& s) {
    using T = openrar_archive_info_t;
    std::vector<FieldActual> a = {ABI_F(T, flags), ABI_F(T, volume_index), ABI_F(T, volume_count),
                                  ABI_F(T, recovery_size), ABI_F(T, comment_len)};
    report_and_check(s, static_cast<long long>(sizeof(T)), static_cast<long long>(alignof(T)), a);
}

#undef ABI_F

} // namespace

int main() {
    OPENRAR_ROUTE_CRT_ASSERT_TO_STDERR();
    // 1. C-mode compile + layout of the public header.
    CHECK(abi_c_mode_version_probe());
    CHECK(abi_c_mode_layout_check() == 0);

    // 2. Runtime layout vs the committed golden.
    const std::string json = read_file_or_empty(ABI_GOLDEN_DIR "/tools/abi_layout.json");
    CHECK(!json.empty());
    if (!json.empty()) {
        JsonCursor cur(json);
        std::vector<StructGolden> structs = cur.read_structs();
        CHECK(cur.ok());
        CHECK(structs.size() == 5); // 4 C structs + canonical ArchiveEntryOut
        for (const StructGolden& s : structs) {
            if (s.name == "openrar_archive_entry_t") {
                check_entry_t(s);
            } else if (s.name == "openrar_entry_ex_t") {
                check_entry_ex_t(s);
            } else if (s.name == "openrar_entry_owner_t") {
                check_entry_owner_t(s);
            } else if (s.name == "openrar_archive_info_t") {
                check_archive_info_t(s);
            } else if (s.name == "openrar::api::ArchiveEntryOut") {
                // The canonical C++ struct is internal (abi_contract.hpp); its
                // offsets are pinned to openrar_archive_entry_t by the
                // dll_api.cpp offsetof pairs. Here only the totals are checked.
                CHECK(s.size == 64);
                CHECK(s.align == 8);
            } else {
                std::fprintf(stderr, "FAIL unknown golden struct '%s'\n", s.name.c_str());
                ++fails;
            }
        }
    }

    // 3. Feature-mask parity: the implementation must report EXACTLY the
    // bits documented in the header registry — no unregistered bits set,
    // no registered bit missing.
    const uint64_t documented =
        OPENRAR_ABI_FEATURE_LIST_PROGRESS | OPENRAR_ABI_FEATURE_LIST_PASSWORD |
        OPENRAR_ABI_FEATURE_HANDLE_OPEN_PROGRESS | OPENRAR_ABI_FEATURE_FILE_HANDLE |
        OPENRAR_ABI_FEATURE_MUTATION | OPENRAR_ABI_FEATURE_ENTRY_EX |
        OPENRAR_ABI_FEATURE_PACKAGE_VERSION | OPENRAR_ABI_FEATURE_SET_LIMITS |
        OPENRAR_ABI_FEATURE_REPAIR | OPENRAR_ABI_FEATURE_CREATE | OPENRAR_ABI_FEATURE_FILTERS |
        OPENRAR_ABI_FEATURE_OWNER | OPENRAR_ABI_FEATURE_DICT_EX | OPENRAR_ABI_FEATURE_VOL_ENCRYPT |
        OPENRAR_ABI_FEATURE_REC_VOL | OPENRAR_ABI_FEATURE_PARALLEL_COMPRESS |
        OPENRAR_ABI_FEATURE_MMAP;
    const uint64_t reported = openrar_abi_features();
    CHECK(reported == documented);
    if (reported != documented) {
        std::fprintf(stderr,
                     "FAIL feature mask: reported=0x%llx documented=0x%llx extra=0x%llx "
                     "missing=0x%llx\n",
                     static_cast<unsigned long long>(reported),
                     static_cast<unsigned long long>(documented),
                     static_cast<unsigned long long>(reported & ~documented),
                     static_cast<unsigned long long>(documented & ~reported));
    }

    // 4. Version-probe contract (spec §3): strict equality, package string
    // present.
    CHECK(openrar_version() == OPENRAR_DLL_API_VERSION);
    CHECK(openrar_archive_version() == 1);
    CHECK(openrar_package_version_string() != nullptr);
    CHECK(std::strlen(openrar_package_version_string()) > 0);

    if (fails == 0) std::printf("[abi] layout golden + C-compat + feature mask: OK\n");
    return fails == 0 ? 0 : 1;
}
