// Diagnostic probe: walk an archive with format::HeaderReader::read_block_raw
// (the same primitive the scanner uses) and print every block — used to
// root-cause the WinRAR-7.20 zero-compressed-entry walk drop.
#include "../../src/format/header_reader.hpp"
#include "../../src/io/file_stream.hpp"

#include <cstring>
#include <iostream>
#include <string>

using namespace openrar;

int main(int argc, char** argv) {
    if (argc != 2) return 1;
    io::FileStream in;
    if (!in.open(argv[1], io::FileMode::ReadOnly)) return 1;
    // skip signature
    in.seek(8, io::SeekOrigin::Begin);
    int idx = 0;
    for (;;) {
        core::uint64 type = 0, flags = 0, data_size = 0;
        std::vector<core::byte> body;
        const auto pos = in.tell();
        const auto r = format::HeaderReader::read_block_raw(in, type, flags, body, data_size);
        if (r == format::HeaderResult::Eof) {
            std::cout << idx << " EOF at " << pos << "\n";
            break;
        }
        std::string detail;
        core::uint64 fflags = 0, usize = 0, nlen = 0;
        if (r == format::HeaderResult::Ok && type == 2 && body.size() > 6) {
            // file body: [file_flags vint][unp vint][name_size vint][name]
            size_t p = 0;
            auto rv = [&](core::uint64& out) {
                core::uint64 v = 0;
                int shift = 0;
                while (p < body.size()) {
                    const core::uint8 x = static_cast<uint8_t>(body[p++]);
                    v |= static_cast<core::uint64>(x & 0x7F) << shift;
                    if (!(x & 0x80)) break;
                    shift += 7;
                }
                out = v;
            };
            rv(fflags);
            rv(usize);
            rv(nlen);
            const size_t name_at = p;
            if (name_at + nlen <= body.size())
                detail = std::string(reinterpret_cast<const char*>(body.data()) + name_at, nlen);
        }
        std::cout << idx << " @" << pos << " result=" << static_cast<int>(r) << " type=" << type
                  << " flags=" << flags << " data_size=" << data_size << " fflags=" << fflags
                  << " unp=" << usize << " nlen=" << nlen << " " << detail << "\n";
        if (r != format::HeaderResult::Ok) break;
        if (type == 5) break;
        // skip data area to keep the walk aligned
        if (data_size > 0 && data_size < (1ull << 40))
            in.seek(static_cast<core::int64>(data_size), io::SeekOrigin::Current);
        ++idx;
        if (idx > 200) {
            std::cout << "cap reached\n";
            break;
        }
    }
    return 0;
}
