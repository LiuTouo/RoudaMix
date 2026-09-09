import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { existsSync } from "node:fs";
import { fileURLToPath, pathToFileURL } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const engineProbe = path.join(root, "engine", "build", "Release", "protocol_probe.exe");

run("cmake", ["-S", "engine", "-B", "engine/build"]);
run("cmake", ["--build", "engine/build", "--config", "Release", "--target", "protocol_probe"]);
assert.ok(existsSync(engineProbe), `engine protocol probe missing: ${engineProbe}`);

const engineCodes = parseCodes(run(engineProbe, ["--error-codes"]).stdout, "engine");
const bridgeCodes = parseCodes(
  run("cargo", [
    "run",
    "--quiet",
    "--manifest-path",
    "src-tauri/Cargo.toml",
    "--bin",
    "protocol_probe",
    "--features",
    "protocol-probe",
    "--",
    "--error-codes",
  ]).stdout,
  "bridge",
);
const generated = await import(
  pathToFileURL(path.join(root, "ui", "src", "lib", "error-codes.generated.ts"))
);
const descriptions = await import(
  pathToFileURL(path.join(root, "ui", "src", "lib", "errors.ts"))
);
const uiCodes = [...generated.ERROR_CODES].sort();
const descriptionCodes = Object.keys(descriptions.ENGINE_ERROR_MESSAGES).sort();

assert.deepEqual(engineCodes, bridgeCodes, "C++ and Rust error-code sets differ");
assert.deepEqual(engineCodes, uiCodes, "backend and generated TypeScript error-code sets differ");
assert.deepEqual(engineCodes, descriptionCodes, "backend and UI description sets differ");
console.log(`error-code parity: ${engineCodes.length} codes agree`);

function run(command, args) {
  const result = spawnSync(command, args, {
    cwd: root,
    encoding: "utf8",
    windowsHide: true,
  });
  if (result.status !== 0) {
    throw new Error(
      `${command} ${args.join(" ")} failed (${result.status})\n${result.stdout}${result.stderr}`,
    );
  }
  return result;
}

function parseCodes(stdout, adapter) {
  let value;
  try {
    value = JSON.parse(stdout);
  } catch {
    throw new Error(`${adapter} probe emitted invalid error-code JSON: ${stdout}`);
  }
  assert.ok(Array.isArray(value), `${adapter} error codes must be an array`);
  return [...value].sort();
}
