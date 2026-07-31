// Diagnostics produced by the workspace configuration (artic.json / .artic-lsp),
// as opposed to diagnostics from compiling .art sources.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFiles } from './helpers.mjs';

const serverPath = findServerBinary();

/** Waits for a diagnostics notification for `uri` that satisfies `predicate`. */
function waitForDiagnosticsMatching(client, uri, predicate, description) {
    const wanted = normalizeUri(uri);
    if (client.diagnostics.has(wanted) && predicate(client.diagnosticsFor(uri))) {
        return Promise.resolve(client.diagnosticsFor(uri));
    }
    return client
        .waitForNotification(
            (m) =>
                m.method === 'textDocument/publishDiagnostics' &&
                normalizeUri(m.params.uri) === wanted &&
                predicate(m.params.diagnostics),
            { timeout: 10000, description },
        )
        .then((m) => m.params.diagnostics);
}

const isError = (d) => d.severity === 1;

describe('config diagnostics', () => {
    let client;
    let ws;

    before(async () => {
        client = new LspClient(serverPath);
        await client.initialize(null);
    });

    after(async () => {
        await client?.stop();
    });

    test('reports an unknown property on the config file itself', async () => {
        ws = stageFiles({
            'artic.json': JSON.stringify(
                { 'artic-config': '2.0', 'bogus-key': 1, projects: [{ name: 'p', files: ['*.art'] }] },
                null,
                2,
            ),
            'a.art': 'fn main() -> i32 { 0 }\n',
        });
        const uri = ws.fileUri('artic.json');
        client.openDocument(uri, ws.read('artic.json'), 'json');

        const diags = await waitForDiagnosticsMatching(
            client, uri, (d) => d.some((x) => /unknown json property/.test(x.message)),
            'unknown property diagnostic',
        );

        const diag = diags.find((d) => /unknown json property/.test(d.message));
        assert.ok(diag, `expected an unknown-property diagnostic, got ${JSON.stringify(diags)}`);
        assert.match(diag.message, /bogus-key/);
        assert.ok(diag.range.start.line > 0, 'diagnostic should point at the property, not line 0');
        ws.cleanup();
    });

    test('clears config diagnostics once the config is fixed', async () => {
        // No "projects" key, so a valid config produces no messages at all and the
        // published list must go empty rather than keep the previous error.
        ws = stageFiles({
            'artic.json': JSON.stringify({ 'artic-config': '2.0', 'bogus-key': 1 }, null, 2),
        });
        const uri = ws.fileUri('artic.json');
        client.openDocument(uri, '', 'json');
        await waitForDiagnosticsMatching(
            client, uri, (d) => d.some(isError), 'initial config error',
        );

        const fixed = JSON.stringify({ 'artic-config': '2.0' }, null, 2);
        ws.write('artic.json', fixed);
        client.saveDocument(uri, fixed);

        const cleared = await waitForDiagnosticsMatching(
            client, uri, (d) => d.length === 0, 'cleared config diagnostics',
        );
        assert.deepEqual(cleared, []);
        ws.cleanup();
    });

    test('drops the error but keeps the file-count hint when a project config is fixed', async () => {
        ws = stageFiles({
            'artic.json': JSON.stringify(
                { 'artic-config': '2.0', 'bogus-key': 1, projects: [{ name: 'p', files: ['*.art'] }] },
                null,
                2,
            ),
            'a.art': 'fn main() -> i32 { 0 }\n',
        });
        const uri = ws.fileUri('artic.json');
        client.openDocument(uri, '', 'json');
        await waitForDiagnosticsMatching(
            client, uri, (d) => d.some(isError), 'initial config error',
        );

        const fixed = JSON.stringify(
            { 'artic-config': '2.0', projects: [{ name: 'p', files: ['*.art'] }] }, null, 2,
        );
        ws.write('artic.json', fixed);
        client.saveDocument(uri, fixed);

        const after = await waitForDiagnosticsMatching(
            client, uri, (d) => !d.some(isError), 'config error resolved',
        );
        assert.equal(after.filter(isError).length, 0);
        ws.cleanup();
    });

    test('attributes a missing include to the config that includes it', async () => {
        ws = stageFiles({
            'artic.json': JSON.stringify(
                { 'artic-config': '2.0', include: ['missing.json'], projects: [{ name: 'p', files: ['*.art'] }] },
                null,
                2,
            ),
            'a.art': 'fn main() -> i32 { 0 }\n',
        });
        const uri = ws.fileUri('artic.json');
        client.openDocument(uri, '', 'json');

        const diags = await waitForDiagnosticsMatching(
            client, uri, (d) => d.some((x) => /does not exist/.test(x.message)),
            'missing include diagnostic',
        );
        const diag = diags.find((d) => /does not exist/.test(d.message));
        assert.match(diag.message, /missing\.json/);
        assert.equal(
            client.diagnosticsFor(ws.fileUri('missing.json')).length, 0,
            'nothing should be published against the file that does not exist',
        );
        ws.cleanup();
    });

    test('recognises a .artic-lsp config file', async () => {
        ws = stageFiles({
            '.artic-lsp': JSON.stringify(
                { 'artic-config': '2.0', 'bogus-key': 1, projects: [{ name: 'p', files: ['*.art'] }] },
                null,
                2,
            ),
            'a.art': 'fn main() -> i32 { 0 }\n',
        });
        const uri = ws.fileUri('.artic-lsp');
        client.openDocument(uri, '', 'json');

        const diags = await waitForDiagnosticsMatching(
            client, uri, (d) => d.some((x) => /unknown json property/.test(x.message)),
            '.artic-lsp config diagnostic',
        );
        assert.ok(diags.some((d) => /bogus-key/.test(d.message)));
        ws.cleanup();
    });

    // The README documents that artic.json is plain JSON. Older examples used
    // JSON-with-comments, which silently made the whole config unusable.
    test('rejects a config file containing comments', async () => {
        ws = stageFiles({
            'artic.json':
                '{\n' +
                '    // the config format does not allow comments\n' +
                '    "artic-config": "2.0",\n' +
                '    "projects": [{ "name": "p", "files": ["*.art"] }]\n' +
                '}\n',
            'a.art': 'fn main() -> i32 { 0 }\n',
        });
        const uri = ws.fileUri('artic.json');
        client.openDocument(uri, ws.read('artic.json'), 'json');

        const diags = await waitForDiagnosticsMatching(
            client, uri, (d) => d.some(isError), 'parse error for a commented config',
        );
        assert.ok(
            diags.some((d) => /Failed to parse json/.test(d.message)),
            `expected a json parse error, got ${JSON.stringify(diags)}`,
        );
        ws.cleanup();
    });
});
