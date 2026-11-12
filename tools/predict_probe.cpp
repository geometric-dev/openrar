// Wave 1b Gate 0 tool: image/audio channel predictor and correlation evaluation.
// Evaluates actual type-0 DELTA transform against baseline and upper-bound 2D predictors.

#include "../src/compress/filters50.hpp"
#include "../src/core/types.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;
using namespace openrar;

struct MetricResult {
    double entropy{0.0};
    double lz_friendliness{0.0};
    std::vector<double> plane_entropies;
};

// Compute Shannon entropy in bits per byte over a byte buffer
static double compute_entropy(const core::byte* data, size_t size) {
    if (size == 0) return 0.0;
    size_t counts[256] = {0};
    for (size_t i = 0; i < size; ++i) {
        counts[data[i]]++;
    }
    double entropy = 0.0;
    const double inv_size = 1.0 / static_cast<double>(size);
    for (size_t i = 0; i < 256; ++i) {
        if (counts[i] > 0) {
            double p = counts[i] * inv_size;
            entropy -= p * std::log2(p);
        }
    }
    return entropy;
}

// Compute fraction of 8-byte windows repeated within 4 MiB sliding window
static double compute_lz_friendliness(const core::byte* data, size_t size) {
    if (size < 8) return 0.0;
    const size_t total_windows = size - 7;
    size_t matched_windows = 0;
    constexpr size_t WINDOW_SIZE = 4 * 1024 * 1024;

    std::unordered_map<core::uint64, size_t> last_seen;
    last_seen.reserve(std::min<size_t>(total_windows, 1048576));

    for (size_t i = 0; i < total_windows; ++i) {
        core::uint64 w = core::read_le64(data + i);
        auto it = last_seen.find(w);
        if (it != last_seen.end()) {
            if ((i - it->second) <= WINDOW_SIZE) {
                matched_windows++;
            }
            it->second = i;
        } else {
            last_seen.emplace(w, i);
        }
    }
    return static_cast<double>(matched_windows) / static_cast<double>(total_windows);
}

struct ImageGeometry {
    bool is_image{false};
    std::string format;
    size_t width{0};
    size_t height{0};
    size_t bpp{0};
    size_t bytes_per_pixel{0};
    size_t row_stride{0};
    size_t pixel_offset{0};
};

static ImageGeometry parse_image_geometry(const std::vector<core::byte>& data) {
    ImageGeometry g;
    if (data.size() < 18) return g;

    // 1. BMP detection
    if (data.size() >= 54 && data[0] == 'B' && data[1] == 'M') {
        core::uint32 off_bits = core::read_le32(data.data() + 10);
        core::uint32 bi_size = core::read_le32(data.data() + 14);
        if (bi_size >= 40 && off_bits < data.size()) {
            g.is_image = true;
            g.format = "BMP";
            g.width = core::read_le32(data.data() + 18);
            core::int32 h = static_cast<core::int32>(core::read_le32(data.data() + 22));
            g.height = std::abs(h);
            g.bpp = core::read_le16(data.data() + 28);
            g.bytes_per_pixel = std::max<size_t>(1, (g.bpp + 7) / 8);
            g.row_stride = ((g.width * g.bpp + 31) / 32) * 4;
            g.pixel_offset = off_bits;
            return g;
        }
    }

    // 2. PPM / PGM
    if (data[0] == 'P' && (data[1] == '6' || data[1] == '5')) {
        bool is_p6 = (data[1] == '6');
        size_t idx = 2;
        auto skip_ws_comments = [&]() {
            while (idx < data.size()) {
                if (data[idx] == '#') {
                    while (idx < data.size() && data[idx] != '\n') idx++;
                } else if (data[idx] <= ' ') {
                    idx++;
                } else {
                    break;
                }
            }
        };
        skip_ws_comments();
        auto read_num = [&]() -> size_t {
            size_t n = 0;
            while (idx < data.size() && data[idx] >= '0' && data[idx] <= '9') {
                n = n * 10 + (data[idx] - '0');
                idx++;
            }
            return n;
        };
        size_t w = read_num();
        skip_ws_comments();
        size_t h = read_num();
        skip_ws_comments();
        size_t maxval = read_num();
        if (idx < data.size() && (data[idx] == '\n' || data[idx] == ' ')) idx++;
        if (w > 0 && h > 0 && maxval <= 255) {
            g.is_image = true;
            g.format = is_p6 ? "PPM" : "PGM";
            g.width = w;
            g.height = h;
            g.bpp = is_p6 ? 24 : 8;
            g.bytes_per_pixel = is_p6 ? 3 : 1;
            g.row_stride = w * g.bytes_per_pixel;
            g.pixel_offset = idx;
            return g;
        }
    }

    // 3. TGA type 2
    if (data.size() >= 18 && data[2] == 2) {
        size_t w = core::read_le16(data.data() + 12);
        size_t h = core::read_le16(data.data() + 14);
        size_t bpp = data[16];
        if (w > 0 && h > 0 && (bpp == 24 || bpp == 32 || bpp == 16 || bpp == 8)) {
            g.is_image = true;
            g.format = "TGA";
            g.width = w;
            g.height = h;
            g.bpp = bpp;
            g.bytes_per_pixel = std::max<size_t>(1, bpp / 8);
            g.row_stride = w * g.bytes_per_pixel;
            g.pixel_offset = 18 + data[0]; // id field length
            return g;
        }
    }

    // 4. TIFF uncompressed
    if (data.size() >= 8 &&
        ((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))) {
        bool is_le = (data[0] == 'I');
        auto r16 = [&](size_t off) {
            return is_le ? core::read_le16(data.data() + off)
                         : core::read_be32(data.data() + off - 2) & 0xFFFF;
        };
        auto r32 = [&](size_t off) {
            return is_le ? core::read_le32(data.data() + off) : core::read_be32(data.data() + off);
        };
        core::uint32 ifd_off = r32(4);
        if (ifd_off + 2 < data.size()) {
            core::uint16 num_tags =
                is_le ? core::read_le16(data.data() + ifd_off)
                      : (static_cast<core::uint16>(data[ifd_off]) << 8 | data[ifd_off + 1]);
            size_t cur = ifd_off + 2;
            size_t w = 0, h = 0, strip_off = 0;
            for (size_t t = 0; t < num_tags && cur + 12 <= data.size(); ++t, cur += 12) {
                core::uint16 tag =
                    is_le ? core::read_le16(data.data() + cur)
                          : (static_cast<core::uint16>(data[cur]) << 8 | data[cur + 1]);
                core::uint32 val = r32(cur + 8);
                if (tag == 256)
                    w = val;
                else if (tag == 257)
                    h = val;
                else if (tag == 273)
                    strip_off = val;
            }
            if (w > 0 && h > 0 && strip_off < data.size()) {
                g.is_image = true;
                g.format = "TIFF";
                g.width = w;
                g.height = h;
                g.bpp = 24;
                g.bytes_per_pixel = 3;
                g.row_stride = w * 3;
                g.pixel_offset = strip_off;
                return g;
            }
        }
    }

    return g;
}

// Compute Up-delta predictor residual: residual[x, y] = cur[x, y] - cur[x, y-1]
static std::vector<core::byte> compute_up_residual(const core::byte* pixels, size_t /*width*/,
                                                   size_t height, size_t row_stride) {
    std::vector<core::byte> res(height * row_stride);
    for (size_t y = 0; y < height; ++y) {
        const core::byte* cur_row = pixels + y * row_stride;
        const core::byte* up_row = (y > 0) ? (pixels + (y - 1) * row_stride) : nullptr;
        core::byte* out_row = res.data() + y * row_stride;
        for (size_t x = 0; x < row_stride; ++x) {
            core::byte up = up_row ? up_row[x] : 0;
            out_row[x] = static_cast<core::byte>(cur_row[x] - up);
        }
    }
    return res;
}

// Compute Paeth predictor residual (PNG Paeth definition):
// p = a + b - c; min diff to a, b, c
static std::vector<core::byte> compute_paeth_residual(const core::byte* pixels, size_t /*width*/,
                                                      size_t height, size_t row_stride,
                                                      size_t bpp_bytes) {
    std::vector<core::byte> res(height * row_stride);
    for (size_t y = 0; y < height; ++y) {
        const core::byte* cur_row = pixels + y * row_stride;
        const core::byte* up_row = (y > 0) ? (pixels + (y - 1) * row_stride) : nullptr;
        core::byte* out_row = res.data() + y * row_stride;
        for (size_t x = 0; x < row_stride; ++x) {
            core::byte a = (x >= bpp_bytes) ? cur_row[x - bpp_bytes] : 0;
            core::byte b = up_row ? up_row[x] : 0;
            core::byte c = (up_row && x >= bpp_bytes) ? up_row[x - bpp_bytes] : 0;

            int p = static_cast<int>(a) + static_cast<int>(b) - static_cast<int>(c);
            int pa = std::abs(p - static_cast<int>(a));
            int pb = std::abs(p - static_cast<int>(b));
            int pc = std::abs(p - static_cast<int>(c));

            core::byte pr;
            if (pa <= pb && pa <= pc)
                pr = a;
            else if (pb <= pc)
                pr = b;
            else
                pr = c;

            out_row[x] = static_cast<core::byte>(cur_row[x] - pr);
        }
    }
    return res;
}

struct RowRecord {
    std::string filename;
    size_t filesize{0};
    std::string format;
    size_t width{0};
    size_t height{0};
    std::string transform;
    size_t channels{0};
    double entropy{0.0};
    double lz_friendliness_pct{0.0};
    double entropy_delta_gain{0.0};
    double lz_gain_pct{0.0};
    std::string per_plane_entropies;
    std::string note;
};

int main(int argc, char** argv) {
    std::string target_dir = "tests/fixtures/experiment";
    std::string out_csv = "predict_probe_report.csv";
    std::string out_md = "predict_probe_report.md";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--csv" && i + 1 < argc) {
            out_csv = argv[++i];
        } else if (arg == "--md" && i + 1 < argc) {
            out_md = argv[++i];
        } else if (arg[0] != '-') {
            target_dir = arg;
        }
    }

    std::cout << "OpenRAR Wave 1b PROBE-PREDICT Tool\n";
    std::cout << "Target: " << target_dir << "\n";
    std::cout << "Output CSV: " << out_csv << "\n";
    std::cout << "Output MD : " << out_md << "\n\n";

    if (!fs::exists(target_dir)) {
        std::cerr << "Error: target path does not exist: " << target_dir << "\n";
        return 1;
    }

    std::vector<fs::path> files;
    if (fs::is_directory(target_dir)) {
        for (const auto& entry : fs::directory_iterator(target_dir)) {
            if (entry.is_regular_file()) {
                std::string name = entry.path().filename().string();
                const bool is_own_output =
                    name == "predict_probe_report.csv" || name == "predict_probe_report.md";
                if (name != "manifest.sha256" && !is_own_output &&
                    (name.size() < 4 || name.rfind(".tmp") != name.size() - 4)) {
                    files.push_back(entry.path());
                }
            }
        }
        std::sort(files.begin(), files.end());
    } else {
        files.push_back(target_dir);
    }

    const size_t CANDIDATE_CHANNELS[] = {1, 2, 3, 4, 12, 16, 18, 24, 32};
    std::vector<RowRecord> records;

    for (const auto& fpath : files) {
        std::string fname = fpath.filename().string();
        std::ifstream in(fpath, std::ios::binary);
        if (!in) continue;
        std::vector<core::byte> raw((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
        if (raw.empty()) continue;

        ImageGeometry geom = parse_image_geometry(raw);

        // 1. Unfiltered baseline
        double base_entropy = compute_entropy(raw.data(), raw.size());
        double base_lz = compute_lz_friendliness(raw.data(), raw.size()) * 100.0;

        RowRecord base_rec;
        base_rec.filename = fname;
        base_rec.filesize = raw.size();
        base_rec.format = geom.is_image ? geom.format : "audio/raw";
        base_rec.width = geom.width;
        base_rec.height = geom.height;
        base_rec.transform = "UNFILTERED";
        base_rec.channels = 0;
        base_rec.entropy = base_entropy;
        base_rec.lz_friendliness_pct = base_lz;
        base_rec.entropy_delta_gain = 0.0;
        base_rec.lz_gain_pct = 0.0;
        base_rec.per_plane_entropies = "-";
        base_rec.note = "raw uncompressed bytes";
        records.push_back(base_rec);

        // 2. Candidate DELTA channels. Emission starts at the pixel data
        //    (bfOffBits) for images, so measure the same phase the encoder
        //    would emit: a 54-byte header is not a multiple of 4, and starting
        //    at byte 0 shifts every 32 bpp plane by two bytes.
        size_t delta_start = geom.is_image ? geom.pixel_offset : 0;
        if (delta_start >= raw.size()) delta_start = 0;
        std::vector<core::byte> residual(raw.size());
        if (delta_start > 0) {
            std::memcpy(residual.data(), raw.data(), delta_start);
        }
        for (size_t ch : CANDIDATE_CHANNELS) {
            if (ch > raw.size() - delta_start) continue;
            bool ok = compress::Filters50::encode_delta(
                raw.data() + delta_start, residual.data() + delta_start, raw.size() - delta_start,
                static_cast<core::uint8>(ch));
            if (!ok) continue;

            double res_entropy = compute_entropy(residual.data(), residual.size());
            double res_lz = compute_lz_friendliness(residual.data(), residual.size()) * 100.0;

            // Per-plane entropies over the emitted region only
            const size_t delta_size = raw.size() - delta_start;
            const size_t full = delta_size / ch;
            const size_t rem = delta_size % ch;
            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2);
            for (size_t c = 0; c < ch; ++c) {
                size_t psize = full + (c < rem ? 1 : 0);
                size_t pstart = delta_start + c * full + (c < rem ? c : rem);
                double pe = compute_entropy(residual.data() + pstart, psize);
                if (c > 0) oss << ";";
                oss << pe;
            }

            RowRecord rec;
            rec.filename = fname;
            rec.filesize = raw.size();
            rec.format = geom.is_image ? geom.format : "audio/raw";
            rec.width = geom.width;
            rec.height = geom.height;
            rec.transform = "DELTA";
            rec.channels = ch;
            rec.entropy = res_entropy;
            rec.lz_friendliness_pct = res_lz;
            rec.entropy_delta_gain = base_entropy - res_entropy; // positive is improvement
            rec.lz_gain_pct = res_lz - base_lz;
            rec.per_plane_entropies = oss.str();
            rec.note = "RAR5 type 0 DELTA";
            records.push_back(rec);
        }

        // 3. Off-line only 2D predictors (Up / Paeth) for images
        if (geom.is_image && geom.pixel_offset + geom.height * geom.row_stride <= raw.size() &&
            geom.height > 1) {
            const core::byte* pixels = raw.data() + geom.pixel_offset;
            auto up_res = compute_up_residual(pixels, geom.width, geom.height, geom.row_stride);
            double up_entropy = compute_entropy(up_res.data(), up_res.size());
            double up_lz = compute_lz_friendliness(up_res.data(), up_res.size()) * 100.0;

            RowRecord up_rec;
            up_rec.filename = fname;
            up_rec.filesize = raw.size();
            up_rec.format = geom.format;
            up_rec.width = geom.width;
            up_rec.height = geom.height;
            up_rec.transform = "UP-DELTA";
            up_rec.channels = geom.bytes_per_pixel;
            up_rec.entropy = up_entropy;
            up_rec.lz_friendliness_pct = up_lz;
            up_rec.entropy_delta_gain = base_entropy - up_entropy;
            up_rec.lz_gain_pct = up_lz - base_lz;
            up_rec.per_plane_entropies = "N/A";
            up_rec.note = "upper bound, not emittable in RAR5 (row-wise)";
            records.push_back(up_rec);

            auto paeth_res = compute_paeth_residual(pixels, geom.width, geom.height,
                                                    geom.row_stride, geom.bytes_per_pixel);
            double paeth_entropy = compute_entropy(paeth_res.data(), paeth_res.size());
            double paeth_lz = compute_lz_friendliness(paeth_res.data(), paeth_res.size()) * 100.0;

            RowRecord paeth_rec;
            paeth_rec.filename = fname;
            paeth_rec.filesize = raw.size();
            paeth_rec.format = geom.format;
            paeth_rec.width = geom.width;
            paeth_rec.height = geom.height;
            paeth_rec.transform = "PAETH";
            paeth_rec.channels = geom.bytes_per_pixel;
            paeth_rec.entropy = paeth_entropy;
            paeth_rec.lz_friendliness_pct = paeth_lz;
            paeth_rec.entropy_delta_gain = base_entropy - paeth_entropy;
            paeth_rec.lz_gain_pct = paeth_lz - base_lz;
            paeth_rec.per_plane_entropies = "N/A";
            paeth_rec.note = "upper bound, not emittable in RAR5 (2D Paeth)";
            records.push_back(paeth_rec);
        }
    }

    // Write CSV
    {
        std::ofstream csv(out_csv);
        csv << "filename,filesize,format,width,height,transform,channels,entropy_b_per_B,lz_match_"
               "pct,entropy_delta_gain,lz_gain_pct,per_plane_entropies,note\n";
        for (const auto& r : records) {
            csv << r.filename << "," << r.filesize << "," << r.format << "," << r.width << ","
                << r.height << "," << r.transform << "," << r.channels << "," << std::fixed
                << std::setprecision(4) << r.entropy << "," << r.lz_friendliness_pct << ","
                << r.entropy_delta_gain << "," << r.lz_gain_pct << ",\"" << r.per_plane_entropies
                << "\",\"" << r.note << "\"\n";
        }
        std::cout << "[+] Wrote CSV to " << out_csv << " (" << records.size() << " rows)\n";
    }

    // Write Markdown summary
    {
        std::ofstream md(out_md);
        md << "# PROBE-PREDICT (Wave 1b) Empirical Report\n\n";
        md << "Candidate evaluation of channel predictors on image (BMP/TIFF/PPM/TGA/Kodak) and "
              "audio (WAV/RF64/AIFF/CAF) fixtures.\n\n";
        md << "| File | Size | Format | Optimal Emittable DELTA | Baseline H | Residual H | H Gain "
              "| LZ Match Gain | Upper Bound (Paeth H) |\n";
        md << "|:---|:---:|:---:|:---:|:---:|:---:|:---:|:---:|:---:|\n";

        // Group by file
        std::string cur_file = "";
        RowRecord best_delta;
        RowRecord unfilt;
        RowRecord paeth;
        bool has_paeth = false;
        double max_gain = -999.0;

        auto emit_summary_row = [&]() {
            if (cur_file.empty()) return;
            md << "| `" << unfilt.filename << "` | " << unfilt.filesize << " B | " << unfilt.format
               << " | " << "ch=" << best_delta.channels << " | " << std::fixed
               << std::setprecision(3) << unfilt.entropy << " | " << best_delta.entropy << " | "
               << (best_delta.entropy_delta_gain >= 0 ? "+" : "") << best_delta.entropy_delta_gain
               << " | " << (best_delta.lz_gain_pct >= 0 ? "+" : "") << best_delta.lz_gain_pct
               << "% | " << (has_paeth ? std::to_string(paeth.entropy).substr(0, 5) : "N/A")
               << " |\n";
        };

        for (const auto& r : records) {
            if (r.filename != cur_file) {
                emit_summary_row();
                cur_file = r.filename;
                unfilt = r;
                best_delta = r;
                paeth = r;
                has_paeth = false;
                max_gain = -999.0;
            }
            if (r.transform == "DELTA") {
                if (r.entropy_delta_gain > max_gain) {
                    max_gain = r.entropy_delta_gain;
                    best_delta = r;
                }
            } else if (r.transform == "PAETH") {
                paeth = r;
                has_paeth = true;
            }
        }
        emit_summary_row();

        std::cout << "[+] Wrote Markdown summary to " << out_md << "\n";
    }

    return 0;
}
