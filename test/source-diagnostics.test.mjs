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

    test('reports an unused local binding as a warning', async () => {
        const uri = ws.fileUri('src', 'unused_local.art');
        client.openDocument(uri, ws.read('src', 'unused_local.art'));
        await client.settle();

        // Not waitForDiagnostics: the file shares a project with type_error.art, so an
        // empty publish for it has already been logged by the earlier tests.
        const published = client.diagnosticsFor(uri);
        const unused = published.filter(d => /unused identifier 'unused_local'/.test(d.message));
        assert.equal(unused.length, 1,
            `expected the unused-identifier warning, got ${JSON.stringify(published)}`);
        assert.equal(unused[0].severity, 2, 'must be reported as a warning');
        // `unused_local` sits on source line 8 (1-based), column 9.
        assert.equal(unused[0].range.start.line, 7);
        assert.equal(unused[0].range.start.character, 8);
    });
});
