// Includes marked optional with a trailing `?` may be absent. This matters because the
// "Detect workspace configuration" command writes generated build files (solutions,
// ninja files, vcxproj files) into artic.json, and those only exist after a build.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFiles, fixturesDir, locate } from './helpers.mjs';

const serverPath = findServerBinary();
const geometryText = readFileSync(join(fixturesDir, 'simple', 'src', 'geometry.art'), 'utf8');
const mainText = readFileSync(join(fixturesDir, 'simple', 'src', 'main.art'), 'utf8');

const isError = (d) => d.severity === 1;

function config(includes) {
    return JSON.stringify({ 'artic-config': '2.0', include: includes }, null, 4);
}

describe('optional includes', () => {
    let client;

    before(async () => {
        client = new LspClient(serverPath);
        await client.initialize(null);
    });

    after(async () => {
        await client?.stop();
    });

    test('a missing optional include of any supported type is silent', async () => {
        const ws = stageFiles({
            'artic.json': config([
                'build/missing.json?',
                'build/missing.vcxproj?',
                'build/missing.sln?',
                'build/build.ninja?',
            ]),
            'src/main.art': 'fn main() -> i32 { 0 }\n',
        });
        try {
            const configUri = ws.fileUri('artic.json');
            const sourceUri = ws.fileUri('src', 'main.art');
            client.openDocument(configUri, ws.read('artic.json'), 'json');
            client.openDocument(sourceUri, ws.read('src', 'main.art'));
            // Opening the source drives the lazy lookup that walks the includes a second
            // time; a missing optional include used to be reported from there.
            await client.waitForDiagnostics(sourceUri);
            await client.settle();

            assert.deepEqual(client.diagnosticsFor(configUri), [],
                'a missing optional include must not be reported');
            for (const name of ['missing.json', 'missing.vcxproj', 'missing.sln', 'build.ninja']) {
                assert.deepEqual(client.diagnosticsFor(ws.fileUri('build', name)), [],
                    `nothing should be published against ${name}`);
            }
        } finally {
            ws.cleanup();
        }
    });

    test('a missing non-optional include is still an error', async () => {
        const ws = stageFiles({
            'artic.json': config(['build/missing.sln']),
            'src/main.art': 'fn main() -> i32 { 0 }\n',
        });
        try {
            const configUri = ws.fileUri('artic.json');
            client.openDocument(configUri, ws.read('artic.json'), 'json');
            client.openDocument(ws.fileUri('src', 'main.art'), ws.read('src', 'main.art'));
            await client.waitForDiagnostics(ws.fileUri('src', 'main.art'));
            await client.settle();

            const errors = client.diagnosticsFor(configUri).filter(isError);
            assert.equal(errors.length, 1, `expected exactly one error, got ${JSON.stringify(errors)}`);
            assert.match(errors[0].message, /does not exist/);
        } finally {
            ws.cleanup();
        }
    });

    test('the `?` is not part of the path', async () => {
        const ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
            'artic.json': config(['build/build.ninja?']),
        });
        ws.write('build/build.ninja', [
            'build src/demo.ll: CUSTOM_COMMAND',
            `  COMMAND = artic ${['src/geometry.art', 'src/main.art'].map((f) => ws.path(f)).join(' ')} -o demo`,
        ].join('\n'));
        try {
            const mainUri = ws.fileUri('src', 'main.art');
            client.openDocument(ws.fileUri('src', 'geometry.art'), geometryText);
            client.openDocument(mainUri, mainText);
            await client.waitForDiagnostics(mainUri);

            const result = await client.request('textDocument/definition', {
                textDocument: { uri: mainUri },
                position: locate(mainText, 'dot(v, v)'),
            });
            const locations = Array.isArray(result) ? result : [result];
            assert.ok(locations.length > 0 && locations[0],
                'an optional include that exists must be loaded like any other');
            assert.equal(normalizeUri(locations[0].uri),
                normalizeUri(ws.fileUri('src', 'geometry.art')));
        } finally {
            ws.cleanup();
        }
    });

    test('optional means "may be absent", not "ignore its errors"', async () => {
        const ws = stageFiles({
            'artic.json': config(['build/broken.json?']),
            'build/broken.json': '{ "artic-config": "2.0", "bogus-key": 1 }',
            'src/main.art': 'fn main() -> i32 { 0 }\n',
        });
        try {
            client.openDocument(ws.fileUri('artic.json'), ws.read('artic.json'), 'json');
            client.openDocument(ws.fileUri('src', 'main.art'), ws.read('src', 'main.art'));
            await client.waitForDiagnostics(ws.fileUri('src', 'main.art'));
            await client.settle();

            const messages = client.diagnosticsFor(ws.fileUri('build', 'broken.json')).map((d) => d.message);
            assert.ok(messages.some((m) => /unknown json property/.test(m)),
                `expected the broken include to be reported, got ${JSON.stringify(messages)}`);
        } finally {
            ws.cleanup();
        }
    });
});
