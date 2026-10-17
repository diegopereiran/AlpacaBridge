// AlpacaHTTP
// Copyright (c) 2025-2026 Joey Troy and contributors
//
// This file is part of AlpacaHTTP.
//
// AlpacaHTTP is licensed under the GNU Affero General Public License,
// version 3 or (at your option) any later version (AGPL-3.0-or-later),
// with an additional permission allowing combination with proprietary
// device-vendor SDKs. See the LICENSE file in this repository for the full
// license text and the vendor-SDK linking exception, or the license online at:
// https://www.gnu.org/licenses/agpl-3.0.html

// Camera picker of a camera-attached thermal switch (thermalCameraOptions /
// Selection / Hint in format.js). The rig shape that started it: an ASI290MM
// Mini (no heater) at index 0 beside an ASI2600MM Pro (heater) at index 1. The
// form always asks: it never preselects a camera for a new switch.
//
// Run: node --test AlpacaHTTP/tests/web/thermal-camera.test.js

const test = require('node:test');
const assert = require('node:assert');
const path = require('node:path');

const { thermalCameraOptions, thermalCameraSelection, thermalCameraHint } =
    require(path.join(__dirname, '..', '..', 'web', 'format.js'));

const MINI = { Index: 0, Name: 'ZWO ASI290MM Mini', Serial: '2d0bd20224000900', HasThermal: false };
const PRO = { Index: 1, Name: 'ZWO ASI2600MM Pro', Serial: '2e18870f28000900', HasThermal: true };
const MC = { Index: 2, Name: 'ZWO ASI2600MC Pro', Serial: '', HasThermal: true };

test('a new switch preselects nothing, even with one heater camera', () => {
    const model = thermalCameraOptions([MINI, PRO], null, 'dew heater', '', []);
    assert.strictEqual(model.selected, '');
    assert.deepStrictEqual(model.options[0],
                           { value: '', label: 'Choose the camera this dew heater is on', disabled: true });
    assert.strictEqual(thermalCameraSelection(model.selected, [MINI, PRO], null), null);
    assert.strictEqual(thermalCameraHint([MINI, PRO], '', 'dew heater', '', null, []),
                       'Choose the camera this dew heater is on.');
});

test('heater cameras come first and the others cannot be picked', () => {
    const model = thermalCameraOptions([MINI, PRO, MC], null, 'dew heater', '', []);
    assert.deepStrictEqual(model.options.map(o => [o.value, o.disabled]),
                           [['', true], ['cam:1', false], ['cam:2', false], ['cam:0', true]]);
    assert.strictEqual(model.options[1].label, 'ZWO ASI2600MM Pro (index 1), has a dew heater');
    assert.strictEqual(model.options[3].label, 'ZWO ASI290MM Mini (index 0), no dew heater');
    // A disabled camera is never kept as the choice.
    assert.strictEqual(thermalCameraOptions([MINI, PRO], null, 'dew heater', 'cam:0', []).selected, '');
});

test('a camera another switch uses is labelled', () => {
    const taken = [{ cameraIndex: 1, serialNumber: '2e18870f28000900' }];
    const model = thermalCameraOptions([MINI, PRO, MC], null, 'dew heater', '', taken);
    assert.strictEqual(model.selected, '');
    assert.strictEqual(model.options[1].label, 'ZWO ASI2600MM Pro (index 1), has a dew heater, used by another switch');
    assert.strictEqual(thermalCameraHint([MINI, PRO, MC], 'cam:1', 'dew heater', '', null, taken),
                       'Another switch already uses ZWO ASI2600MM Pro.');
    assert.strictEqual(thermalCameraHint([MINI, PRO, MC], 'cam:2', 'dew heater', '', null, taken), '');
});

test('picking a camera sends its index and serial', () => {
    assert.deepStrictEqual(thermalCameraSelection('cam:1', [MINI, PRO], null),
                           { cameraIndex: 1, serialNumber: '2e18870f28000900' });
    assert.deepStrictEqual(thermalCameraSelection('cam:2', [MINI, PRO, MC], null), { cameraIndex: 2 });
});

test('editing selects the saved heater camera by serial even after it moved index', () => {
    const moved = [{ ...PRO, Index: 0 }, { ...MINI, Index: 1 }];
    const saved = { cameraIndex: 1, serialNumber: '2e18870f28000900' };
    assert.strictEqual(thermalCameraOptions(moved, saved, 'dew heater', '', []).selected, 'cam:0');
    assert.strictEqual(thermalCameraOptions([MINI, PRO], { cameraIndex: 1 }, 'dew heater', '', []).selected, 'cam:1');
});

test('editing a switch saved as Auto or on a camera without the heater asks again', () => {
    const auto = { cameraIndex: -1 };
    assert.strictEqual(thermalCameraOptions([MINI, PRO], auto, 'dew heater', '', []).selected, '');
    assert.strictEqual(thermalCameraHint([MINI, PRO], '', 'dew heater', '', auto, []),
                       'This switch was saved as Auto. Choose the camera this dew heater is on.');
    const onMini = { cameraIndex: 0 };
    assert.strictEqual(thermalCameraOptions([MINI, PRO], onMini, 'dew heater', '', []).selected, '');
    assert.strictEqual(thermalCameraHint([MINI, PRO], '', 'dew heater', '', onMini, []),
                       'The saved camera, ZWO ASI290MM Mini (index 0), has no dew heater. ' +
                       'Choose the camera this dew heater is on.');
});

test('a saved camera that is not attached is kept as it was', () => {
    const saved = { cameraIndex: 1, serialNumber: 'ffff' };
    const model = thermalCameraOptions([MINI], saved, 'dew heater', '', []);
    assert.strictEqual(model.selected, 'saved');
    assert.strictEqual(model.options[model.options.length - 1].label, 'Saved camera, serial ffff (not attached)');
    assert.deepStrictEqual(thermalCameraSelection('saved', [MINI], saved), { cameraIndex: 1, serialNumber: 'ffff' });
});

test('the user choice survives a refresh unless that camera is gone', () => {
    assert.strictEqual(thermalCameraOptions([MINI, PRO, MC], null, 'dew heater', 'cam:2', []).selected, 'cam:2');
    assert.strictEqual(thermalCameraOptions([MINI, PRO], null, 'dew heater', 'cam:2', []).selected, '');
});

test('no heater camera attached says so', () => {
    assert.match(thermalCameraHint([MINI], '', 'dew heater', '', null, []),
                 /No attached camera reports a dew heater\. Plug the camera in, then press Refresh\./);
});

test('a failed listing still asks, offering the first indexes', () => {
    const model = thermalCameraOptions(null, null, 'dew heater or fan', '', []);
    assert.deepStrictEqual(model.options.map(o => o.value), ['', 'cam:0', 'cam:1', 'cam:2', 'cam:3']);
    assert.strictEqual(model.selected, '');
    assert.strictEqual(thermalCameraOptions(null, { cameraIndex: 2 }, 'dew heater or fan', '', []).selected, 'cam:2');
    assert.match(thermalCameraHint(null, '', 'dew heater or fan', 'HTTP 500', null, []),
                 /Could not list the attached cameras: HTTP 500\. Pick the index/);
});
