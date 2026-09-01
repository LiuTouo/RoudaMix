#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

#include <windows.h>

#include "vst_registry.hpp"

namespace fs = std::filesystem;

namespace {

int failures = 0;

void check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

fs::path temp_root() {
    return fs::temp_directory_path() /
           (L"roudamix-vst-registry-test-" + std::to_wstring(GetCurrentProcessId()));
}

rmx::vst_registry::Entry success_entry(const fs::path& path,
                                       rmx::vst_registry::Fingerprint fingerprint) {
    rmx::vst_registry::Entry entry;
    entry.path = path;
    entry.fingerprint = fingerprint;
    entry.classes.push_back({
        {"uid", "00112233445566778899AABBCCDDEEFF"},
        {"name", "Test Plugin"},
        {"vendor", "RoudaMix"},
        {"version", "1"},
        {"subcategories", "Fx"},
    });
    return entry;
}

}  // namespace

int main() {
    const auto root = temp_root();
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root);
    const auto module = root / L"TEST.VST3";
    {
        std::ofstream out(module, std::ios::binary);
        out << "abc";
    }

    rmx::vst_registry::Fingerprint fp;
    std::string error;
    check(rmx::vst_registry::fingerprint(module, fp, error), "fingerprint regular file");

    rmx::vst_registry::Registry registry;
    registry.roots = {root};
    registry.entries.push_back(success_entry(module, fp));
    // Windows 路徑不分大小寫，normalize 後只能保留一筆。
    auto duplicate = success_entry(root / L"test.vst3", fp);
    registry.entries.push_back(std::move(duplicate));
    rmx::vst_registry::normalize(registry);
    check(registry.entries.size() == 1, "case-insensitive path deduplication");
    check(rmx::vst_registry::find_unchanged(registry, module, fp) != nullptr,
          "unchanged fingerprint reuses cache");
    auto changed = fp;
    ++changed.size;
    check(rmx::vst_registry::find_unchanged(registry, module, changed) == nullptr,
          "changed fingerprint invalidates cache");

    const auto bundle = root / L"Bundle.vst3";
    fs::create_directories(bundle / L"Contents" / L"x86_64-win");
    const auto bundle_binary = bundle / L"Contents" / L"x86_64-win" / L"Bundle.vst3";
    {
        std::ofstream out(bundle_binary, std::ios::binary);
        out << "bundle-v1";
    }
    rmx::vst_registry::Fingerprint bundle_fp;
    check(rmx::vst_registry::fingerprint(bundle, bundle_fp, error) && bundle_fp.size == 9,
          "directory VST3 bundle receives an aggregate fingerprint");
    {
        std::ofstream out(bundle_binary, std::ios::binary | std::ios::app);
        out << "2";
    }
    rmx::vst_registry::Fingerprint changed_bundle_fp;
    check(rmx::vst_registry::fingerprint(bundle, changed_bundle_fp, error) &&
              changed_bundle_fp.size != bundle_fp.size,
          "bundle content change invalidates aggregate fingerprint");

    const auto cache = root / L"vst-registry.json";
    check(rmx::vst_registry::save_atomic(cache, registry, error), "atomic save succeeds");
    rmx::vst_registry::Registry loaded;
    check(rmx::vst_registry::load(cache, loaded, error), "saved registry loads");
    check(loaded.entries.size() == 1 &&
              rmx::vst_registry::plugins_json(loaded).size() == 1,
          "roundtrip keeps successful plugin");

    rmx::vst_registry::Entry failed;
    failed.path = root / L"bad.vst3";
    failed.fingerprint = {10, 20};
    failed.error = "load failed";
    loaded.entries.push_back(failed);
    check(rmx::vst_registry::failures_json(loaded).size() == 1,
          "failed plugin remains quarantined");
    check(rmx::vst_registry::find_unchanged(loaded, failed.path, failed.fingerprint) != nullptr,
          "unchanged failure is reusable");

    {
        std::ofstream out(cache, std::ios::binary | std::ios::trunc);
        out << "{broken";
    }
    loaded.entries.push_back(success_entry(module, fp));
    check(!rmx::vst_registry::load(cache, loaded, error) && loaded.entries.empty(),
          "corrupt registry is rejected and cleared");

    {
        std::ofstream out(cache, std::ios::binary | std::ios::trunc);
        out << R"({"schemaVersion":"1","scannerVersion":1,"roots":[],"entries":[]})";
    }
    check(!rmx::vst_registry::load(cache, loaded, error),
          "wrong registry field types are rejected without throwing");

    const auto blocker = root / L"blocker";
    {
        std::ofstream out(blocker);
        out << "x";
    }
    check(!rmx::vst_registry::save_atomic(blocker / L"registry.json", registry, error),
          "unwritable registry path fails without pretending to commit");

    fs::remove_all(root, ec);
    if (failures == 0) {
        std::puts("vst_registry: all pass");
        return 0;
    }
    std::fprintf(stderr, "vst_registry: %d failure(s)\n", failures);
    return 1;
}
