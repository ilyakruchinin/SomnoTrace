/*
 * SomnoTrace - Dashboard missing-data panel regression tests
 * Copyright (C) 2026 Ilya Kruchinin <https://github.com/ilyakruchinin>
 *
 * This file is part of SomnoTrace.
 *
 * SomnoTrace is free software: you can redistribute it and/or modify it under
 * the terms of the GNU General Public License as published by the Free Software
 * Foundation, either version 3 of the License, or (at your option) any later
 * version.
 *
 * SomnoTrace is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <https://www.gnu.org/licenses/>.
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
function getFunction(name) {
  const start = portal.indexOf('function ' + name + '(');
  assert(start >= 0, 'missing function ' + name);
  const end = portal.indexOf('\n}', start);
  assert(end >= 0, 'unterminated function ' + name);
  return portal.slice(start, end + 2);
}

const context = vm.createContext({
  dashSessionData: [],
  dashFullRange: () => ({min: 0, max: 6000}),
  SNT_HDR_SIZE: 28,
  SNT_MISSING_V1: -1,
  SNT_MISSING_V2: -32768,
  DASH_MAX_DISPLAY: 3000,
  dashGridMin: new Float64Array(3000),
  dashGridMax: new Float64Array(3000),
  dashGridOvMin: new Float64Array(3000),
  dashGridOvMax: new Float64Array(3000),
  dashOwner: new Int32Array(3000),
});
for (const name of ['readSntHeader', 'parseSntPld', 'dashLowerBound',
                    'buildSignalData', 'dashPanelState', 'setDashPanelState']) {
  vm.runInContext(getFunction(name), context);
}

function pldBuffer(version, snore, flowlim) {
  const n = snore.length;
  const buf = new ArrayBuffer(28 + n * 24);
  const dv = new DataView(buf);
  dv.setUint32(0, 0x534E5442, true);
  dv.setUint8(4, version);
  dv.setUint8(6, 12);
  dv.setUint8(7, 2);
  dv.setUint16(8, 5, true);
  dv.setUint32(20, n, true);
  for (let i = 0; i < n; i++) {
    dv.setInt16(28 + i * 24 + 18, snore[i], true);
    dv.setInt16(28 + i * 24 + 20, flowlim[i], true);
  }
  return buf;
}

const snore = {key: 'snore', src: 'pld', ch: 'snore'};
const flowlim = {key: 'flowlim', src: 'pld', ch: 'flowlim'};
for (const [version, missing] of [[1, -1], [2, -32768]]) {
  const pld = context.parseSntPld(pldBuffer(version, [missing, missing, missing],
                                           [missing, missing, missing]));
  assert(pld);
  context.dashSessionData = [{pld, pldState: 'ready'}];
  assert.strictEqual(context.buildSignalData(snore, null, 20), null);
  assert.strictEqual(context.buildSignalData(flowlim, null, 20), null);
  assert.strictEqual(context.dashPanelState(snore, null), 'empty');

  const withZero = context.parseSntPld(pldBuffer(version, [missing, 0, missing],
                                                [missing, missing, missing]));
  context.dashSessionData = [{pld: withZero, pldState: 'ready'}];
  const src = context.buildSignalData(snore, null, 20);
  assert(src && src.v.includes(0), 'measured zero must be plotted');
  assert.strictEqual(context.dashPanelState(snore, src), 'ready');
  assert.strictEqual(context.buildSignalData(snore, {min: 4000, max: 6000}, 20), null);
}

context.dashSessionData = [{pldState: 'ready', brpState: 'ready'},
                           {pldState: 'loading', brpState: 'loading'}];
assert.strictEqual(context.dashPanelState(snore, null), 'loading');
assert.strictEqual(context.dashPanelState(snore, {t: [1]}), 'ready');
context.dashSessionData[1] = {pldState: 'loading', noTherapy: true};
assert.strictEqual(context.dashPanelState(snore, null), 'empty');
context.dashSessionData[1] = {pldState: 'unavailable'};
assert.strictEqual(context.dashPanelState(snore, null), 'unavailable');
assert.strictEqual(context.dashPanelState({key: 'press', src: 'pld'}, null), 'unavailable');
assert.strictEqual(context.dashPanelState({key: 'leak', src: 'pld'}, null), 'unavailable');
assert.strictEqual(context.dashPanelState({key: 'flow', src: 'brp'}, null), 'empty');
context.dashSessionData = [{brpState: 'loading', pldState: 'loading'}];
assert.strictEqual(context.dashPanelState({key: 'flow', src: 'brp'}, null), 'loading');
assert.strictEqual(context.dashPanelState({key: 'press', src: 'pld'}, null), 'loading');
context.dashSessionData = [{brpState: 'unavailable', pldState: 'unavailable'}];
assert.strictEqual(context.dashPanelState({key: 'flow', src: 'brp'}, null), 'unavailable');

const classes = new Set();
const attrs = {};
const element = {
  classList: {toggle: (name, on) => on ? classes.add(name) : classes.delete(name)},
  setAttribute: (key, value) => { attrs[key] = value; },
  removeAttribute: key => { delete attrs[key]; },
};
context.setDashPanelState(element, snore, null);
assert(classes.has('dash-panel-empty'));
assert(!classes.has('dash-panel-loading'));
assert.strictEqual(attrs['data-empty-label'], 'Data unavailable');
context.setDashPanelState(element, snore, {t: [1]});
assert(!classes.has('dash-panel-empty'));
assert(!('data-empty-label' in attrs));

vm.runInContext(getFunction('refreshStageAvailability'), context);

async function testStageRefresh() {
  let fetched = 0, rendered = 0;
  context.dashNoonDay = '20240115';
  context.dashLoadId = 1;
  context.dashSessions = [{id: 'a', has_stages: false}, {id: 'b', has_stages: false}];
  context.dashSessionData = [{}, {}];
  context.fetch = () => Promise.resolve({
    ok: true, json: () => Promise.resolve([
      {id: 'a', has_stages: true, stage_status: 'ready'},
      {id: 'b', has_stages: false, stage_status: 'absent'},
    ]),
  });
  context.fetchSntFile = () => { fetched++; return Promise.resolve('sst'); };
  context.parseSst = () => ({n: 1});
  context.renderDashboardCharts = () => { rendered++; };
  await context.refreshStageAvailability('20240115', 1);
  assert.strictEqual(fetched, 1, 'only newly available stages are fetched');
  assert.strictEqual(rendered, 1);
  assert(context.dashSessionData[0].stg);
  assert(!context.dashSessionData[1].stg);

  let finish;
  context.dashSessionData[0] = {};
  context.fetchSntFile = () => new Promise(resolve => { finish = resolve; });
  const pending = context.refreshStageAvailability('20240115', 1);
  await new Promise(resolve => setImmediate(resolve));
  assert(finish, 'stage download started');
  context.dashLoadId = 2;
  finish('sst');
  await pending;
  assert(!context.dashSessionData[0].stg, 'stale stage download is discarded');
}

testStageRefresh().then(() => {
  for (const name of ['fmtDriftMs', 'fmtTzOffset', 'renderStreamQuality']) {
    vm.runInContext(getFunction(name), context);
  }
  const sqEl = {
    style: {display: 'none'},
    classes: new Set(),
    classList: {
      add: c => sqEl.classes.add(c),
      remove: c => sqEl.classes.delete(c),
    },
    innerHTML: '',
  };
  context.document = {getElementById: id => (id === 'dash-stream-quality' ? sqEl : null)};

  // 1. Live session with large TZ mismatch (Squid case: +2h41m19s UTC, -18m41.2s local)
  context.dashSessions = [{
    fmt: 2, stream_notifications: 100, gap_events: 0, gap_missing: 0,
    start_epoch_ms: 1795616917000, end_epoch_ms: 1795624566000,
    clock_drift_valid: true, clock_drift_ms: 9678816,
    device_tz_offset_s: -7200, esp_tz_offset_s: -18000,
  }];
  context.renderStreamQuality();
  assert(sqEl.innerHTML.includes('+2h 41m 19s'), 'shows raw UTC drift');
  assert(sqEl.innerHTML.includes('CPAP TZ UTC-02:00 vs SomnoTrace UTC-05:00; local -18m 41.2s'),
         'annotates TZ mismatch and smaller local drift');

  // 2. Live session with small UTC drift and missed DST (1h TZ mismatch)
  context.dashSessions = [{
    fmt: 2, stream_notifications: 100, gap_events: 0, gap_missing: 0,
    start_epoch_ms: 1795616917000, end_epoch_ms: 1795624566000,
    clock_drift_valid: true, clock_drift_ms: 5000,
    device_tz_offset_s: 36000, esp_tz_offset_s: 39600,
  }];
  context.renderStreamQuality();
  assert(sqEl.innerHTML.includes('+5.0s'), 'keeps pure UTC drift');
  assert(sqEl.innerHTML.includes('CPAP TZ UTC+10:00 vs SomnoTrace UTC+11:00)'),
         'shows TZ mismatch without bogus local drift when local is larger');
  assert(!sqEl.innerHTML.includes('local '), 'omits local drift when |localDrift| >= |rtcDrift|');

  // 3. Live session with matching timezones
  context.dashSessions = [{
    fmt: 2, stream_notifications: 100, gap_events: 0, gap_missing: 0,
    start_epoch_ms: 1795616917000, end_epoch_ms: 1795624566000,
    clock_drift_valid: true, clock_drift_ms: 5000,
    device_tz_offset_s: -18000, esp_tz_offset_s: -18000,
  }];
  context.renderStreamQuality();
  assert(sqEl.innerHTML.includes('+5.0s'), 'shows matching TZ drift cleanly');
  assert(!sqEl.innerHTML.includes('CPAP TZ'), 'no TZ note when offsets match');

  // 4. SD-imported session preserves device_tz_delta_ms behaviour
  context.dashSessions = [{
    imported: true, fmt: 2, stream_notifications: 0, gap_events: 0, gap_missing: 0,
    clock_drift_valid: true, clock_drift_ms: 3605000, device_tz_delta_ms: 3600000,
    device_tz_offset_s: 36000, esp_tz_offset_s: 39600,
  }];
  context.renderStreamQuality();
  assert(sqEl.innerHTML.includes('+5.0s') && sqEl.innerHTML.includes('(device TZ +1h)'),
         'preserves existing SD import device_tz_delta_ms display');

  console.log('dashboard missing-data panels, stage refresh, and stream quality: passed');
}).catch(err => {
  console.error(err);
  process.exitCode = 1;
});
