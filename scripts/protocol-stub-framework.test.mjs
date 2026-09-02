import assert from "node:assert/strict";
import { readFileSync, readdirSync } from "node:fs";
import path from "node:path";
import test from "node:test";
import vm from "node:vm";
import { fileURLToPath } from "node:url";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const context = vm.createContext({});
vm.runInContext(
  readFileSync(path.join(root, "scripts", "protocol-stub-framework.js"), "utf8"),
  context,
);
const contract = JSON.parse(
  readFileSync(path.join(root, "contracts", "command_contract.json"), "utf8"),
);
const validate = context.createProtocolStubValidator(contract);

test("stub accepts payloads through the contract table", () => {
  assert.doesNotThrow(() =>
    validate("start", { deviceKey: "asio:test", sampleRate: null, bufferSize: 256 }),
  );
});

test("stub rejects drift and unknown kinds through the contract table", () => {
  assert.throws(
    () => validate("start", { deviceKey: "asio:test", sampleRate: null, bufferSize: "256" }),
    /payload\.bufferSize/,
  );
  assert.throws(() => validate("not_a_command", {}), /unknown command kind/);
});

test("stub agrees with the generated command fixture corpus", () => {
  const validDir = path.join(root, "fixtures", "protocol", "valid");
  for (const name of readdirSync(validDir).filter((name) => name.startsWith("command_"))) {
    const frame = JSON.parse(readFileSync(path.join(validDir, name), "utf8"));
    assert.doesNotThrow(() => validate(frame.kind, frame.payload), name);
  }

  const invalidDir = path.join(root, "fixtures", "protocol", "invalid");
  for (const name of readdirSync(invalidDir).filter((name) => name.startsWith("generated_"))) {
    const frame = JSON.parse(readFileSync(path.join(invalidDir, name), "utf8"));
    assert.throws(() => validate(frame.kind, frame.payload), undefined, name);
  }
});
