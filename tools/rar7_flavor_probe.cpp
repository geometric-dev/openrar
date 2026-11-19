// Probe: does a rar.exe-produced archive carry unp_ver=1 / FCI_RAR5_COMPAT
// clear (the extra_dist / RAR7-table flavor §4.26 needs)?
//
// The roadmap refuses to assume -ma7 produces this flavor: the fixture must be
// verified by re-parsing through header_reader.cpp, not by trusting a switch.
//
// Usage: rar7_flavor_probe <archive>
// Exit 0 = at least one member is extra_dist; 1 = none are (flavor absent);
// 2 = could not open/parse.

#include "../src/archive/archive_reader.hpp"

#include <cstdio>
#include <string>

using namespace openrar;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: rar7_flavor_probe <archive>\n");
        return 2;
    }

    archive::ArchiveReader reader;
    std::string detail;
    int status = 0;
    if (!reader.open_ex(argv[1], "", status, detail)) {
        std::fprintf(stderr, "cannot open %s (status=%d, %s)\n", argv[1], status, detail.c_str());
        return 2;
    }

    size_t total = 0, extra = 0, compat = 0, encrypted = 0;
    for (const auto& e : reader.entries()) {
        if (e.header.is_service) continue;
        if ((e.header.file_flags & format::file_flags::DIRECTORY) != 0) continue;
        ++total;
        if (e.header.extra_dist) {
            ++extra;
            std::printf("  [extra_dist] %-28s unp_size=%llu win_size=%llu\n",
                        e.header.file_name.c_str(),
                        static_cast<unsigned long long>(e.header.unp_size),
                        static_cast<unsigned long long>(e.header.win_size));
        } else if (e.header.unp_ver == 1) {
            ++compat;
            std::printf("  [ver=1 compat] %-24s unp_size=%llu\n", e.header.file_name.c_str(),
                        static_cast<unsigned long long>(e.header.unp_size));
        } else {
            std::printf("  [ver=%u] %-33s unp_size=%llu\n", e.header.unp_ver,
                        e.header.file_name.c_str(),
                        static_cast<unsigned long long>(e.header.unp_size));
        }
        if (e.header.is_encrypted) ++encrypted;
    }

    std::printf("\ntotal=%zu extra_dist=%zu ver1_compat=%zu encrypted=%zu\n", total, extra, compat,
                encrypted);
    if (total == 0) {
        std::fprintf(stderr, "no regular members parsed - not evidence\n");
        return 2;
    }
    if (extra == 0) {
        std::printf("\nFLAVOR ABSENT: no member has unp_ver=1 with FCI_RAR5_COMPAT clear.\n"
                    "This switch/corpus cannot serve as C8.\n");
        return 1;
    }
    std::printf("\nFLAVOR PRESENT: %zu extra_dist member(s) - this is C8.\n", extra);
    return 0;
}