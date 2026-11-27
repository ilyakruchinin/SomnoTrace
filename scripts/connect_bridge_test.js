/*
 * SomnoTrace - Host unit tests for browser postMessage bridge in portal.html
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * ADDITIONAL TERM (GPLv3 Section 7(b)): Redistributions must preserve the
 * attribution "Based on SomnoTrace, originally created by Ilya Kruchinin
 * (https://github.com/ilyakruchinin)." See the NOTICE file for details.
 */

'use strict';

const assert = require('assert');
const fs = require('fs');
const path = require('path');
const vm = require('vm');

const portal = fs.readFileSync(path.join(__dirname, '../main/portal.html'), 'utf8');

function extractFunction(name) {
  const start = portal.indexOf('function ' + name + '(');
  assert(start >= 0, 'missing function ' + name);
  const end = portal.indexOf('\n}', start);
  assert(end >= 0, 'unterminated function ' + name);
  return portal.slice(start, end + 2);
}

const context = vm.createContext({
  URL,
  encodeURIComponent,
  Array,
  Object,
  Error,
  Promise,
  setTimeout,
  clearTimeout,
});
vm.runInContext(extractFunction('parseConnectOrigin'), context);
vm.runInContext(extractFunction('connectRunTransfer'), context);

function testOriginValidation() {
  let r = context.parseConnectOrigin('https://cpapclarity.com');
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.origin, 'https://cpapclarity.com');

  r = context.parseConnectOrigin('https://cpapclarity.com/import?foo=1#bar');
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.origin, 'https://cpapclarity.com');

  r = context.parseConnectOrigin('http://localhost:3000/test');
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.origin, 'http://localhost:3000');

  r = context.parseConnectOrigin('http://127.0.0.1:8080');
  assert.strictEqual(r.ok, true);
  assert.strictEqual(r.origin, 'http://127.0.0.1:8080');

  for (const bad of [
    '',
    null,
    'not a url',
    'http://evil.example',
    'http://192.168.1.50',
    'javascript:alert(1)',
    'data:text/html,<script>',
    'file:///etc/passwd',
  ]) {
    const res = context.parseConnectOrigin(bad);
    assert.strictEqual(res.ok, false, 'should reject: ' + bad);
  }
}

async function testBridgeTransfer() {
  const targetOrigin = 'https://cpapclarity.com';
  const manifestFiles = [
    { path: 'STR.edf', size: 4, mtime: 100 },
    { path: 'DATALOG/20261124/BRP.edf', size: 6, mtime: 200 },
  ];
  const filePayloads = {
    'STR.edf': new Uint8Array([1, 2, 3, 4]).buffer,
    'DATALOG/20261124/BRP.edf': new Uint8Array([10, 20, 30, 40, 50, 60]).buffer,
  };

  const listeners = new Set();
  const posted = [];
  const opener = {
    postMessage(data, origin, transfer) {
      posted.push({ data, origin, transfer });
      if (data.type === 'st:manifest') {
        // Noise from wrong origin and wrong source must be ignored
        for (const fn of listeners) {
          fn({ origin: 'https://evil.example', source: opener, data: { type: 'st:request', paths: ['STR.edf'] } });
          fn({ origin: targetOrigin, source: {}, data: { type: 'st:request', paths: ['STR.edf'] } });
          // Valid incremental request for only 1 of 2 files
          fn({ origin: targetOrigin, source: opener, data: { type: 'st:request', paths: ['DATALOG/20261124/BRP.edf'] } });
        }
      }
    },
  };

  const mockFetch = async (url) => {
    if (url === '/api/sdcard/files') {
      return { ok: true, status: 200, json: async () => manifestFiles };
    }
    const prefix = '/api/sdcard/file?path=';
    if (url.startsWith(prefix)) {
      const p = decodeURIComponent(url.slice(prefix.length));
      const buf = filePayloads[p];
      if (!buf) return { ok: false, status: 404 };
      return { ok: true, status: 200, arrayBuffer: async () => buf.slice(0) };
    }
    return { ok: false, status: 404 };
  };

  const progress = [];
  const count = await context.connectRunTransfer(
    opener,
    targetOrigin,
    mockFetch,
    (ev, fn) => { if (ev === 'message') listeners.add(fn); },
    (ev, fn) => { if (ev === 'message') listeners.delete(fn); },
    (done, total, p) => progress.push({ done, total, p }),
    () => false
  );

  assert.strictEqual(count, 1);
  assert.strictEqual(posted.length, 3);
  assert.strictEqual(posted[0].origin, targetOrigin);
  assert.strictEqual(posted[0].data.type, 'st:manifest');
  assert.strictEqual(posted[0].data.v, 1);
  assert.strictEqual(posted[0].data.files.length, 2);

  assert.strictEqual(posted[1].origin, targetOrigin);
  assert.strictEqual(posted[1].data.type, 'st:file');
  assert.strictEqual(posted[1].data.path, 'DATALOG/20261124/BRP.edf');
  assert.strictEqual(posted[1].data.size, 6);
  assert.strictEqual(posted[1].data.mtime, 200);
  assert.strictEqual(posted[1].transfer.length, 1);
  assert.strictEqual(posted[1].transfer[0], posted[1].data.data);
  assert.deepStrictEqual(Array.from(new Uint8Array(posted[1].data.data)), [10, 20, 30, 40, 50, 60]);

  assert.strictEqual(posted[2].origin, targetOrigin);
  assert.strictEqual(posted[2].data.type, 'st:done');
  assert.strictEqual(posted[2].data.count, 1);
  assert.strictEqual(listeners.size, 0);

  // Unknown path requested by opener must abort and post st:error
  const errPosted = [];
  const badListeners = new Set();
  const badOpener = {
    postMessage(data, origin) {
      errPosted.push({ data, origin });
      if (data.type === 'st:manifest') {
        for (const fn of badListeners) {
          fn({ origin: targetOrigin, source: badOpener, data: { type: 'st:request', paths: ['../etc/passwd'] } });
        }
      }
    },
  };
  let rejected = false;
  try {
    await context.connectRunTransfer(
      badOpener,
      targetOrigin,
      mockFetch,
      (ev, fn) => { if (ev === 'message') badListeners.add(fn); },
      (ev, fn) => { if (ev === 'message') badListeners.delete(fn); },
      null,
      () => false
    );
  } catch (e) {
    rejected = true;
    assert.strictEqual(e.message, 'unknown_path');
  }
  assert.strictEqual(rejected, true);
  assert.strictEqual(errPosted[1].data.type, 'st:error');
  assert.strictEqual(errPosted[1].data.error, 'unknown_path');

  // User cancel during transfer must abort cleanly and post user_cancelled
  let cancelFlag = false;
  const cancelPosted = [];
  const cancelListeners = new Set();
  const cancelOpener = {
    postMessage(data, origin) {
      cancelPosted.push({ data, origin });
      if (data.type === 'st:manifest') {
        cancelFlag = true;
        for (const fn of cancelListeners) {
          fn({ origin: targetOrigin, source: cancelOpener, data: { type: 'st:request', paths: ['STR.edf'] } });
        }
      }
    },
  };
  let cancelRejected = false;
  try {
    await context.connectRunTransfer(
      cancelOpener,
      targetOrigin,
      mockFetch,
      (ev, fn) => { if (ev === 'message') cancelListeners.add(fn); },
      (ev, fn) => { if (ev === 'message') cancelListeners.delete(fn); },
      null,
      () => cancelFlag
    );
  } catch (e) {
    cancelRejected = true;
    assert.strictEqual(e.message, 'user_cancelled');
  }
  assert.strictEqual(cancelRejected, true);
  assert.strictEqual(cancelPosted[1].data.type, 'st:error');
  assert.strictEqual(cancelPosted[1].data.error, 'user_cancelled');
}

(async function main() {
  testOriginValidation();
  await testBridgeTransfer();
  console.log('connect_bridge_test: all tests passed');
})().catch((err) => {
  console.error(err);
  process.exit(1);
});
