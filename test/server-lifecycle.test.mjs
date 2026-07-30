import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture } from './helpers.mjs';

describe('server lifecycle', () => {
    let ws;
    let client;
    let capabilities;

    before(async () => {
        ws = stageFixture('simple');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        ({ capabilities } = await client.initialize(ws.uri));
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('completes the initialize handshake', () => {
        assert.ok(capabilities, 'initialize must return capabilities');
    });

    test('advertises the documented feature set', () => {
        assert.ok(capabilities.textDocumentSync, 'textDocumentSync');
        assert.ok(capabilities.completionProvider, 'completionProvider');
        assert.equal(capabilities.definitionProvider, true);
        assert.equal(capabilities.referencesProvider, true);
        assert.ok(capabilities.renameProvider, 'renameProvider');
        assert.equal(capabilities.renameProvider.prepareProvider, true);
        assert.ok(capabilities.semanticTokensProvider, 'semanticTokensProvider');
    });

    test('responds to requests after initialization', async () => {
        const uri = ws.fileUri('src', 'main.art');
        client.openDocument(uri, ws.read('src', 'main.art'));
        const result = await client.request('textDocument/completion', {
            textDocument: { uri },
            position: { line: 0, character: 0 },
        });
        assert.ok(result !== undefined, 'server must answer completion requests');
    });
});
