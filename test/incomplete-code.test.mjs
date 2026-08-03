// Language features while the buffer is mid-edit, i.e. while it does not compile.
//
// This is the state an editor spends most of its time in, and it is the state in which
// completion and go-to-definition matter most. The compiler is error-tolerant on purpose:
// `Compiler::compile_files` keeps the partially parsed AST and still runs the name binder
// and the type checker over it, so `name_map` is populated for everything that did parse.
// These tests exist to keep that true.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFixture, locate } from './helpers.mjs';

// A function the user is halfway through typing: no closing paren, no closing brace.
// Appended rather than inserted, so everything above it is untouched valid source.
const HALF_WRITTEN = `
fn half_written() -> i32 {
    let z = scale(
`;

describe('features while the code does not compile', () => {
    let ws;
    let client;
    let usesUri;
    let shapesUri;
    let uses;
    let broken;
    let version = 1;

    // Puts the document into a state and waits for the resulting diagnostics, so the
    // request that follows is answered from the compile of exactly that text.
    const setText = async (uri, text) => {
        client.clearDiagnosticsLog();
        client.changeDocument(uri, text, ++version);
        await client.waitForDiagnostics(uri);
    };

    const definitionAt = (uri, text, needle, occurrence = 1) =>
        client.request('textDocument/definition', {
            textDocument: { uri },
            position: locate(text, needle, occurrence),
        });

    before(async () => {
        ws = stageFixture('navigation');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);

        usesUri = ws.fileUri('src', 'uses.art');
        shapesUri = ws.fileUri('src', 'shapes.art');
        uses = ws.read('src', 'uses.art');
        broken = uses + HALF_WRITTEN;

        client.openDocument(usesUri, uses);
        await client.waitForDiagnostics(usesUri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('the fixture really is broken', async () => {
        // Guard: without this the whole suite could pass against valid source.
        await setText(usesUri, broken);
        const errors = client.diagnosticsFor(usesUri).filter((d) => d.severity === 1);
        assert.ok(errors.length > 0, 'the half-written buffer should produce an error');
    });

    test('go-to-definition still works above the broken line', async () => {
        await setText(usesUri, broken);
        const result = await definitionAt(usesUri, broken, 'scale(ORIGIN');
        const location = Array.isArray(result) ? result[0] : result;
        assert.ok(location, 'expected a definition');
        assert.equal(normalizeUri(location.uri), normalizeUri(shapesUri));
    });

    test('go-to-definition works on the half-written call itself', async () => {
        // The most valuable case: the identifier under the cursor is part of the
        // expression that failed to parse.
        await setText(usesUri, broken);
        const result = await definitionAt(usesUri, broken, 'scale(\n');
        const location = Array.isArray(result) ? result[0] : result;
        assert.ok(location, 'expected a definition for the callee of an unclosed call');
        assert.equal(normalizeUri(location.uri), normalizeUri(shapesUri));
    });

    test('completion offers struct fields behind a trailing dot', async () => {
        const text = uses + '\nfn typing() -> i32 {\n    ORIGIN.\n';
        await setText(usesUri, text);
        const start = locate(text, 'ORIGIN.');
        const result = await client.request('textDocument/completion', {
            textDocument: { uri: usesUri },
            position: { line: start.line, character: start.character + 'ORIGIN.'.length },
        });
        const labels = (result?.items ?? result ?? []).map((i) => i.label);
        assert.deepEqual(labels.sort(), ['x', 'y']);
    });

    test('completion still offers top-level declarations', async () => {
        const text = uses + '\nfn typing() -> i32 {\n    sca\n';
        await setText(usesUri, text);
        const start = locate(text, '    sca');
        const result = await client.request('textDocument/completion', {
            textDocument: { uri: usesUri },
            position: { line: start.line, character: start.character + '    sca'.length },
        });
        const labels = (result?.items ?? result ?? []).map((i) => i.label);
        // The label carries the whole signature; the client does the prefix filtering.
        assert.ok(labels.some((l) => l.startsWith('scale(')),
            `expected a 'scale' item in ${labels.join(', ')}`);
    });

    test('hover still renders a declaration', async () => {
        await setText(usesUri, broken);
        const result = await client.request('textDocument/hover', {
            textDocument: { uri: usesUri },
            position: locate(broken, 'norm(a)'),
        });
        assert.match(result?.contents?.value ?? '', /fn norm/);
    });

    test('the outline keeps the declarations that did parse', async () => {
        // An outline that empties itself on every keystroke is worse than no outline.
        await setText(usesUri, broken);
        const symbols = await client.request('textDocument/documentSymbol', {
            textDocument: { uri: usesUri },
        });
        assert.ok(symbols.some((s) => s.name === 'total'),
            `expected 'total' in ${symbols.map((s) => s.name).join(', ')}`);
    });

    test('semantic tokens are still produced', async () => {
        // Syntax highlighting must not blink off while a line is being typed.
        await setText(usesUri, broken);
        const result = await client.request('textDocument/semanticTokens/full', {
            textDocument: { uri: usesUri },
        });
        assert.ok(result?.data?.length > 0, 'expected semantic tokens for a broken buffer');
    });

    test('a second file stays navigable while this one is broken', async () => {
        // Both files are in one project and are compiled together, so a parse failure in
        // one must not take the other down with it.
        await setText(usesUri, broken);
        const shapes = ws.read('src', 'shapes.art');
        client.openDocument(shapesUri, shapes);
        await client.waitForDiagnostics(shapesUri);

        const result = await definitionAt(shapesUri, shapes, 'Vec2, k: i32');
        const location = Array.isArray(result) ? result[0] : result;
        assert.ok(location, 'expected a definition in the file that still parses');
        assert.equal(normalizeUri(location.uri), normalizeUri(shapesUri));
    });

    test('recovers once the code is valid again', async () => {
        await setText(usesUri, broken);
        await setText(usesUri, uses);
        const errors = client.diagnosticsFor(usesUri).filter((d) => d.severity === 1);
        assert.deepEqual(errors, []);
        const result = await definitionAt(usesUri, uses, 'scale(ORIGIN');
        const location = Array.isArray(result) ? result[0] : result;
        assert.equal(normalizeUri(location.uri), normalizeUri(shapesUri));
    });
});
