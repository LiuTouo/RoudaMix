#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "protocol.hpp"

namespace fs = std::filesystem;

namespace {

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::stringstream content;
    content << file.rdbuf();
    return content.str();
}

void probe_dir(const fs::path& directory, const char* group) {
    std::vector<fs::path> files;
    for (const auto& entry : fs::directory_iterator(directory))
        if (entry.is_regular_file() && entry.path().extension() == ".json")
            files.push_back(entry.path());
    std::sort(files.begin(), files.end());

    for (const auto& path : files) {
        bool accepted = false;
        std::string code;
        const auto frame = nlohmann::json::parse(read_file(path), nullptr, false);
        if (frame.is_discarded()) {
            code = "bad_frame";
        } else {
            try {
                rmx::parse_frame(frame, /*strict=*/true);
                accepted = true;
            } catch (const rmx::ParseError& error) {
                code = error.code;
            }
        }
        const auto result = nlohmann::json{{"file", std::string(group) + "/" + path.filename().string()},
                                           {"accepted", accepted},
                                           {"code", code}};
        std::puts(result.dump().c_str());
    }
}

}  // namespace

int main(int argc, char** argv) {
    const fs::path root = argc > 1 ? fs::path(argv[1]) : fs::path("fixtures/protocol");
    probe_dir(root / "valid", "valid");
    probe_dir(root / "invalid", "invalid");
    return 0;
}
