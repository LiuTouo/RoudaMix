import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import test from "node:test";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("C++, Rust, and TypeScript expose the same error-code set", () => {
  const result = spawnSync(
    process.execPath,
    [path.join(root, "scripts", "error-code-parity.mjs")],
    { cwd: root, encoding: "utf8", windowsHide: true },
  );
  assert.equal(result.status, 0, `${result.stdout}${result.stderr}`);
  assert.match(result.stdout, /error-code parity: \d+ codes agree/);
});
