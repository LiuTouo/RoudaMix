import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const engineProbe = path.join(root, "engine", "build", "Release", "protocol_probe.exe");
const fixtures = path.join(root, "fixtures", "protocol");

run("cmake", ["-S", "engine", "-B", "engine/build"]);
run("cmake", ["--build", "engine/build", "--config", "Release", "--target", "protocol_probe"]);
assert.ok(existsSync(engineProbe), `engine protocol probe missing: ${engineProbe}`);

const engine = parseLines(run(engineProbe, [fixtures]).stdout, "engine");
const bridge = parseLines(
  run("cargo", [
    "run",
    "--quiet",
    "--manifest-path",
    "src-tauri/Cargo.toml",
    "--bin",
    "protocol_probe",
    "--",
    fixtures,
  ]).stdout,
  "bridge",
);

assert.deepEqual([...engine.keys()], [...bridge.keys()], "fixture sets differ");
const mismatches = [];
for (const [file, engineResult] of engine) {
  const bridgeResult = bridge.get(file);
  if (JSON.stringify(engineResult) !== JSON.stringify(bridgeResult)) {
    mismatches.push({ file, engine: engineResult, bridge: bridgeResult });
  }
  const shouldAccept = file.startsWith("valid/");
  if (engineResult.accepted !== shouldAccept) {
    mismatches.push({ file, expectedAccepted: shouldAccept, actual: engineResult });
  }
}
assert.deepEqual(mismatches, [], "protocol adapter parity failures");
console.log(`protocol parity: ${engine.size} fixtures agree`);

function run(command, args) {
  const result = spawnSync(command, args, { cwd: root, encoding: "utf8", windowsHide: true });
  if (result.status !== 0) {
    throw new Error(
      `${command} ${args.join(" ")} failed (${result.status})\n${result.stdout}${result.stderr}`,
    );
  }
  return result;
}

function parseLines(stdout, adapter) {
  const results = new Map();
  for (const line of stdout.split(/\r?\n/).filter(Boolean)) {
    let result;
    try {
      result = JSON.parse(line);
    } catch {
      throw new Error(`${adapter} probe emitted non-JSON output: ${line}`);
    }
    results.set(result.file, { accepted: result.accepted, code: result.code });
  }
  return results;
}
