#ifndef OPENRAR_COMPRESS_SOLID_ORDER_PLANNER_HPP
#define OPENRAR_COMPRESS_SOLID_ORDER_PLANNER_HPP

#include "../core/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace openrar::compress {

// Semantic content category for heuristic solid packing.
// Ordering defines the coarse packing sequence:
//  0. Directories (created/extracted first)
//  1. Header Code (definitions & interface declarations: .h, .hpp)
//  2. C/C++ Source (implementations matching headers: .c, .cpp)
//  3. Systems Code (Rust, Go, Zig, Swift, Java, C#)
//  4. Scripting Code (Python, JS, TS, Ruby, Shell)
//  5. Markup & Web (HTML, CSS, XML, SVG)
//  6. Structured Data & Config (JSON, YAML, TOML, SQL, CSV)
//  7. Documentation & Text (Markdown, TXT, RST)
//  8. Build & Project files (CMake, Makefile, Ninja)
//  9. Executable Binaries (EXE, DLL, SO, ELF, WASM - machine code)
// 10. Object Files & Libraries (.o, .obj, .a, .lib, .pdb)
// 11. Uncompressed Media (WAV, BMP, TGA)
// 12. Compressed Media (PNG, JPG, MP3, MP4)
// 13. Compressed Archives (ZIP, 7Z, RAR, GZ)
// 14. Other / Unclassified
enum class ContentCategory : core::uint32 {
    Directory = 0,
    HeaderCode = 1,
    SourceCode = 2,
    SystemsCode = 3,
    ScriptCode = 4,
    MarkupWeb = 5,
    DataConfig = 6,
    Documentation = 7,
    BuildProject = 8,
    CompiledBinary = 9,
    ObjectFile = 10,
    UncompressedMedia = 11,
    CompressedMedia = 12,
    CompressedArchive = 13,
    Other = 14
};

// Input descriptor for one queue item to be ordered.
struct SolidOrderItem {
    std::string entry_name;
    core::uint64 file_size{0};
    bool is_dir{false};
};

struct SolidPlanResult {
    std::vector<size_t> order; // Permutation of 0..N-1
    bool reordered{false};     // True if order != identity
};

// Classifies a file entry (by extension and directory flag) into a ContentCategory.
ContentCategory classify_content_category(const std::string& entry_name, bool is_dir);

// Plans the solid packing order for the given items.
// If reference_of is non-empty, for any index i where reference_of[i] != SIZE_MAX,
// item i (a FILECOPY or hardlink reference) will be placed immediately after its master,
// ensuring the reference never precedes its target.
SolidPlanResult plan_solid_order(const std::vector<SolidOrderItem>& items,
                                 const std::vector<size_t>& reference_of = {});

} // namespace openrar::compress

#endif // OPENRAR_COMPRESS_SOLID_ORDER_PLANNER_HPP
