// fixtures conformance:valid 必解、invalid 必拒(契約:contracts/protocol.md §9)
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace fs = std::filesystem;

static int g_fail = 0;

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void check_dir(const fs::path& dir, bool must_pass) {
    if (!fs::exists(dir)) {
        std::printf("MISSING DIR: %s\n", dir.string().c_str());
        ++g_fail;
        return;
    }
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir)) files.push_back(e.path());
    if (files.empty()) {
        std::printf("EMPTY DIR (fixtures missing): %s\n", dir.string().c_str());
        ++g_fail;
        return;
    }
    for (const auto& p : files) {
        nlohmann::json j = nlohmann::json::parse(read_file(p), nullptr, false);
        bool pass;
        if (j.is_discarded()) {
            pass = !must_pass;
        } else {
            try {
                rmx::parse_frame(j, /*strict=*/true);
                pass = must_pass;
            } catch (const rmx::ParseError&) {
                pass = !must_pass;
            }
        }
        if (!pass) {
            std::printf("FAIL(%s): %s\n", must_pass ? "valid" : "invalid",
                        p.filename().string().c_str());
            ++g_fail;
        }
    }
}

int main(int argc, char** argv) {
    fs::path fixtures = argc > 1 ? fs::path(argv[1])
                                 : fs::path("fixtures") / "protocol";
    check_dir(fixtures / "valid", /*must_pass=*/true);
    check_dir(fixtures / "invalid", /*must_pass=*/false);
    if (g_fail == 0) {
        std::printf("conformance: all pass\n");
        return 0;
    }
    std::printf("conformance: %d failure(s)\n", g_fail);
    return 1;
}
