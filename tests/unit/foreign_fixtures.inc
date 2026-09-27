// Shared foreign-format fixture builders (v1.29 M2/M3/M5): ZIP / TAR / GZIP.
// Included INSIDE an anonymous namespace by each suite after the foreign
// reader includes and using-declarations.

void put16(std::vector<core::byte>& b, core::uint16 v) {
    b.push_back(static_cast<core::byte>(v & 0xFF));
    b.push_back(static_cast<core::byte>(v >> 8));
}
void put32(std::vector<core::byte>& b, core::uint32 v) {
    put16(b, static_cast<core::uint16>(v & 0xFFFF));
    put16(b, static_cast<core::uint16>(v >> 16));
}
void put64(std::vector<core::byte>& b, core::uint64 v) {
    put32(b, static_cast<core::uint32>(v & 0xFFFFFFFFu));
    put32(b, static_cast<core::uint32>(v >> 32));
}
void put_str(std::vector<core::byte>& b, const std::string& s) {
    b.insert(b.end(), s.begin(), s.end());
}

struct ZipMember {
    std::string name;
    core::uint16 method = 0;
    core::uint16 flags = 0;         // GP flags (bit 3 descriptor, bit 11 utf-8, bit 0 encrypted)
    std::vector<core::byte> packed; // packed payload bytes
    core::uint32 crc = 0;           // CRC32 of the UNPACKED payload
    core::uint64 unp_size = 0;
    // Mismatch injections (default: none — consistent archive):
    bool lfh_name_mismatch = false;
    bool lfh_method_mismatch = false;
    bool lfh_size_mismatch = false;
    bool lfh_crc_mismatch = false;
    bool cd_offset_mismatch = false; // CDH points at the wrong LFH offset
    bool zip64 = false;              // 0xFFFFFFFF placeholders + ZIP64 extra
    std::string entry_comment;
    core::uint32 dos_attrs = 0;
    core::uint32 unix_mode = 0; // external attrs high 16 bits
    bool is_dir = false;
};

// Builds a complete ZIP (LFHs + data + CD + EOCD). With no injections the
// archive is fully consistent.
std::vector<core::byte> build_zip(const std::vector<ZipMember>& members,
                                  const std::string& archive_comment = {},
                                  size_t prepend_garbage = 0) {
    std::vector<core::byte> out;
    if (prepend_garbage) out.assign(prepend_garbage, 0x41);

    struct Done {
        core::uint64 lfh_off;
        core::uint64 data_off;
        const ZipMember* m;
    };
    std::vector<Done> done;

    for (const ZipMember& m : members) {
        const core::uint32 unp32 = m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.unp_size);
        const core::uint32 pack32 =
            m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.packed.size());
        const std::string lfh_name =
            m.lfh_name_mismatch ? std::string("Z") + m.name.substr(1) : m.name;
        const core::uint16 name_len = static_cast<core::uint16>(lfh_name.size());
        std::vector<core::byte> lfh;
        put32(lfh, 0x04034b50);
        put16(lfh, 20); // version needed
        put16(lfh, m.flags);
        put16(lfh, m.method);
        put16(lfh, 0x6000); // DOS time 12:00:00
        put16(lfh, 0x5821); // DOS date 2024-01-01
        put32(lfh, m.lfh_crc_mismatch ? m.crc ^ 0xFFFF : m.crc);
        put32(lfh, pack32);
        put32(lfh, m.lfh_size_mismatch ? static_cast<core::uint32>(m.unp_size + 7) : unp32);
        put16(lfh, name_len);
        put16(lfh, m.zip64 ? 20 : 0); // LFH extra len (ZIP64 size fields ride here)
        put_str(lfh, lfh_name);
        if (m.zip64) {
            put16(lfh, 0x0001);
            put16(lfh, 16);
            put64(lfh, m.unp_size);
            put64(lfh, m.packed.size());
        }
        done.push_back({out.size(), out.size() + lfh.size(), &m});
        out.insert(out.end(), lfh.begin(), lfh.end());
        out.insert(out.end(), m.packed.begin(), m.packed.end());
    }

    const core::uint64 cd_offset = out.size();
    for (const Done& d : done) {
        const ZipMember& m = *d.m;
        std::vector<core::byte> cdh;
        put32(cdh, 0x02014b50);
        put16(cdh, 0x031E); // made by: unix, 3.0
        put16(cdh, 20);
        put16(cdh, m.flags);
        put16(cdh, m.lfh_method_mismatch ? static_cast<core::uint16>(m.method ^ 0xFF) : m.method);
        put16(cdh, 0x6000);
        put16(cdh, 0x5821);
        put32(cdh, m.crc);
        put32(cdh, m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.packed.size()));
        put32(cdh, m.zip64 ? 0xFFFFFFFFu : static_cast<core::uint32>(m.unp_size));
        put16(cdh, static_cast<core::uint16>(m.name.size()));
        const bool zip64_extra = m.zip64;
        put16(cdh, zip64_extra ? 20 : 0); // extra len
        put16(cdh, static_cast<core::uint16>(m.entry_comment.size()));
        put16(cdh, 0); // disk start
        put16(cdh, 0); // internal attrs
        put32(cdh, m.dos_attrs | (m.unix_mode << 16));
        // CDH LFH offset: data offset (the LFH header start); injected
        // mismatch points at the next member's area instead.
        const core::uint64 off = d.lfh_off;
        put32(cdh, m.cd_offset_mismatch ? static_cast<core::uint32>(off + 3)
                                        : static_cast<core::uint32>(off));
        put_str(cdh, m.name);
        if (zip64_extra) {
            put16(cdh, 0x0001);
            put16(cdh, 16);
            put64(cdh, m.unp_size);
            put64(cdh, m.packed.size());
        }
        put_str(cdh, m.entry_comment);
        out.insert(out.end(), cdh.begin(), cdh.end());
    }
    const core::uint64 cd_size = out.size() - cd_offset;

    put32(out, 0x06054b50);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<core::uint16>(members.size()));
    put16(out, static_cast<core::uint16>(members.size()));
    put32(out, static_cast<core::uint32>(cd_size));
    put32(out, static_cast<core::uint32>(cd_offset));
    put16(out, static_cast<core::uint16>(archive_comment.size()));
    put_str(out, archive_comment);
    return out;
}

std::vector<core::byte> stored_payload(const std::string& s, core::uint32& crc_out) {
    crypto::Crc32 crc;
    crc.update(s.data(), s.size());
    crc_out = crc.get();
    return std::vector<core::byte>(s.begin(), s.end());
}

ForeignStatus open_zip(const std::filesystem::path& p, ZipReader& r, size_t* count = nullptr) {
    ExtractionLimits limits;
    r = ZipReader{};
    std::string detail;
    const ForeignStatus st = r.open(p, limits, ReaderHooks{}, detail);
    if (st == ForeignStatus::Ok && count) *count = r.entry_count();
    return st;
}


struct TarMember {
    std::string name;
    std::string linkname;
    char typeflag = '0';
    std::vector<core::byte> data;
    core::uint32 mode = 0644;
    core::uint64 mtime = 1704110400;
    std::string uname = "tester";
    std::string gname = "tgroup";
    core::uint64 uid = 1000;
    core::uint64 gid = 1000;
    std::vector<std::pair<std::string, std::string>> pax; // emits 'x' first
    bool use_longname = false;                            // GNU 'L' for the name
};

void put_octal(std::vector<core::byte>& b, size_t field, core::uint64 v, size_t width) {
    char buf[32] = {0};
    for (size_t i = 0; i < width; ++i) b[field + i] = static_cast<core::byte>(' ');
    snprintf(buf, sizeof(buf), "%0*llo", static_cast<int>(width - 1),
             static_cast<unsigned long long>(v));
    for (size_t i = 0; i < width - 1 && buf[i]; ++i) b[field + i] = static_cast<core::byte>(buf[i]);
}

std::vector<core::byte> tar_header(const std::string& name, char typeflag, core::uint64 size,
                                   core::uint32 mode, core::uint64 mtime,
                                   const std::string& linkname, const std::string& uname,
                                   const std::string& gname, core::uint64 uid, core::uint64 gid,
                                   const std::string& prefix = {}) {
    std::vector<core::byte> h(512, core::byte(0));
    const auto copy_field = [&](size_t off, const std::string& s, size_t max) {
        const size_t n = s.size() < max ? s.size() : max;
        std::memcpy(h.data() + off, s.data(), n);
    };
    copy_field(0, name, 100);
    put_octal(h, 100, mode, 8);
    put_octal(h, 108, uid, 8);
    put_octal(h, 116, gid, 8);
    put_octal(h, 124, size, 12);
    put_octal(h, 136, mtime, 12);
    std::memcpy(h.data() + 148, "        ", 8); // checksum placeholder
    h[156] = static_cast<core::byte>(typeflag);
    copy_field(157, linkname, 100);
    std::memcpy(h.data() + 257, "ustar\0", 6);
    std::memcpy(h.data() + 263, "00", 2);
    copy_field(265, uname, 32);
    copy_field(297, gname, 32);
    copy_field(345, prefix, 155);
    core::uint32 sum = 0;
    for (size_t i = 0; i < 512; ++i) sum += static_cast<core::uint32>(h[i]);
    char cs[8] = {0};
    snprintf(cs, sizeof(cs), "%06o", sum);
    std::memcpy(h.data() + 148, cs, 6);
    h[154] = core::byte(0);
    h[155] = static_cast<core::byte>(' ');
    return h;
}

std::vector<core::byte> build_tar(const std::vector<TarMember>& members, bool end_blocks = true) {
    std::vector<core::byte> out;
    for (const TarMember& m : members) {
        if (!m.pax.empty()) {
            std::vector<core::byte> recs;
            for (const auto& kv : m.pax) {
                std::string rec = kv.first + "=" + kv.second + "\n";
                size_t total = rec.size() + std::to_string(rec.size()).size() + 1;
                while (std::to_string(total).size() + 1 + rec.size() != total)
                    total = std::to_string(total).size() + 1 + rec.size();
                std::string full = std::to_string(total) + " " + rec;
                recs.insert(recs.end(), full.begin(), full.end());
            }
            const size_t pad = (512 - recs.size() % 512) % 512;
            recs.resize(recs.size() + pad, core::byte(0));
            auto h =
                tar_header("PaxHeaders/x", 'x', recs.size(), 0644, 0, "", "root", "root", 0, 0);
            out.insert(out.end(), h.begin(), h.end());
            out.insert(out.end(), recs.begin(), recs.end());
        }
        if (m.use_longname) {
            std::vector<core::byte> lname(m.name.begin(), m.name.end());
            lname.push_back(core::byte(0));
            const size_t pad = (512 - lname.size() % 512) % 512;
            lname.resize(lname.size() + pad, core::byte(0));
            auto h =
                tar_header("././@LongLink", 'L', lname.size(), 0644, 0, "", "root", "root", 0, 0);
            out.insert(out.end(), h.begin(), h.end());
            out.insert(out.end(), lname.begin(), lname.end());
        }
        std::string stored_name = m.use_longname ? std::string("longlink-target") : m.name;
        std::string prefix;
        if (!m.use_longname && m.name.size() > 100) {
            const size_t slash = m.name.rfind('/', 154);
            if (slash != std::string::npos) {
                prefix = m.name.substr(0, slash);
                stored_name = m.name.substr(slash + 1);
            }
        }
        auto h = tar_header(stored_name, m.typeflag, m.data.size(), m.mode, m.mtime, m.linkname,
                            m.uname, m.gname, m.uid, m.gid, prefix);
        out.insert(out.end(), h.begin(), h.end());
        out.insert(out.end(), m.data.begin(), m.data.end());
        const size_t pad = (512 - m.data.size() % 512) % 512;
        out.resize(out.size() + pad, core::byte(0));
    }
    if (end_blocks) out.resize(out.size() + 1024, core::byte(0));
    return out;
}

bool write_file(const std::filesystem::path& p, const std::vector<core::byte>& data) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return static_cast<bool>(f);
}

std::vector<core::byte> build_gzip(const std::vector<const std::vector<core::byte>*>& streams,
                                   const std::vector<core::uint32>& crcs,
                                   const std::vector<core::uint32>& isizes,
                                   const std::string& fname = "", bool corrupt_trailer = false) {
    std::vector<core::byte> out;
    auto put16le = [&](core::uint16 v) {
        out.push_back(static_cast<core::byte>(v & 0xFF));
        out.push_back(static_cast<core::byte>(v >> 8));
    };
    auto put32le = [&](core::uint32 v) {
        put16le(static_cast<core::uint16>(v & 0xFFFF));
        put16le(static_cast<core::uint16>(v >> 16));
    };
    std::string first_fname = fname;
    for (size_t i = 0; i < streams.size(); ++i) {
        out.push_back(0x1F);
        out.push_back(0x8B);
        out.push_back(8); // CM = deflate
        const core::uint8 flg = (i == 0 && !first_fname.empty()) ? 0x08 : 0x00;
        out.push_back(static_cast<core::byte>(flg));
        put32le(1704110400); // MTIME
        out.push_back(0);    // XFL
        out.push_back(3);    // OS = unix
        if (flg & 0x08) {
            out.insert(out.end(), first_fname.begin(), first_fname.end());
            out.push_back(0);
        }
        out.insert(out.end(), streams[i]->begin(), streams[i]->end());
        put32le(corrupt_trailer ? crcs[i] ^ 0xFFFFFFFFu : crcs[i]);
        put32le(isizes[i]);
    }
    return out;
}

