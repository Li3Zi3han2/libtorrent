// Standalone Windows torrent creator based on libtorrent 2.x.
// Uses Windows Unicode command-line arguments. No Python dependency.
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/version.hpp>
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <cstdint>

namespace fs = std::filesystem;
namespace lt = libtorrent;

static std::string utf8(std::wstring const& s)
{
    if (s.empty()) return {};
    int const len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        s.data(), static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    std::string result(static_cast<std::size_t>(len), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        s.data(), static_cast<int>(s.size()), result.data(), len, nullptr, nullptr))
        throw std::runtime_error("UTF-16 to UTF-8 conversion failed");
    return result;
}

static std::wstring value(int& i, int argc, wchar_t** argv)
{
    if (++i >= argc) throw std::runtime_error("Missing option value");
    return argv[i];
}

static void help()
{
    std::cout
        << "libtorrent.exe (libtorrent " << LIBTORRENT_VERSION << ")\n"
        << "Usage:\n"
        << "  libtorrent.exe create <file-or-directory> --output <file.torrent> [options]\n"
        << "  libtorrent.exe verify <file.torrent> [--mode <v2|hybrid|v1-aligned>]\n"
        << "  libtorrent.exe --version\n\n"
        << "Options for create:\n"
        << "  --mode <v2|hybrid|v1-aligned>  Default: hybrid\n"
        << "  --piece-mib <1|2|4|8|16|32|64|128>  Default: automatic\n"
        << "  --tracker <url>               Repeatable\n"
        << "  --private                     Requires at least one tracker\n"
        << "  --comment <text>\n"
        << "  --overwrite\n"
        << "  --help\n\n"
        << "All modes align non-empty files to piece boundaries.\n"
        << "Input files are never modified.\n";
}

static void verify_torrent(fs::path const& path, std::wstring const& expected)
{
    lt::torrent_info ti(path.u8string());
    if (!ti.is_valid()) throw std::runtime_error("Invalid torrent metadata");
    bool const has_v1 = ti.v1();
    bool const has_v2 = ti.v2();
    if (expected == L"v2" && (has_v1 || !has_v2))
        throw std::runtime_error("Not a v2-only torrent");
    if (expected == L"hybrid" && (!has_v1 || !has_v2))
        throw std::runtime_error("Not a hybrid torrent");
    if (expected == L"v1-aligned" && (!has_v1 || has_v2))
        throw std::runtime_error("Not a v1-only torrent");
    int files = 0;
    auto const& layout = ti.files();
    for (auto f : layout.file_range()) {
        if (layout.pad_file_at(f) || layout.file_size(f) == 0) continue;
        ++files;
        if (layout.file_offset(f) % ti.piece_length() != 0)
            throw std::runtime_error("File starts inside a piece (not aligned)");
    }
    if (files == 0) throw std::runtime_error("No non-empty data files found");
    std::cout << "VERIFIED: " << (has_v1 ? (has_v2 ? "hybrid" : "v1") : "v2")
              << ", piece=" << ti.piece_length()
              << " bytes, non-empty files=" << files
              << ", all file starts aligned\n";
}

int wmain(int argc, wchar_t** argv)
{
    try {
        SetConsoleOutputCP(CP_UTF8);
        if (argc == 1 || (argc == 2 && std::wstring(argv[1]) == L"--help")) {
            help();
            return 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--version") {
            std::cout << "libtorrent " << LIBTORRENT_VERSION << "\n";
            return 0;
        }

        std::wstring const command = argv[1];
        if (command == L"verify") {
            if (argc != 3 && argc != 5)
                throw std::runtime_error("Usage: verify FILE [--mode MODE]");
            std::wstring mode;
            if (argc == 5) {
                if (std::wstring(argv[3]) != L"--mode")
                    throw std::runtime_error("Expected --mode");
                mode = argv[4];
            }
            fs::path file = fs::absolute(fs::path(argv[2]));
            verify_torrent(file, mode);
            return 0;
        }
        if (command != L"create" || argc < 5) {
            help();
            return 2;
        }

        fs::path const input = fs::absolute(fs::path(argv[2])).lexically_normal();
        fs::path output;
        std::wstring mode = L"hybrid";
        int piece_bytes = 0;
        bool private_torrent = false;
        bool overwrite = false;
        std::vector<std::string> trackers;
        std::string comment;

        for (int i = 3; i < argc; ++i) {
            std::wstring const opt = argv[i];
            if (opt == L"--mode") {
                mode = value(i, argc, argv);
            } else if (opt == L"--output") {
                output = fs::path(value(i, argc, argv));
            } else if (opt == L"--piece-mib") {
                std::wstring const s = value(i, argc, argv);
                std::size_t pos = 0;
                unsigned long const n = std::stoul(s, &pos, 10);
                if (pos != s.size() || n < 1 || n > 128 || (n & (n - 1)) != 0)
                    throw std::runtime_error("--piece-mib must be a power of two (1..128)");
                piece_bytes = static_cast<int>(n * 1024UL * 1024UL);
            } else if (opt == L"--tracker") {
                trackers.push_back(utf8(value(i, argc, argv)));
            } else if (opt == L"--private") {
                private_torrent = true;
            } else if (opt == L"--comment") {
                comment = utf8(value(i, argc, argv));
            } else if (opt == L"--overwrite") {
                overwrite = true;
            } else if (opt == L"--help") {
                help();
                return 0;
            } else {
                throw std::runtime_error("Unrecognized option: " + utf8(opt));
            }
        }

        if (mode != L"hybrid" && mode != L"v2" && mode != L"v1-aligned")
            throw std::runtime_error("Unknown mode (use v2, hybrid, v1-aligned)");
        if (output.empty()) throw std::runtime_error("--output is required");
        if (private_torrent && trackers.empty())
            throw std::runtime_error("--private requires at least one --tracker");
        if (!fs::exists(input) || !(fs::is_directory(input) || fs::is_regular_file(input)))
            throw std::runtime_error("Source does not exist or is not a regular file/directory");

        output = fs::absolute(output).lexically_normal();
        if (fs::exists(output) && !overwrite)
            throw std::runtime_error("Output exists; specify --overwrite to replace");
        if (input == output) throw std::runtime_error("Output must not replace the source");
        if (!fs::exists(output.parent_path()) || !fs::is_directory(output.parent_path()))
            throw std::runtime_error("Output parent folder does not exist");
        if (fs::is_directory(input)) {
            fs::path const relative = output.lexically_relative(input);
            if (!relative.empty() && !relative.is_absolute() && *relative.begin() != fs::path(L".."))
                throw std::runtime_error("Output torrent must be outside the source directory");
        }

        lt::create_flags_t flags = lt::create_torrent::no_attributes;
        if (mode == L"v2") flags |= lt::create_torrent::v2_only;
        if (mode == L"v1-aligned")
            flags |= lt::create_torrent::v1_only | lt::create_torrent::canonical_files;

        auto files = lt::list_files(input.u8string(), flags);
        if (files.empty()) throw std::runtime_error("No files to hash");
        lt::create_torrent torrent(std::move(files), piece_bytes, flags);
        torrent.set_creator("libtorrent standalone / libtorrent " LIBTORRENT_VERSION);
        if (!comment.empty()) torrent.set_comment(comment.c_str());
        for (auto const& t : trackers) torrent.add_tracker(t);
        if (private_torrent) torrent.set_priv(true);

        std::cout << "Mode: " << utf8(mode)
                  << "\nPiece: " << torrent.piece_length() << " bytes"
                  << "\nPieces: " << torrent.num_pieces()
                  << "\nHashing source data...\n";
        int last = -1;
        int const total = torrent.num_pieces();
        lt::set_piece_hashes(torrent, input.parent_path().u8string(),
            [total, &last](lt::piece_index_t piece) {
                int const pct = total > 0 ? int((100LL * (int(piece) + 1)) / total) : 100;
                if (pct != last) {
                    std::cout << "\r" << pct << "%" << std::flush;
                    last = pct;
                }
            });
        auto const bytes = torrent.generate_buf();
        {
            std::ofstream out(output, std::ios::binary | std::ios::trunc);
            if (!out) throw std::runtime_error("Cannot open output torrent");
            out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            out.flush();
            if (!out) throw std::runtime_error("Cannot write output torrent");
        }
        std::cout << "\n";
        verify_torrent(output, mode);
        std::cout << "SUCCESS: source files unchanged\n";
        return 0;
    }
    catch (std::exception const& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
