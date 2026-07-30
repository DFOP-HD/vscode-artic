// Guards the .art fixtures (and the examples in the artic-language skill) against
// drifting out of sync with the real compiler.

import { test, describe } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { join } from 'node:path';

import { findArticBinary, fixturesDir } from './helpers.mjs';

const artic = findArticBinary();

function compile(...files) {
    try {
        execFileSync(artic, ['--no-color', ...files], { encoding: 'utf8', stdio: 'pipe' });
        return { ok: true, output: '' };
    } catch (err) {
        return { ok: false, output: `${err.stdout ?? ''}${err.stderr ?? ''}` };
    }
}

describe('fixture validity', { skip: artic ? false : 'artic compiler not built' }, () => {
    test('the artic-language skill examples compile', () => {
        const result = compile(join(fixturesDir, 'skill-examples.art'));
        assert.ok(result.ok, `skill examples must compile:\n${result.output}`);
    });

    test('the simple project compiles', () => {
        const dir = join(fixturesDir, 'simple', 'src');
        const result = compile(join(dir, 'geometry.art'), join(dir, 'main.art'));
        assert.ok(result.ok, `simple fixture must compile:\n${result.output}`);
    });

    test('the type error fixture fails with exactly the asserted diagnostic', () => {
        const dir = join(fixturesDir, 'diagnostics', 'src');
        const result = compile(join(dir, 'healthy.art'), join(dir, 'type_error.art'));
        assert.equal(result.ok, false, 'type_error.art must not compile');
        assert.match(result.output, /expected type 'i32', but got type 'f32'/);
        assert.match(result.output, /type_error\.art:7:9-7:10/);
        assert.match(result.output, /1 error\(s\)/);
    });
});
