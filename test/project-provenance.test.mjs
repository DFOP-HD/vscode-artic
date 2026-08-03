// Which project a file is compiled in, and where that answer came from.
//
// The server falls back to compiling a file on its own when it finds no configuration
// above it. That fallback is correct — a directory of artic sources is very often a set of
// independent single-file programs — but it used to be completely silent, so every
// cross-file reference turned into "unknown identifier" with nothing saying why.
// `artic.projectForFile` is what makes it explainable, and the editor's status bar reads it.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { resolve } from 'node:path';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture, stageFiles } from './helpers.mjs';

const projectForFile = (client, uri) =>
    client.request('workspace/executeCommand', {
        command: 'artic.projectForFile',
        arguments: [uri],
    });

// The server folds the drive letter to lower case, so paths cannot be compared literally.
const samePath = (a, b) => resolve(a).toLowerCase() === resolve(b).toLowerCase();

describe('project provenance', () => {
    describe('a file a configuration lists', () => {
        let ws, client, capabilities;

        before(async () => {
            ws = stageFixture('navigation');
            client = new LspClient(findServerBinary(), { cwd: ws.dir });
            const result = await client.initialize(ws.uri);
            capabilities = result.capabilities;
        });
        after(async () => { await client?.stop(); ws?.cleanup(); });

        test('the command is advertised', () => {
            assert.deepEqual(capabilities.executeCommandProvider?.commands, ['artic.projectForFile']);
        });

        test('reports the project, the config that declared it, and the file count', async () => {
            const uri = ws.fileUri('src', 'uses.art');
            const info = await projectForFile(client, uri);
            assert.equal(info.provenance, 'config');
            assert.equal(info.name, 'navigation');
            assert.ok(samePath(info.origin, ws.path('artic.json')),
                      `expected ${info.origin} to be ${ws.path('artic.json')}`);
            // src/*.art, all three of them.
            assert.equal(info.fileCount, 3);
        });

        test('answers for a file that was never opened', async () => {
            // The status bar asks about whatever the editor is showing, which need not be a
            // document the server has compiled.
            const info = await projectForFile(client, ws.fileUri('src', 'shapes.art'));
            assert.equal(info.provenance, 'config');
            assert.equal(info.name, 'navigation');
        });
    });

    describe('a file no configuration covers', () => {
        let ws, client;

        before(async () => {
            ws = stageFiles({
                'lonely.art': 'fn main() -> i32 { helper() }\n',
                'helper.art': 'fn helper() -> i32 { 1 }\n',
            });
            client = new LspClient(findServerBinary(), { cwd: ws.dir });
            await client.initialize(ws.uri);
        });
        after(async () => { await client?.stop(); ws?.cleanup(); });

        test('is reported as a single-file compile', async () => {
            const info = await projectForFile(client, ws.fileUri('lonely.art'));
            assert.equal(info.provenance, 'single-file');
            assert.equal(info.name, '');
            assert.equal(info.origin, '');
            assert.equal(info.fileCount, 1);
        });

        test('and that really is why the sibling is invisible', async () => {
            // The guard that keeps the message honest: if a future implicit project makes
            // these two compile together, this assertion is what says the report must change.
            client.openDocument(ws.fileUri('lonely.art'), ws.read('lonely.art'));
            const { diagnostics } = await client.waitForDiagnostics(ws.fileUri('lonely.art'));
            assert.ok(diagnostics.some((d) => d.severity === 1),
                      'expected the unresolved call to helper() to be an error');
        });
    });

    describe('a file only the default project covers', () => {
        let ws, client;

        before(async () => {
            ws = stageFiles({
                'artic.json': JSON.stringify({
                    'artic-config': '2.0',
                    projects: [{ name: 'provenance-lib', files: ['lib/*.art'] }],
                    'default-project': { name: 'provenance-default', dependencies: ['provenance-lib'], files: [] },
                }),
                'lib/lib.art': 'fn shared() -> i32 { 7 }\n',
                'app/app.art': 'fn app() -> i32 { shared() }\n',
            });
            client = new LspClient(findServerBinary(), { cwd: ws.dir });
            await client.initialize(ws.uri);
        });
        after(async () => { await client?.stop(); ws?.cleanup(); });

        test('is reported as the default project, counting itself', async () => {
            const info = await projectForFile(client, ws.fileUri('app', 'app.art'));
            assert.equal(info.provenance, 'default-project');
            assert.equal(info.name, 'provenance-default');
            // The default project lists no files of its own; it inherits lib.art and the
            // file being asked about is compiled alongside it.
            assert.equal(info.fileCount, 2);
        });

        test('a file the library lists is reported against the library', async () => {
            const info = await projectForFile(client, ws.fileUri('lib', 'lib.art'));
            assert.equal(info.provenance, 'config');
            assert.equal(info.name, 'provenance-lib');
            assert.equal(info.fileCount, 1);
        });
    });

    describe('bad input', () => {
        let ws, client;

        before(async () => {
            ws = stageFixture('navigation');
            client = new LspClient(findServerBinary(), { cwd: ws.dir });
            await client.initialize(ws.uri);
        });
        after(async () => { await client?.stop(); ws?.cleanup(); });

        test('an unknown command answers null rather than failing', async () => {
            assert.equal(await client.request('workspace/executeCommand', { command: 'artic.nope' }), null);
        });

        test('a missing argument answers null rather than crashing the server', async () => {
            assert.equal(await projectForFile(client, undefined), null);
            // The session must still be alive.
            const info = await projectForFile(client, ws.fileUri('src', 'uses.art'));
            assert.equal(info.name, 'navigation');
        });
    });
});
