// C7 corpus gate. Two questions, deliberately separate:
//
//  1. Detection — does this member trip the intended filter detector?
//     `filter_probe --expect=e8|delta|e8e9 <file>...`
//
//  2. Emission — does the archive the encoder produced actually carry a
//     filter region of the expected type, with the expected channel count and
//     offsets covering the member?
//     `filter_probe --archive=<arc> --entry=<name> --expect=e8|delta|none [--channels=N]`
//
//  3. Whole-archive type scan — does ANY member carry a reserved filter type
//     (4..7), which every reference decoder drops?
//     `filter_probe --scan-types=<arc>`
//
// Detection firing is NECESSARY, not sufficient, for C7 coverage: a member
// whose bytes never make `Filters50::detect_filter` return the expected type
// is not C7 evidence at all, and a detector that fires while the encoder emits
// nothing is not evidence either. Mode 2 parses the emitted records out of the
// member's token stream via the decoder's FilterCensus, so both claims are
// asserted against the real archive rather than against the input's shape.
//
// Exit: 0 all assertions held, 1 an assertion failed, 2 usage/IO error.

#include "archive/archive_reader.hpp"
#include "compress/decompressor50.hpp"
#include "compress/filters50.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace openrar;
using namespace openrar::compress;
using openrar::archive::ArchiveEntry;
using openrar::archive::ArchiveReader;

namespace {

const char* type_name(FilterType t) {
    switch (t) {
    case FilterType::Delta:
        return "DELTA";
    case FilterType::E8:
        return "E8";
    case FilterType::E8E9:
        return "E8E9";
    case FilterType::Arm:
        return "ARM";
    case FilterType::None:
        return "NONE";
    }
    return "?";
}

const char* type_name(core::uint8 t) {
    return type_name(static_cast<FilterType>(t));
}

bool parse_expect(const char* arg, FilterType& out) {
    if (std::strcmp(arg, "--expect=e8") == 0) {
        out = FilterType::E8;
        return true;
    }
    if (std::strcmp(arg, "--expect=e8e9") == 0) {
        out = FilterType::E8E9;
        return true;
    }
    if (std::strcmp(arg, "--expect=delta") == 0) {
        out = FilterType::Delta;
        return true;
    }
    return false;
}

// Unique leaf per call: two probes (or two entries in one scan) must not share
// a temp file, or a concurrent extraction can be truncated under the other.
std::filesystem::path probe_temp_path(const char* tag) {
    static std::atomic<unsigned> counter{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path() /
           ("openrar_filter_probe_" + std::string(tag) + "_" +
            std::to_string(static_cast<unsigned long long>(stamp)) + "_" +
            std::to_string(counter.fetch_add(1)));
}

std::vector<core::byte> read_file(const char* path, bool& ok) {
    std::ifstream f(path, std::ios::binary);
    ok = static_cast<bool>(f);
    if (!ok) return {};
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    f.seekg(0, std::ios::beg);
    if (n <= 0) {
        ok = false;
        return {};
    }
    std::vector<core::byte> buf(static_cast<size_t>(n));
    f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(n));
    ok = static_cast<bool>(f);
    return buf;
}

int detect_mode(const std::vector<std::string>& files, FilterType expect) {
    if (files.empty() || expect == FilterType::None) {
        std::fprintf(stderr,
                     "usage: filter_probe --expect=e8|e8e9|delta <file>...\n"
                     "  fails (exit 1) when the detector does not return the expected type\n");
        return 2;
    }

    int failures = 0;
    for (const std::string& path : files) {
        bool ok = false;
        const std::vector<core::byte> data = read_file(path.c_str(), ok);
        if (!ok) {
            std::fprintf(stderr, "FILTER-PROBE IO ERROR: %s\n", path.c_str());
            return 2;
        }
        core::uint8 channels = 0;
        const FilterType got = Filters50::detect_filter(data.data(), data.size(), channels);
        const bool pass = (got == expect);
        std::printf("[%s] %-24s expect=%-5s got=%-5s channels=%u\n", pass ? "PASS" : "FAIL",
                    path.c_str(), type_name(expect), type_name(got),
                    static_cast<unsigned>(channels));
        if (!pass) {
            ++failures;
            std::fprintf(stderr,
                         "FILTER-PROBE: %s does not trigger %s (detector returned %s). "
                         "It is not C7 coverage; fix the member or drop the claim.\n",
                         path.c_str(), type_name(expect), type_name(got));
        }
    }
    return failures == 0 ? 0 : 1;
}

int emission_mode(const std::string& archive_path, const std::string& entry_name, FilterType expect,
                  int want_channels) {
    ArchiveReader reader;
    if (!reader.open(archive_path)) {
        std::fprintf(stderr, "FILTER-PROBE: cannot open archive %s\n", archive_path.c_str());
        return 2;
    }
    const ArchiveEntry* entry = nullptr;
    for (const auto& e : reader.entries()) {
        if (e.header.file_name == entry_name) {
            entry = &e;
            break;
        }
    }
    if (entry == nullptr) {
        std::fprintf(stderr, "FILTER-PROBE: archive has no entry '%s'\n", entry_name.c_str());
        return 2;
    }
    const core::uint64 dest_size = entry->header.unp_size;

    FilterCensus census;
    reader.set_filter_census(&census);
    const std::filesystem::path tmp = probe_temp_path("emit");
    const bool extracted = reader.extract_entry(*entry, tmp);
    std::error_code ec;
    std::filesystem::remove(tmp, ec);
    reader.set_filter_census(nullptr);
    if (!extracted) {
        std::fprintf(stderr, "FILTER-PROBE: entry '%s' did not decode\n", entry_name.c_str());
        return 1;
    }

    std::vector<FilterEntry> recs;
    {
        std::lock_guard<std::mutex> lock(census.mutex);
        recs = census.entries;
    }
    std::sort(recs.begin(), recs.end(), [](const FilterEntry& a, const FilterEntry& b) {
        return a.block_start < b.block_start;
    });

    std::printf("[INFO] %s:%s records=%zu dest=%llu\n", archive_path.c_str(), entry_name.c_str(),
                recs.size(), static_cast<unsigned long long>(dest_size));

    std::vector<std::string> problems;
    core::uint64 covered = 0, next = 0;
    for (const auto& r : recs) {
        std::printf("[REC ] off=%llu len=%u type=%s channels=%u\n",
                    static_cast<unsigned long long>(r.block_start),
                    static_cast<unsigned>(r.block_length), type_name(r.type),
                    static_cast<unsigned>(r.channels));
        if (r.block_start != next) {
            problems.push_back(r.block_start < next ? "regions overlap or are out of order"
                                                    : "coverage gap between regions");
        }
        if (r.block_length == 0) problems.push_back("zero-length region");
        if (r.block_start > dest_size || r.block_length > dest_size - r.block_start) {
            problems.push_back("region runs past the member end");
        }
        if (static_cast<FilterType>(r.type) != expect) {
            problems.push_back(std::string("unexpected region type ") + type_name(r.type));
        }
        if (want_channels > 0 && static_cast<int>(r.channels) != want_channels) {
            problems.push_back("unexpected channel count " + std::to_string(r.channels));
        }
        next = r.block_start + r.block_length;
        covered += r.block_length;
    }

    if (expect == FilterType::None) {
        if (!recs.empty()) problems.push_back("expected no filter regions");
    } else {
        if (recs.empty()) problems.push_back("no filter region emitted at all");
        if (recs.empty() ? false : (recs.front().block_start != 0)) {
            problems.push_back("first region does not start at offset 0");
        }
        if (recs.empty() ? false : (next != dest_size)) {
            problems.push_back("regions do not cover the member end");
        }
        if (recs.empty() ? false : (covered != dest_size)) {
            problems.push_back("regions do not cover the member exactly");
        }
    }

    const bool pass = problems.empty();
    std::printf("[%s] %s:%s expect=%s channels=%d coverage=%llu/%llu\n", pass ? "PASS" : "FAIL",
                archive_path.c_str(), entry_name.c_str(), type_name(expect), want_channels,
                static_cast<unsigned long long>(covered),
                static_cast<unsigned long long>(dest_size));
    for (const auto& p : problems) {
        std::fprintf(stderr, "FILTER-PROBE: %s: %s\n", entry_name.c_str(), p.c_str());
    }
    return pass ? 0 : 1;
}

// Whole-archive filter-type scan (SEC-INTEROP-UNDEF-FILTER). Decodes every
// regular member and asserts no emitted filter record carries a reserved type
// (4..7). This is the corpus half of the row: the enum pin proves the encoder
// cannot write one, this proves the bytes on disk carry none.
int scan_types_mode(const std::string& archive_path) {
    ArchiveReader reader;
    if (!reader.open(archive_path)) {
        std::fprintf(stderr, "FILTER-PROBE: cannot open archive %s\n", archive_path.c_str());
        return 2;
    }

    size_t members = 0, records = 0, reserved = 0;
    for (const auto& entry : reader.entries()) {
        if (entry.header.is_service) continue;
        if ((entry.header.file_flags & format::file_flags::DIRECTORY) != 0) continue;
        if (entry.header.redir_type != 0) continue;

        FilterCensus census;
        reader.set_filter_census(&census);
        const std::filesystem::path tmp = probe_temp_path("scan");
        const bool ok = reader.extract_entry(entry, tmp);
        std::error_code ec;
        std::filesystem::remove(tmp, ec);
        reader.set_filter_census(nullptr);
        if (!ok) {
            std::fprintf(stderr,
                         "FILTER-PROBE: %s:%s did not decode; cannot claim a clean "
                         "filter-type scan\n",
                         archive_path.c_str(), entry.header.file_name.c_str());
            return 1;
        }
        ++members;

        std::lock_guard<std::mutex> lock(census.mutex);
        for (const auto& r : census.entries) {
            ++records;
            if (r.type > 3) {
                ++reserved;
                std::fprintf(stderr,
                             "FILTER-PROBE: %s:%s emitted RESERVED filter type %u "
                             "(reference decoders drop the region)\n",
                             archive_path.c_str(), entry.header.file_name.c_str(),
                             static_cast<unsigned>(r.type));
            }
        }
    }

    const bool pass = reserved == 0 && members > 0;
    std::printf("[%s] %s members=%zu records=%zu type>3=%zu\n", pass ? "PASS" : "FAIL",
                archive_path.c_str(), members, records, reserved);
    if (members == 0) {
        std::fprintf(stderr,
                     "FILTER-PROBE: %s has no decodable regular members; an empty "
                     "scan is not evidence\n",
                     archive_path.c_str());
    }
    return pass ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    std::string archive_path, entry_name, scan_path;
    std::vector<std::string> files;
    FilterType expect = FilterType::None;
    bool archive_mode = false, have_expect = false;
    int want_channels = 0;

    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (std::strncmp(a, "--scan-types=", 13) == 0) {
            scan_path = a + 13;
        } else if (std::strncmp(a, "--archive=", 10) == 0) {
            archive_path = a + 10;
            archive_mode = true;
        } else if (std::strncmp(a, "--entry=", 8) == 0) {
            entry_name = a + 8;
        } else if (std::strncmp(a, "--channels=", 11) == 0) {
            want_channels = std::atoi(a + 11);
        } else if (std::strcmp(a, "--expect=none") == 0) {
            expect = FilterType::None;
            have_expect = true;
        } else if (parse_expect(a, expect)) {
            have_expect = true;
        } else if (a[0] == '-') {
            std::fprintf(stderr, "FILTER-PROBE: unknown option '%s'\n", a);
            return 2;
        } else {
            files.emplace_back(a);
        }
    }

    if (!scan_path.empty()) {
        if (archive_mode || !files.empty()) {
            std::fprintf(stderr, "usage: filter_probe --scan-types=<arc>\n");
            return 2;
        }
        return scan_types_mode(scan_path);
    }
    if (archive_mode) {
        if (!have_expect || entry_name.empty() || !files.empty()) {
            std::fprintf(stderr, "usage: filter_probe --archive=<arc> --entry=<name> "
                                 "--expect=e8|delta|none [--channels=N]\n");
            return 2;
        }
        return emission_mode(archive_path, entry_name, expect, want_channels);
    }
    return detect_mode(files, expect);
}
