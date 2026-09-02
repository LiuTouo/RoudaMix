import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { readFileSync } from "node:fs";
import test from "node:test";
import { fileURLToPath } from "node:url";
import path from "node:path";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");

test("contract table artifacts are current", () => {
  const result = spawnSync(
    process.execPath,
    [path.join(root, "scripts", "generate-protocol-artifacts.mjs"), "--check"],
    { cwd: root, encoding: "utf8" },
  );
  assert.equal(result.status, 0, `${result.stdout}${result.stderr}`);
});

test("generated invalid fixtures cover every command payload field", () => {
  const contract = JSON.parse(
    readFileSync(path.join(root, "contracts", "command_contract.json"), "utf8"),
  );
  const manifest = JSON.parse(
    readFileSync(
      path.join(root, "fixtures", "protocol", "generated-invalid-manifest.json"),
      "utf8",
    ),
  );

  for (const command of contract.commands) {
    const payload = resolve(contract, command.payload);
    const properties = Object.keys(payload.properties ?? {});
    const coverage = manifest.commands[command.kind];
    assert.ok(coverage, `missing fixture coverage for ${command.kind}`);
    assert.deepEqual(coverage.wrongType.sort(), properties.sort());
    assert.deepEqual(
      coverage.missing.sort(),
      [...(payload.required ?? [])].sort(),
    );
  }
});

test("every command error is defined once in the contract error catalog", () => {
  const contract = JSON.parse(
    readFileSync(path.join(root, "contracts", "command_contract.json"), "utf8"),
  );
  const codes = new Set(contract.errorCodes.map(({ code }) => code));
  assert.equal(codes.size, contract.errorCodes.length, "duplicate error code definitions");
  for (const command of contract.commands) {
    for (const code of command.errors) {
      assert.ok(codes.has(code), `${command.kind} references unknown error code ${code}`);
    }
  }
});

function resolve(contract, schema) {
  if (!schema.$ref) return schema;
  const name = schema.$ref.replace("#/definitions/", "");
  return resolve(contract, contract.definitions[name]);
}
