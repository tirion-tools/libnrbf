// SPDX-License-Identifier: MIT
// Dump an NRBF stream as human-readable text. Useful for schema discovery.
//   usage: nrbf_dump <file> [--full]
// `--full` removes the 80-character string truncation.

#include "nrbf/nrbf.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr,
                     "usage: %s <file> [--full]\n", argv[0]);
        return 2;
    }
    bool full = (argc >= 3 && std::strcmp(argv[2], "--full") == 0);

    std::ifstream f(std::filesystem::u8path(argv[1]), std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open %s\n", argv[1]);
        return 1;
    }
    std::ostringstream buf;
    buf << f.rdbuf();
    std::string bytes = buf.str();

    try {
        std::string out;
        nrbf::dump(bytes, out, full ? size_t(-1) : 80);
        std::fwrite(out.data(), 1, out.size(), stdout);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "parse error: %s\n", e.what());
        return 1;
    }
    return 0;
}
