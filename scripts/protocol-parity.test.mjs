import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";
import path from "node:path";
import test from "node:test";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("engine and bridge classify every protocol fixture identically", () => {
  const result = spawnSync(
    process.execPath,
    [path.join(root, "scripts", "protocol-parity.mjs")],
    { cwd: root, encoding: "utf8" },
  );
  assert.equal(result.status, 0, `${result.stdout}${result.stderr}`);
  assert.match(result.stdout, /protocol parity: \d+ fixtures agree/);
});
