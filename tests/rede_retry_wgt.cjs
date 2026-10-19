'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const src = fs.readFileSync('src/rede.c', 'utf8');
const found = src.match(/EM_JS\(char \*, nv_http,[\s\S]*?, \{([\s\S]*?)\n\}\);/);
assert(found, 'production nv_http body');
const heap = new ArrayBuffer(4096);
const HEAP32 = new Int32Array(heap), HEAPU8 = new Uint8Array(heap);
let header = null, noHeader = false, now = Date.UTC(2026, 9, 3, 12, 0, 0);
class XHR {
  open() {}
  overrideMimeType() {}
  setRequestHeader() {}
  send() { this.status = 429; this.responseText = 'response'; }
  getResponseHeader(key) { if (noHeader) throw new Error('not exposed'); return key === 'retry-after' ? header : null; }
}
class Clock extends Date { static now() { return now; } }
const run = new Function('UTF8ToString', 'XMLHttpRequest', 'HEAP32', 'HEAPU8', '_malloc',
  'stringToUTF8', 'Date', 'metodo', 'url', 'cabs', 'corpo', 'tam', 'status',
  'urlFinal', 'urlFinalTam', 'etag', 'etagTam', 'retryAfter', found[1]);
function check(value, expected, throws = false) {
  header = value; noHeader = throws; HEAP32[4] = 777;
  const reply = run(p => p === 1 ? 'GET' : 'https://fixture.invalid/', XHR, HEAP32,
    HEAPU8, () => 256, () => {}, Clock, 1, 2, 0, 0, 4, 8, 0, 0, 0, 0, 16);
  assert.equal(reply, 256); assert.equal(HEAP32[2], 429);
  assert.equal(HEAP32[4], expected);
}
check('19', 19);
check(' 120 ', 120);
check('999999999999999999999999', 2147483647);
check('Sat, 03 Oct 2026 12:01:30 GMT', 90);
check('Sat, 03 Oct 2026 11:00:00 GMT', 0);
check('nonsense', 0);
check('12 garbage', 0);
check('-5', 0);
check('', 0);
check(null, 0);
check('19', 0, true);
assert(src.includes('r->erro = REDE_INDISPONIVEL;\n  return 0; /* nenhum XHR'));
console.log('rede_retry_wgt: PASS (exposed seconds/date; hidden/invalid keeps unknown; strict request unavailable)');
