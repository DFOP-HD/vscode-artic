// Covers the request types the editor uses for everything other than diagnostics:
// semantic tokens, inlay hints, go-to-definition and find-references.
//
// These were previously untested, which is how a regression that left every one of
// them returning nothing reached a user.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFixture, locate } from './helpers.mjs';

describe('language features', () => {
    let ws;
    let client;
    let mainUri;
    let mainText;
    let geometryUri;
    let geometryText;

    before(async () => {
        ws = stageFixture('simple');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);

        mainUri = ws.fileUri('src', 'main.art');
        mainText = ws.read('src', 'main.art');
        geometryUri = ws.fileUri('src', 'geometry.art');
        geometryText = ws.read('src', 'geometry.art');

        client.openDocument(geometryUri, geometryText);
        client.openDocument(mainUri, mainText);
        await client.waitForDiagnostics(mainUri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('semanticTokens/full returns tokens for an open source file', async () => {
        const result = await client.request('textDocument/semanticTokens/full', {
            textDocument: { uri: mainUri },
        });
        assert.ok(result, 'server must not answer with null for a compiled file');
        assert.ok(Array.isArray(result.data), 'result must carry an encoded token array');
        assert.ok(result.data.length > 0, 'main.art must produce at least one semantic token');
        assert.equal(result.data.length % 5, 0,
            'LSP requires five integers per token');
    });

    test('semanticTokens/range returns tokens for a sub-range', async () => {
        const result = await client.request('textDocument/semanticTokens/range', {
            textDocument: { uri: mainUri },
            range: {
                start: { line: 0, character: 0 },
                end: { line: mainText.split('\n').length, character: 0 },
            },
        });
        assert.ok(result, 'server must not answer with null for a compiled file');
        assert.ok(result.data.length > 0, 'range request must produce tokens');
        assert.equal(result.data.length % 5, 0);
    });

    test('inlayHint returns type hints for inferred bindings', async () => {
        const hints = await client.request('textDocument/inlayHint', {
            textDocument: { uri: mainUri },
            range: {
                start: { line: 0, character: 0 },
                end: { line: mainText.split('\n').length, character: 0 },
            },
        });
        assert.ok(Array.isArray(hints), `expected an array of hints, got ${JSON.stringify(hints)}`);
        assert.ok(hints.length > 0, 'the `let` bindings in main.art must produce type hints');
        for (const hint of hints) {
            assert.ok(hint.label.startsWith(': '), `unexpected hint label: ${hint.label}`);
        }
        assert.ok(hints.some((h) => h.label.includes('Vec2')),
            `at least one hint must name Vec2, got ${JSON.stringify(hints.map((h) => h.label))}`);
    });

    test('definition jumps from a call in main.art to the declaration in geometry.art', async () => {
        // `dot(v, v)` in main.art is declared as `fn dot(...)` in geometry.art.
        const pos = locate(mainText, 'dot(v, v)');
        const result = await client.request('textDocument/definition', {
            textDocument: { uri: mainUri },
            position: pos,
        });
        const locations = Array.isArray(result) ? result : [result];
        assert.ok(locations.length > 0 && locations[0],
            'go-to-definition must resolve a cross-file call');

        const decl = locate(geometryText, 'fn dot');
        assert.equal(normalizeUri(locations[0].uri), normalizeUri(geometryUri));
        assert.equal(locations[0].range.start.line, decl.line);
    });

    test('references from a declaration finds the use in the other file', async () => {
        const decl = locate(geometryText, 'dot(a: Vec2');
        const result = await client.request('textDocument/references', {
            textDocument: { uri: geometryUri },
            position: decl,
            context: { includeDeclaration: true },
        });
        assert.ok(Array.isArray(result) && result.length > 0,
            `expected references, got ${JSON.stringify(result)}`);

        const uris = result.map((loc) => normalizeUri(loc.uri));
        assert.ok(uris.includes(normalizeUri(mainUri)),
            `references must include the call site in main.art, got ${JSON.stringify(uris)}`);
    });
});
