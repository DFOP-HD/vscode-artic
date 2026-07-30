import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFixture } from './helpers.mjs';

describe('source diagnostics', () => {
    let ws;
    let client;

    before(async () => {
        ws = stageFixture('diagnostics');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('reports the type error at the right file and position', async () => {
        const uri = ws.fileUri('src', 'type_error.art');
        client.openDocument(uri, ws.read('src', 'type_error.art'));

        const published = await client.waitForDiagnostics(uri);
        assert.equal(published.diagnostics.length, 1,
            `expected exactly one diagnostic, got ${JSON.stringify(published.diagnostics)}`);

        const [diag] = published.diagnostics;
        assert.match(diag.message, /expected type 'i32', but got type 'f32'/);
        // `x + y` sits on source line 7 (1-based), column 9.
        assert.equal(diag.range.start.line, 6);
        assert.equal(diag.range.start.character, 8);
        assert.equal(diag.severity, 1, 'must be reported as an error');
    });

    test('publishes diagnostics under a well-formed file URI', async () => {
        const uri = ws.fileUri('src', 'type_error.art');
        const published = await client.waitForDiagnostics(uri);
        // MSVC builds used to emit native separators encoded as %5C, which no editor
        // can match against the document it opened.
        assert.ok(!published.uri.includes('%5C'),
            `URI must not contain encoded backslashes: ${published.uri}`);
        assert.equal(normalizeUri(published.uri), normalizeUri(uri));
    });

    test('does not attribute a sibling file\'s error to a healthy file', async () => {
        const healthyUri = ws.fileUri('src', 'healthy.art');
        client.openDocument(healthyUri, ws.read('src', 'healthy.art'));
        await client.settle();

        const healthy = client.diagnosticsFor(healthyUri);
        assert.deepEqual(healthy, [],
            `healthy.art must have no diagnostics, got ${JSON.stringify(healthy)}`);
    });

    test('clears diagnostics once the error is fixed', async () => {
        const uri = ws.fileUri('src', 'type_error.art');
        const fixed = ws.read('src', 'type_error.art').replace('x + y', 'x + (y as i32)');

        client.changeDocument(uri, fixed);
        await client.settle();

        const remaining = client.diagnosticsFor(uri);
        assert.deepEqual(remaining, [],
            `diagnostics must be cleared after the fix, got ${JSON.stringify(remaining)}`);
    });
});
