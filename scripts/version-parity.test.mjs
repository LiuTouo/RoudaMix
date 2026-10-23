import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const read = (p) => readFileSync(path.join(root, p), "utf8");

// 版本同步曾是純手工作業：0.1.41 因 kEngineVersion 漏改而敗 CI（24aa390），
// 而 router fixture 比對已凍結 engineVersion（2dbddb4），不再隱含檢查同步。
const sources = {
    "package.json": (text) => JSON.parse(text).version,
    "Cargo.toml": (text) => text.match(/^version = "([^"]+)"/m)?.[1],
    "src-tauri/tauri.conf.json": (text) => JSON.parse(text).version,
    "engine/src/protocol.hpp": (text) => text.match(/kEngineVersion = "([^"]+)"/)?.[1],
};

test("app version stays in sync across manifests and the engine", () => {
    const versions = Object.fromEntries(
        Object.entries(sources).map(([file, pick]) => [file, pick(read(file))]),
    );
    const expected = versions["package.json"];
    for (const [file, version] of Object.entries(versions)) {
        assert.match(
            version ?? "",
            /^\d+\.\d+\.\d+(-[\w.]+)?$/,
            `${file}: 無法解析版本字串，請檢查解析規則`,
        );
        assert.equal(
            version,
            expected,
            `${file} 是 ${version}，與 package.json 的 ${expected} 不一致；改版時四個檔案要一起 bump`,
        );
    }
});
