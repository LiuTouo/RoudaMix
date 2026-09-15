// minisign 簽章驗證 — tauri CLI 無 signer verify 子命令,複刻 minisign-verify 0.2.5
// (blake2b512 預雜湊 + Ed25519 + key_id 比對 + global signature),僅用 node 內建 crypto。
// 用法: node verify-updater-sig.mjs <artifact> <artifact.sig> <pubkey(base64 或完整公鑰檔內容)>
import { readFileSync } from 'node:fs';
import { createHash, createPublicKey, verify } from 'node:crypto';

const [, , artifactPath, sigPath, pubkeyArg] = process.argv;
if (!artifactPath || !sigPath || !pubkeyArg) {
  console.error('usage: node verify-updater-sig.mjs <artifact> <sig> <pubkey>');
  process.exit(1);
}
const die = (message) => { console.error('verify-updater-sig: ' + message); process.exit(1); };
const decode = (text) => Buffer.from(text, 'base64');

// 公鑰:base64 單行(RW 開頭)、完整公鑰檔文字(untrusted comment 開頭),
// 或 tauri.conf.json 的整檔再包一層 base64(其餘情形)。
let pubText = pubkeyArg.trim();
if (!pubText.startsWith('RW') && !pubText.startsWith('untrusted comment')) pubText = decode(pubText).toString('utf8');
const pubLines = pubText.split(/\r?\n/).filter((line) => line && !line.startsWith('untrusted comment'));
const pub = decode(pubLines[pubLines.length - 1]);
if (pub.length !== 42) die('public key blob must be 42 bytes');
const keyId = pub.subarray(2, 10);
const key = createPublicKey({
  key: { kty: 'OKP', crv: 'Ed25519', x: pub.subarray(10, 42).toString('base64url') },
  format: 'jwk',
});

let sigText = readFileSync(sigPath, 'utf8').trim();
// tauri CLI 產物是「整份 minisign 文字再包一層 base64」;先解外層。
if (!sigText.startsWith('untrusted comment')) sigText = decode(sigText).toString('utf8').trim();
const sigLines = sigText.split(/\r?\n/);
if (sigLines.length < 4) die('signature file must have 4 lines');
const blob = decode(sigLines[1]);
if (blob.length !== 74 || blob[0] !== 0x45 || blob[1] !== 0x44) die('want 74-byte prehashed (ED) signature blob');
const sigKeyId = blob.subarray(2, 10);
const sig = blob.subarray(10, 74);
if (!sigLines[2].startsWith('trusted comment: ')) die('missing trusted comment');
const trusted = sigLines[2].slice(17);
const globalSig = decode(sigLines[3]);
if (globalSig.length !== 64) die('global signature must be 64 bytes');
if (!keyId.equals(sigKeyId)) die('key id mismatch');

const data = readFileSync(artifactPath);
const hash = createHash('blake2b512').update(data).digest();
if (!verify(null, hash, key, sig)) die('signature mismatch');
if (!verify(null, Buffer.concat([sig, Buffer.from(trusted, 'utf8')]), key, globalSig)) die('global signature mismatch');
console.log(`verified ${artifactPath} (minisign Ed25519, key id ${sigKeyId.toString('hex')})`);
