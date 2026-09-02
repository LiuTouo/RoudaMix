#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "telemetry.hpp"

#define CHECK(x)                                                                  \
    do {                                                                          \
        if (!(x)) {                                                               \
            std::fprintf(stderr, "CHECK FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
            std::abort();                                                         \
        }                                                                         \
    } while (0)

int main() {
    rmx::MeterAccumulator meters;
    rmx::TelemetryBlockShm block{};
    meters.set_runtime(48000.0F, 128, 10, 12);
    rmx::TelemetryStripIdentity strip_table[1]{
        {0, rmx::TelemetryStripKind::kEngineOutput, rmx::kNoTelemetryOwner,
         rmx::kNoTelemetryOwner}};
    std::uint32_t plugin_ids[300]{};
    std::uint32_t variants[300]{};
    for (std::uint32_t i = 0; i < 300; ++i) {
        plugin_ids[i] = 1000u + i;
        variants[i] = i < 128 ? 0u : 1u;
        if (i < rmx::kPluginLoadEntries) meters.add_plugin_cycles(i, 100u + i);
    }
    meters.publish(block, 3, strip_table, 1, plugin_ids, variants, 300,
                   true);
    CHECK(block.magic == rmx::kTelemetryMagic);
    CHECK(block.abi_version == rmx::kTelemetryAbiVersion);
    CHECK(block.sequence == 2 && (block.sequence & 1u) == 0);
    CHECK(block.plugin_load_count == rmx::kPluginLoadEntries);
    for (std::size_t i = 0; i < rmx::kPluginLoadEntries; ++i) {
        CHECK(block.plugin_loads[i].instance_id == 1000u + i);
        CHECK(block.plugin_loads[i].variant == (i < 128 ? 0u : 1u));
        CHECK(std::isfinite(block.plugin_loads[i].process_load));
    }
    CHECK(block.spectrum_count == rmx::kTelemetrySpectrumBins);

    meters.publish(block, 4, strip_table, 1, plugin_ids, variants, 0,
                   false);
    CHECK(block.sequence == 4);
    CHECK(block.plugin_load_count == 0);
    CHECK(block.spectrum_count == 0);

    std::printf("telemetry_test PASSED\n");
    return 0;
}
