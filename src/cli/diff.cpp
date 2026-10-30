#include "diff.hpp"

#include "../archive/archive_reader.hpp"
#include "json_output.hpp"
#include "progress.hpp"
#include "tui.hpp"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <vector>

namespace openrar::cli {

namespace {

// Directive 4: Canonical path normalization
std::string normalize_path(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) {
        if (c == '\\')
            s.push_back('/');
        else
            s.push_back(c);
    }
    size_t start = s.find_first_not_of('/');
    if (start != std::string::npos)
        s = s.substr(start);
    else
        s.clear();
    while (!s.empty() && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

std::string format_hex_crc(uint32_t crc) {
    std::ostringstream ss;
    ss << std::hex << std::uppercase << std::setw(8) << std::setfill('0') << crc;
    return ss.str();
}

enum class DiffKind { Added, Deleted, Modified, Metadata, Identical };

struct DiffItem {
    std::string path;
    DiffKind kind{DiffKind::Identical};
    uint64_t size1{0};
    uint64_t size2{0};
    uint32_t crc1{0};
    uint32_t crc2{0};
    int method1{0};
    int method2{0};
    uint64_t mtime1{0};
    uint64_t mtime2{0};
};

} // namespace

int diff_archives(const DiffOptions& opt) {
    if (!std::filesystem::exists(opt.arc1_path)) {
        std::cerr << "Error: File not found: " << opt.arc1_path << "\n";
        return 2; // Directive 3: POSIX diff error code
    }
    if (!std::filesystem::exists(opt.arc2_path)) {
        std::cerr << "Error: File not found: " << opt.arc2_path << "\n";
        return 2;
    }

    archive::ArchiveReader r1, r2;
    int st1 = 0, st2 = 0;
    std::string det1, det2;

    if (!r1.open_ex(opt.arc1_path, opt.password, st1, det1)) {
        std::cerr << "Error: Cannot open primary archive: " << opt.arc1_path << "\n";
        return 2;
    }
    if (!r2.open_ex(opt.arc2_path, opt.password, st2, det2)) {
        std::cerr << "Error: Cannot open secondary archive: " << opt.arc2_path << "\n";
        return 2;
    }

    // Build normalized entry catalogs (excluding service blocks)
    std::map<std::string, const archive::ArchiveEntry*> m1, m2;
    for (const auto& e : r1.entries()) {
        if (!e.header.is_service) {
            std::string np = normalize_path(e.header.file_name);
            if (!np.empty()) m1[np] = &e;
        }
    }
    for (const auto& e : r2.entries()) {
        if (!e.header.is_service) {
            std::string np = normalize_path(e.header.file_name);
            if (!np.empty()) m2[np] = &e;
        }
    }

    std::vector<DiffItem> items;
    size_t count_added = 0;
    size_t count_deleted = 0;
    size_t count_modified = 0;
    size_t count_metadata = 0;
    size_t count_identical = 0;

    // Check files in m1 (deleted, modified, metadata, or identical)
    for (const auto& [p, e1] : m1) {
        auto it = m2.find(p);
        if (it == m2.end()) {
            DiffItem item;
            item.path = p;
            item.kind = DiffKind::Deleted;
            item.size1 = e1->header.unp_size;
            item.crc1 = e1->header.data_crc32;
            item.method1 = e1->header.method;
            item.mtime1 = e1->header.mtime_win;
            items.push_back(item);
            count_deleted++;
        } else {
            const auto* e2 = it->second;
            DiffItem item;
            item.path = p;
            item.size1 = e1->header.unp_size;
            item.size2 = e2->header.unp_size;
            item.crc1 = e1->header.data_crc32;
            item.crc2 = e2->header.data_crc32;
            item.method1 = e1->header.method;
            item.method2 = e2->header.method;
            item.mtime1 = e1->header.mtime_win;
            item.mtime2 = e2->header.mtime_win;

            if (e1->header.unp_size != e2->header.unp_size ||
                e1->header.data_crc32 != e2->header.data_crc32) {
                item.kind = DiffKind::Modified;
                count_modified++;
            } else if (e1->header.mtime_win != e2->header.mtime_win ||
                       e1->header.method != e2->header.method) {
                item.kind = DiffKind::Metadata;
                count_metadata++;
            } else {
                item.kind = DiffKind::Identical;
                count_identical++;
            }
            items.push_back(item);
        }
    }

    // Check files in m2 that are not in m1 (added)
    for (const auto& [p, e2] : m2) {
        if (m1.find(p) == m1.end()) {
            DiffItem item;
            item.path = p;
            item.kind = DiffKind::Added;
            item.size2 = e2->header.unp_size;
            item.crc2 = e2->header.data_crc32;
            item.method2 = e2->header.method;
            item.mtime2 = e2->header.mtime_win;
            items.push_back(item);
            count_added++;
        }
    }

    // Sort items alphabetically by path
    std::sort(items.begin(), items.end(),
              [](const DiffItem& a, const DiffItem& b) { return a.path < b.path; });

    const bool has_differences =
        (count_added > 0 || count_deleted > 0 || count_modified > 0 || count_metadata > 0);

    if (opt.json) {
        std::cout << "{\n"
                  << "  \"command\": \"diff\",\n"
                  << "  \"version\": \"2.0.0\",\n"
                  << "  \"archive1\": \"" << escape_json_string(opt.arc1_path) << "\",\n"
                  << "  \"archive2\": \"" << escape_json_string(opt.arc2_path) << "\",\n"
                  << "  \"identical\": " << (has_differences ? "false" : "true") << ",\n"
                  << "  \"summary\": {\n"
                  << "    \"added\": " << count_added << ",\n"
                  << "    \"deleted\": " << count_deleted << ",\n"
                  << "    \"modified\": " << count_modified << ",\n"
                  << "    \"metadata_diff\": " << count_metadata << ",\n"
                  << "    \"identical\": " << count_identical << "\n"
                  << "  },\n"
                  << "  \"differences\": [\n";
        bool first = true;
        for (const auto& item : items) {
            if (item.kind == DiffKind::Identical && !opt.verbose) continue;
            if (!first) std::cout << ",\n";
            first = false;

            const char* tag = "identical";
            if (item.kind == DiffKind::Added)
                tag = "added";
            else if (item.kind == DiffKind::Deleted)
                tag = "deleted";
            else if (item.kind == DiffKind::Modified)
                tag = "modified";
            else if (item.kind == DiffKind::Metadata)
                tag = "metadata_diff";

            std::cout << "    {\n"
                      << "      \"path\": \"" << escape_json_string(item.path) << "\",\n"
                      << "      \"type\": \"" << tag << "\",\n"
                      << "      \"size1\": " << item.size1 << ",\n"
                      << "      \"size2\": " << item.size2 << ",\n"
                      << "      \"crc1\": \"" << format_hex_crc(item.crc1) << "\",\n"
                      << "      \"crc2\": \"" << format_hex_crc(item.crc2) << "\"\n"
                      << "    }";
        }
        std::cout << "\n  ]\n}\n";
        return has_differences ? 1 : 0;
    }

    // Terminal side-by-side / unified table output
    std::cout
        << "\n  \033[1;36mOpenRAR 2.0.0 In-Archive Structural Diff\033[0m\n"
        << "  \033[90m==================================================================\033[0m\n"
        << "  Archive 1 (-): \033[1;37m" << opt.arc1_path << "\033[0m\n"
        << "  Archive 2 (+): \033[1;37m" << opt.arc2_path << "\033[0m\n\n";

    if (!has_differences) {
        std::cout << "  \033[1;32m[MATCH] Archives are completely identical (" << count_identical
                  << " files verified)\033[0m\n"
                  << "  "
                     "\033[90m=================================================================="
                     "\033[0m\n\n";
        return 0; // Directive 3: POSIX 0
    }

    std::cout
        << "  Tag  File Path                           Size (A1 -> A2)       CRC32 (A1 -> A2)\n"
        << "  ---  ----------------------------------  --------------------  -----------------\n";

    for (const auto& item : items) {
        if (item.kind == DiffKind::Identical && !opt.verbose) continue;

        const std::string disp_path = sanitize_for_display(item.path);
        const std::string path_field = truncate_utf8(disp_path, 34);

        if (item.kind == DiffKind::Added) {
            std::cout << "  \033[1;32m+   \033[0m " << std::left << std::setw(34) << path_field
                      << "  " << std::right << std::setw(20)
                      << ("+ " + std::to_string(item.size2) + " B") << "  " << std::setw(17)
                      << ("+ " + format_hex_crc(item.crc2)) << "\n";
        } else if (item.kind == DiffKind::Deleted) {
            std::cout << "  \033[1;31m-   \033[0m " << std::left << std::setw(34) << path_field
                      << "  " << std::right << std::setw(20)
                      << ("- " + std::to_string(item.size1) + " B") << "  " << std::setw(17)
                      << ("- " + format_hex_crc(item.crc1)) << "\n";
        } else if (item.kind == DiffKind::Modified) {
            std::string s_str = std::to_string(item.size1) + " -> " + std::to_string(item.size2);
            std::string c_str = format_hex_crc(item.crc1) + "->" + format_hex_crc(item.crc2);
            std::cout << "  \033[1;33m~   \033[0m " << std::left << std::setw(34) << path_field
                      << "  " << std::right << std::setw(20) << s_str << "  " << std::setw(17)
                      << c_str << "\n";
        } else if (item.kind == DiffKind::Metadata) {
            std::cout << "  \033[1;35m*   \033[0m " << std::left << std::setw(34) << path_field
                      << "  " << std::right << std::setw(20) << (std::to_string(item.size1) + " B")
                      << "  " << std::setw(17) << "[attr/mtime drift]\n";
        } else if (item.kind == DiffKind::Identical && opt.verbose) {
            std::cout << "  \033[90m=    " << std::left << std::setw(34) << path_field << "  "
                      << std::right << std::setw(20) << (std::to_string(item.size1) + " B") << "  "
                      << std::setw(17) << format_hex_crc(item.crc1) << "\033[0m\n";
        }
    }

    std::cout
        << "\n  Summary: \033[1;32m" << count_added << " added\033[0m, \033[1;31m" << count_deleted
        << " deleted\033[0m, \033[1;33m" << count_modified << " modified\033[0m, \033[1;35m"
        << count_metadata << " metadata diff\033[0m, " << count_identical << " identical\n"
        << "  "
           "\033[90m==================================================================\033[0m\n\n";

    return 1; // Directive 3: POSIX 1 (differences detected)
}

} // namespace openrar::cli
