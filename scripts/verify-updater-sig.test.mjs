// verify-updater-sig.mjs 對真實 tauri CLI 簽章產物的驗證。
// fixture/ 內容由 tauri signer generate/sign 產生,金鑰為拋棄式測試金鑰,與正式簽章無關。
import { test } from 'node:test';
import assert from 'node:assert';
import { readFileSync, writeFileSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, dirname } from 'node:path';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const here = dirname(fileURLToPath(import.meta.url));
const fixture = join(here, 'verify-updater-sig.fixture');
const pubB64 = readFileSync(join(fixture, 'test.key.pub'), 'utf8').trim(); // 外層 base64
const pubText = Buffer.from(pubB64, 'base64').toString('utf8'); // untrusted comment + RW 行
const pubLine = pubText.trim().split(/\r?\n/).pop(); // 單行 base64
const sigOuter = readFileSync(join(fixture, 'payload.bin.sig'), 'utf8').trim(); // 外層 base64
const sigText = Buffer.from(sigOuter, 'base64').toString('utf8');
const payload = join(fixture, 'payload.bin');
const sig = join(fixture, 'payload.bin.sig');

function verify(file, sigFile, key) {
  const result = spawnSync(process.execPath, [join(here, 'verify-updater-sig.mjs'), file, sigFile, key],
    { encoding: 'utf8' });
  return { status: result.status, output: (result.stdout + result.stderr).trim() };
}

const outer = (text) => Buffer.from(text, 'utf8').toString('base64');

test('accepts real tauri signature under all three pubkey forms', () => {
  for (const key of [pubLine, pubText, pubB64]) {
    assert.equal(verify(payload, sig, key).status, 0, key.slice(0, 24));
  }
});

test('rejects tampered artifact', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rmx-sig-'));
  const tampered = join(dir, 'tampered.bin');
  writeFileSync(tampered, 'tampered artifact');
  assert.equal(verify(tampered, sig, pubB64).status, 1);
});

test('rejects different public key with same key id', () => {
  const lines = pubText.trim().split(/\r?\n/);
  const blob = Buffer.from(lines[1], 'base64');
  blob[10] ^= 1; // 翻公鑰位元組,key id(前 8 位元組雜湊)不變
  lines[1] = blob.toString('base64');
  assert.equal(verify(payload, sig, outer(lines.join('\n'))).status, 1);
});

test('rejects tampered trusted comment', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rmx-sig-'));
  const tampered = join(dir, 'comment.sig');
  writeFileSync(tampered, outer(sigText.replace('trusted comment: timestamp:', 'trusted comment: timestamp:9')));
  assert.equal(verify(payload, tampered, pubB64).status, 1);
});

test('rejects tampered signature bytes', () => {
  const dir = mkdtempSync(join(tmpdir(), 'rmx-sig-'));
  const lines = sigText.trim().split(/\r?\n/);
  const blob = Buffer.from(lines[1], 'base64');
  blob[10] ^= 1;
  lines[1] = blob.toString('base64');
  const tampered = join(dir, 'tampered.sig');
  writeFileSync(tampered, outer(lines.join('\n')));
  assert.equal(verify(payload, tampered, pubB64).status, 1);
});
