import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(path.join(root, p), "utf8");
const vendor = "engine/third_party/libebur128/";

test("libebur128 v1.2.6 upstream files remain byte-identical to the pinned source", () => {
  const upstream = JSON.parse(read(vendor + "UPSTREAM.json"));
  assert.equal(upstream.version, "1.2.6");
  assert.equal(upstream.commit, "67b33abe1558160ed76ada1322329b0e9e058b02");
  assert.equal(upstream.license, "MIT AND BSD-3-Clause");
  assert.equal(upstream.files.length, 4);
  for (const { file, sha256 } of upstream.files) {
    assert.equal(createHash("sha256").update(readFileSync(path.join(root, vendor, file))).digest("hex"), sha256, file);
  }
});

test("release inventory and portable archives include both upstream licenses", () => {
  assert.match(read("scripts/release.py"), /add\("libebur128", "1\.2\.6", "MIT AND BSD-3-Clause"/);
  assert.match(read("scripts/release.py"), /license_texts\(ROOT \/ "engine\/third_party\/libebur128"\)/);
  const script = read("scripts/build-portable.ps1");
  const notices = read("packaging/portable/THIRD-PARTY-NOTICES.txt");
  for (const [source, destination] of [["COPYING", "libebur128-MIT.txt"], ["COPYING.queue", "libebur128-queue-BSD.txt"]]) {
    assert.ok(script.includes(`engine\\third_party\\libebur128\\${source}`));
    assert.ok(script.includes(destination));
    assert.ok(notices.includes(`LICENSES/${destination}`));
  }
  assert.equal(read(vendor + "COPYING.queue"), read(vendor + "sys/queue.h").split("*/")[0] + "*/\n");
  assert.match(read(vendor + "COPYING"), /Jan Kokemüller/);
  const cmake = read("engine/CMakeLists.txt");
  assert.match(cmake, /add_library\(ebur128 STATIC third_party\/libebur128\/ebur128.c\)/);
  assert.match(cmake, /CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded/);
});
