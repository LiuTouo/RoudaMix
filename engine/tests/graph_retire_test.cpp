// #15 (CWE-416):GraphRetireQueue 門閂契約 — reader 未退,逾時/force 都不可
// delete;退了才刪。固定 500ms 牆鐘原本會在 reader 遍歷中釋放退休圖。
#include <cstdio>
#include <utility>

#include "graph_retire.hpp"

using rmx::GraphRetireQueue;
using rmx::TrackGraph;

static int g_failures = 0;

void check(bool cond, const char* what) {
    if (!cond) {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

int main() {
    // grace 內不刪;逾時且無 reader 才刪
    {
        GraphRetireQueue q;
        q.retire(new TrackGraph, 1000);
        check(q.reap(1200, false) == 0, "grace 內 reap 不刪");
        check(q.reap(1000 + GraphRetireQueue::kGraceMs + 1, false) == 1,
              "逾時且無 reader:刪 1");
    }

    // #15 核心:reader 持圖時,逾時/force 都不刪;退場後一次清空
    {
        GraphRetireQueue q;
        q.retire(new TrackGraph, 0);
        q.retire(new TrackGraph, 0);
        q.reader_enter();
        check(q.reap(10000, false) == 0, "reader 持圖:逾時不刪");
        check(q.reap(10000, true) == 0, "reader 持圖:force 也不刪");
        q.reader_exit();
        check(q.reap(10000, true) == 2, "reader 退場後 force 清 2");
    }

    // 混合:一張逾時、一張未逾時;無 reader 只刪逾時那張(now 單調遞增)
    {
        GraphRetireQueue q;
        q.retire(new TrackGraph, 0);
        q.retire(new TrackGraph, 900);
        check(q.reap(1000, false) == 1, "混合:只刪逾時那張");
        check(q.reap(900 + GraphRetireQueue::kGraceMs + 1, false) == 1,
              "混合:另一張逾時後刪");
    }

    // reader 持圖時保留的 entry 用原 tick:退場後未逾時的仍不刪
    {
        GraphRetireQueue q;
        q.retire(new TrackGraph, 900);
        q.reader_enter();
        (void)q.reap(10000, true);
        q.reader_exit();
        check(q.reap(1000, false) == 0, "保留 entry 用原 tick:未逾時不刪");
        check(q.reap(900 + GraphRetireQueue::kGraceMs + 1, false) == 1,
              "保留 entry 逾時後正常刪");
    }

    // retire(nullptr) = 無操作(retire_graph 換到空圖時不塞)
    {
        GraphRetireQueue q;
        q.retire(nullptr, 0);
        check(q.reap(10000, true) == 0, "retire(nullptr) 無操作");
    }

    // readers_active 平衡
    {
        GraphRetireQueue q;
        check(!q.readers_active(), "初始無 reader");
        q.reader_enter();
        q.reader_enter();
        check(q.readers_active(), "登記後有 reader");
        q.reader_exit();
        q.reader_exit();
        check(!q.readers_active(), "退場後歸零");
    }

    if (g_failures != 0) {
        std::printf("%d failures\n", g_failures);
        return 1;
    }
    std::printf("graph_retire_test: all passed\n");
    return 0;
}
