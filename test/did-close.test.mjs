// Covers textDocument/didClose: the closed document's diagnostics are withdrawn, and the
// unsaved buffer it carried is dropped so the rest of the project is recompiled from disk.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles } from './helpers.mjs';

const config = JSON.stringify({
    'artic-config': '2.0',
    projects: [{ name: 'did-close', files: ['src/**/*.art'] }],
}, null, 4);

const LIB_OK = 'fn helper() -> i32 { 40 }\n';
const LIB_BROKEN = 'fn helper() -> bool { true }\n';
const MAIN = 'fn main() -> i32 { helper() + 2 }\n';
const BROKEN_ON_DISK = 'fn oops() -> i32 { true }\n';

describe('didClose', () => {
    let ws;
    let client;
    let libUri;
    let mainUri;
    let brokenUri;

    before(async () => {
        ws = stageFiles({
            'artic.json': config,
            'src/lib.art': LIB_OK,
            'src/main.art': MAIN,
            'src/broken.art': BROKEN_ON_DISK,
        });
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);

        libUri = ws.fileUri('src', 'lib.art');
        mainUri = ws.fileUri('src', 'main.art');
        brokenUri = ws.fileUri('src', 'broken.art');
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('withdraws the diagnostics of a document that is closed while still broken', async () => {
        client.openDocument(brokenUri, BROKEN_ON_DISK);
        await client.waitForDiagnostics(brokenUri);
        await client.settle();
        assert.ok(client.diagnosticsFor(brokenUri).length > 0,
            'broken.art must report an error to begin with');

        client.closeDocument(brokenUri);
        await client.settle();
        assert.deepEqual(client.diagnosticsFor(brokenUri), [],
            'closing must withdraw the diagnostics');
    });

    test('drops the unsaved buffer, so other documents are recompiled from disk', async () => {
        client.openDocument(libUri, LIB_OK);
        client.openDocument(mainUri, MAIN);
        await client.waitForDiagnostics(mainUri);
        await client.settle();
        assert.deepEqual(client.diagnosticsFor(mainUri), [], 'main.art must start out healthy');

        // Break main.art by editing lib.art without ever saving it.
        client.changeDocument(libUri, LIB_BROKEN);
        await client.settle();
        assert.ok(client.diagnosticsFor(mainUri).length > 0,
            'the unsaved edit in lib.art must break main.art');

        // Closing lib.art discards that edit, so main.art has to go back to healthy.
        client.closeDocument(libUri);
        await client.settle();
        assert.deepEqual(client.diagnosticsFor(mainUri), [],
            'main.art must be recompiled against the on-disk lib.art');
        assert.deepEqual(client.diagnosticsFor(libUri), [],
            'the closed document keeps no diagnostics of its own');
    });

    test('closing a document that was never edited leaves the server usable', async () => {
        client.openDocument(mainUri, MAIN);
        await client.settle();

        client.closeDocument(mainUri);
        await client.settle();
        assert.deepEqual(client.diagnosticsFor(mainUri), []);

        client.openDocument(mainUri, MAIN);
        const symbols = await client.request('textDocument/documentSymbol', {
            textDocument: { uri: mainUri },
        });
        assert.ok(Array.isArray(symbols) && symbols.some((s) => s.name === 'main'));
    });
});
