// A config or build file is the source of truth for a project, and it is the one file the
// editor never opens: a `git checkout` or a build regenerating `build.ninja` rewrites it
// behind the server's back. `workspace/didChangeWatchedFiles` is the only notification that
// reports it, and its `Changed` events used to be discarded outright.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles } from './helpers.mjs';

const Created = 1, Changed = 2, Deleted = 3;

const LIB = 'fn watched_helper() -> i32 { 7 }\n';
const MAIN = 'fn main() -> i32 { watched_helper() }\n';

const config = (files) => JSON.stringify({
    'artic-config': '2.0',
    projects: [{ name: 'watched', folder: '.', files }],
    'default-project': 'watched',
}, null, 2);

describe('a config rewritten on disk while nothing has it open', () => {
    let client, ws, mainUri;

    before(async () => {
        client = new LspClient(findServerBinary());
        ws = stageFiles({
            'artic.json': config(['main.art']),   // lib.art deliberately left out
            'main.art': MAIN,
            'lib.art': LIB,
        });
        mainUri = ws.fileUri('main.art');
        await client.initialize(ws.uri);
        client.openDocument(mainUri, MAIN);
        await client.waitForDiagnostics(mainUri);
    });

    after(async () => {
        await client.stop();
        ws.cleanup();
    });

    test('starts out reporting the identifier the project does not include', () => {
        const messages = client.diagnosticsFor(mainUri).map((d) => d.message).join('\n');
        assert.match(messages, /watched_helper/,
            'the fixture must start out broken, or the test proves nothing');
    });

    test('is re-read, and the project it describes is rebuilt', async () => {
        ws.write('artic.json', config(['main.art', 'lib.art']));
        client.clearDiagnosticsLog();
        client.changeWatchedFiles([{ uri: ws.fileUri('artic.json'), type: Changed }]);

        await client.waitForDiagnostics(mainUri);
        assert.deepEqual(client.diagnosticsFor(mainUri), [],
            'lib.art is part of the project now, so main.art resolves');
    });
});

describe('a build file rewritten on disk', () => {
    let client, ws, mainUri;

    // Only `build.ninja` describes this project -- there is no artic.json to fall back on.
    const ninja = (sources) => [
        'ninja_required_version = 1.11',
        'build out.ll | ${cmake_ninja_workdir}out.ll: CUSTOM_COMMAND',
        `  COMMAND = artic ${sources.map((s) => ws.path(s).replace(/\\/g, '/')).join(' ')} -emit-llvm -o out.ll`,
        '',
    ].join('\n');

    before(async () => {
        client = new LspClient(findServerBinary());
        ws = stageFiles({ 'main.art': MAIN, 'lib.art': LIB });
        ws.write('build.ninja', ninja(['main.art']));
        mainUri = ws.fileUri('main.art');
        await client.initialize(ws.uri);
        client.openDocument(mainUri, MAIN);
        await client.waitForDiagnostics(mainUri);
    });

    after(async () => {
        await client.stop();
        ws.cleanup();
    });

    test('is re-read too, not only artic.json', async () => {
        assert.match(client.diagnosticsFor(mainUri).map((d) => d.message).join('\n'), /watched_helper/);

        ws.write('build.ninja', ninja(['main.art', 'lib.art']));
        client.clearDiagnosticsLog();
        client.changeWatchedFiles([{ uri: ws.fileUri('build.ninja'), type: Changed }]);

        await client.waitForDiagnostics(mainUri);
        assert.deepEqual(client.diagnosticsFor(mainUri), []);
    });
});

describe('a source file changing on disk', () => {
    let client, ws, mainUri;

    before(async () => {
        client = new LspClient(findServerBinary());
        ws = stageFiles({
            'artic.json': config(['*.art']),
            'main.art': MAIN,
            'lib.art': LIB,
        });
        mainUri = ws.fileUri('main.art');
        await client.initialize(ws.uri);
        client.openDocument(mainUri, MAIN);
        await client.waitForDiagnostics(mainUri);
        // The first compile publishes for every file in the project, and lib.art's
        // notification can arrive after main.art's. Without waiting for it, it lands in the
        // log the test below clears and reads as a reload the server never did.
        await client.waitForDiagnostics(ws.fileUri('lib.art'));
    });

    after(async () => {
        await client.stop();
        ws.cleanup();
    });

    test('does not reload the workspace', async () => {
        assert.deepEqual(client.diagnosticsFor(mainUri), []);
        client.clearDiagnosticsLog();

        // The editor owns this buffer; didChange is what carries its content. Treating the
        // watcher echo as a config change would rebuild every project on every keystroke.
        client.changeWatchedFiles([{ uri: ws.fileUri('lib.art'), type: Changed }]);
        await client.settle();

        assert.deepEqual(client.diagnosticsLog, [],
            'a changed source file is already covered by didChange/didSave');
    });

    test('is reloaded when one appears or disappears, because a glob may now match it', async () => {
        ws.write('extra.art', 'fn watched_extra() -> i32 { 1 }\n');
        client.clearDiagnosticsLog();
        client.changeWatchedFiles([{ uri: ws.fileUri('extra.art'), type: Created }]);

        await client.waitForDiagnostics(mainUri);
        assert.deepEqual(client.diagnosticsFor(mainUri), []);
    });
});
