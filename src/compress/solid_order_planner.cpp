#include "solid_order_planner.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace openrar::compress {

namespace {

std::string to_lower_ascii(std::string_view sv) {
    std::string s;
    s.reserve(sv.size());
    for (char c : sv) {
        s.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    }
    return s;
}

std::string_view get_filename(std::string_view path) {
    size_t pos = path.find_last_of("/\\");
    if (pos == std::string_view::npos) return path;
    return path.substr(pos + 1);
}

std::string get_extension_lower(std::string_view path) {
    std::string_view fname = get_filename(path);
    size_t dot = fname.find_last_of('.');
    if (dot == std::string_view::npos || dot == 0) return "";
    return to_lower_ascii(fname.substr(dot));
}

} // namespace

ContentCategory classify_content_category(const std::string& entry_name, bool is_dir) {
    if (is_dir) return ContentCategory::Directory;

    std::string_view fname = get_filename(entry_name);
    std::string fname_lower = to_lower_ascii(fname);

    // Special exact filename matches
    if (fname_lower == "cmakelists.txt" || fname_lower == "makefile" ||
        fname_lower == "gnumakefile" || fname_lower == "meson.build" ||
        fname_lower == "build.ninja") {
        return ContentCategory::BuildProject;
    }
    if (fname_lower == "dockerfile" || fname_lower == "containerfile" || fname_lower == "gemfile" ||
        fname_lower == "rakefile") {
        return ContentCategory::BuildProject;
    }
    if (fname_lower == "license" || fname_lower == "copying" || fname_lower == "authors" ||
        fname_lower == "contributors" || fname_lower == "readme" || fname_lower == "changelog" ||
        fname_lower == "news" || fname_lower == "todo") {
        return ContentCategory::Documentation;
    }

    std::string ext = get_extension_lower(entry_name);
    if (ext.empty()) {
        // Dotfiles like .gitignore, .env, .editorconfig
        if (!fname_lower.empty() && fname_lower[0] == '.') {
            return ContentCategory::DataConfig;
        }
        return ContentCategory::Other;
    }

    // Header Code
    if (ext == ".h" || ext == ".hpp" || ext == ".hxx" || ext == ".hh" || ext == ".h++" ||
        ext == ".inl" || ext == ".inc" || ext == ".ipp" || ext == ".tpp") {
        return ContentCategory::HeaderCode;
    }

    // C/C++ Source Code
    if (ext == ".c" || ext == ".cpp" || ext == ".cc" || ext == ".cxx" || ext == ".c++" ||
        ext == ".m" || ext == ".mm") {
        return ContentCategory::SourceCode;
    }

    // Systems Code
    if (ext == ".rs" || ext == ".go" || ext == ".zig" || ext == ".d" || ext == ".swift" ||
        ext == ".kt" || ext == ".kts" || ext == ".java" || ext == ".cs" || ext == ".scala" ||
        ext == ".groovy") {
        return ContentCategory::SystemsCode;
    }

    // Scripting Code
    if (ext == ".py" || ext == ".pyw" || ext == ".rb" || ext == ".php" || ext == ".js" ||
        ext == ".mjs" || ext == ".cjs" || ext == ".ts" || ext == ".tsx" || ext == ".jsx" ||
        ext == ".vue" || ext == ".svelte" || ext == ".lua" || ext == ".pl" || ext == ".pm" ||
        ext == ".t" || ext == ".sh" || ext == ".bash" || ext == ".zsh" || ext == ".ps1" ||
        ext == ".psm1" || ext == ".bat" || ext == ".cmd") {
        return ContentCategory::ScriptCode;
    }

    // Markup & Web
    if (ext == ".html" || ext == ".htm" || ext == ".xhtml" || ext == ".xml" || ext == ".xsl" ||
        ext == ".xslt" || ext == ".svg" || ext == ".css" || ext == ".scss" || ext == ".sass" ||
        ext == ".less") {
        return ContentCategory::MarkupWeb;
    }

    // Structured Data & Config
    if (ext == ".json" || ext == ".jsonc" || ext == ".json5" || ext == ".yaml" || ext == ".yml" ||
        ext == ".toml" || ext == ".ini" || ext == ".conf" || ext == ".cfg" ||
        ext == ".properties" || ext == ".env" || ext == ".sql" || ext == ".graphql" ||
        ext == ".proto" || ext == ".csv" || ext == ".tsv" || ext == ".plist") {
        return ContentCategory::DataConfig;
    }

    // Documentation & Text
    if (ext == ".txt" || ext == ".md" || ext == ".markdown" || ext == ".rst" || ext == ".adoc" ||
        ext == ".asciidoc" || ext == ".tex" || ext == ".rtf" || ext == ".log" || ext == ".pdf") {
        return ContentCategory::Documentation;
    }

    // Build & Project files
    if (ext == ".cmake" || ext == ".ninja" || ext == ".make" || ext == ".mak" || ext == ".mk" ||
        ext == ".sln" || ext == ".vcxproj" || ext == ".csproj" || ext == ".gradle" ||
        ext == ".props" || ext == ".targets") {
        return ContentCategory::BuildProject;
    }

    // Executable Binaries
    if (ext == ".exe" || ext == ".dll" || ext == ".so" || ext == ".dylib" || ext == ".sys" ||
        ext == ".bin" || ext == ".elf" || ext == ".wasm") {
        return ContentCategory::CompiledBinary;
    }

    // Object Files & Libraries
    if (ext == ".o" || ext == ".obj" || ext == ".a" || ext == ".lib" || ext == ".pdb" ||
        ext == ".map" || ext == ".sym" || ext == ".class" || ext == ".pyc") {
        return ContentCategory::ObjectFile;
    }

    // Uncompressed Media
    if (ext == ".wav" || ext == ".aiff" || ext == ".au" || ext == ".pcm" || ext == ".bmp" ||
        ext == ".dib" || ext == ".tga" || ext == ".ppm" || ext == ".pgm" || ext == ".pbm") {
        return ContentCategory::UncompressedMedia;
    }

    // Compressed Media
    if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".webp" ||
        ext == ".ico" || ext == ".tiff" || ext == ".tif" || ext == ".mp3" || ext == ".ogg" ||
        ext == ".flac" || ext == ".aac" || ext == ".m4a" || ext == ".wma" || ext == ".opus" ||
        ext == ".mp4" || ext == ".mkv" || ext == ".avi" || ext == ".mov" || ext == ".wmv" ||
        ext == ".webm" || ext == ".flv" || ext == ".psd") {
        return ContentCategory::CompressedMedia;
    }

    // Compressed Archives
    if (ext == ".zip" || ext == ".rar" || ext == ".7z" || ext == ".tar" || ext == ".gz" ||
        ext == ".bz2" || ext == ".xz" || ext == ".zst" || ext == ".tgz" || ext == ".jar" ||
        ext == ".war" || ext == ".iso") {
        return ContentCategory::CompressedArchive;
    }

    return ContentCategory::Other;
}

SolidPlanResult plan_solid_order(const std::vector<SolidOrderItem>& items,
                                 const std::vector<size_t>& reference_of) {
    const size_t n = items.size();
    SolidPlanResult result;
    result.order.resize(n);
    if (n == 0) return result;

    // Track master -> references relationship.
    // If reference_of is provided and entry i has reference_of[i] = master (< n),
    // entry i is attached to master and placed immediately after it.
    std::vector<std::vector<size_t>> references_for(n);
    std::vector<bool> is_reference(n, false);
    if (!reference_of.empty()) {
        for (size_t i = 0; i < n; ++i) {
            if (i < reference_of.size()) {
                size_t master = reference_of[i];
                if (master < n && master != i) {
                    references_for[master].push_back(i);
                    is_reference[i] = true;
                }
            }
        }
    }

    // Descriptor for standalone items to sort
    struct StandaloneSortKey {
        size_t orig_idx;
        bool is_dir_entry;
        std::string dir_lower;
        ContentCategory category;
        std::string ext;
        std::string fname_lower;
    };

    std::vector<StandaloneSortKey> standalones;
    standalones.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        if (is_reference[i]) continue;
        bool is_dir = items[i].is_dir;
        ContentCategory cat = classify_content_category(items[i].entry_name, is_dir);
        std::string ext = get_extension_lower(items[i].entry_name);

        std::string_view p = items[i].entry_name;
        size_t last_slash = p.find_last_of("/\\");
        std::string dir_lower =
            (last_slash != std::string_view::npos) ? to_lower_ascii(p.substr(0, last_slash)) : "";
        std::string fname_lower = to_lower_ascii(get_filename(p));

        standalones.push_back(
            {i, is_dir, std::move(dir_lower), cat, std::move(ext), std::move(fname_lower)});
    }

    // Directory-affine heuristic solid grouping:
    // 1. Directory entries first (extracted before their children)
    // 2. Directory path lowercase ascending (preserves subsystem/module locality)
    // 3. ContentCategory ascending within the directory (headers immediately precede source)
    // 4. Extension lowercase ascending
    // 5. Filename lowercase ascending
    // 6. Original index ascending (stable tie-break)
    std::sort(standalones.begin(), standalones.end(),
              [](const StandaloneSortKey& a, const StandaloneSortKey& b) {
                  if (a.is_dir_entry != b.is_dir_entry) return a.is_dir_entry > b.is_dir_entry;
                  if (a.dir_lower != b.dir_lower) return a.dir_lower < b.dir_lower;
                  if (a.category != b.category) return a.category < b.category;
                  if (a.ext != b.ext) return a.ext < b.ext;
                  if (a.fname_lower != b.fname_lower) return a.fname_lower < b.fname_lower;
                  return a.orig_idx < b.orig_idx;
              });

    // Assemble final order: standalone items with their attached references
    size_t k = 0;
    std::vector<bool> placed(n, false);

    for (const auto& sk : standalones) {
        result.order[k++] = sk.orig_idx;
        placed[sk.orig_idx] = true;

        for (size_t ref_idx : references_for[sk.orig_idx]) {
            if (!placed[ref_idx]) {
                result.order[k++] = ref_idx;
                placed[ref_idx] = true;
            }
        }
    }

    // Defensive fallback: if any items were somehow missed (e.g. cycle in references),
    // append them in original index order.
    for (size_t i = 0; i < n; ++i) {
        if (!placed[i]) {
            result.order[k++] = i;
            placed[i] = true;
        }
    }

    result.reordered = false;
    for (size_t i = 0; i < n; ++i) {
        if (result.order[i] != i) {
            result.reordered = true;
            break;
        }
    }

    return result;
}

} // namespace openrar::compress
