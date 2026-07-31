// Covers textDocument/hover: the rendering of every declaration kind, the range the
// server reports, and the cases where hover must answer with null.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture, locate } from './helpers.mjs';

describe('hover', () => {
    let ws;
    let client;
    let uri;
    let text;
    let initResult;

    const hoverAt = async (needle, occurrence = 1) => {
        const result = await client.request('textDocument/hover', {
            textDocument: { uri },
            position: locate(text, needle, occurrence),
        });
        return result;
    };

    const signatureAt = async (needle, occurrence = 1) => {
        const result = await hoverAt(needle, occurrence);
        assert.ok(result, `expected a hover for ${JSON.stringify(needle)}, got null`);
        assert.equal(result.contents.kind, 'markdown');
        const match = /^```artic\n([\s\S]*)\n```$/.exec(result.contents.value);
        assert.ok(match, `hover must be a fenced artic block, got ${JSON.stringify(result.contents.value)}`);
        return match[1];
    };

    before(async () => {
        ws = stageFixture('hover');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        initResult = await client.initialize(ws.uri);

        uri = ws.fileUri('src', 'shapes.art');
        text = ws.read('src', 'shapes.art');
        client.openDocument(uri, text);
        await client.waitForDiagnostics(uri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('the server advertises hoverProvider', async () => {
        assert.equal(initResult.capabilities.hoverProvider, true);
    });

    test('renders a function signature with parameter names and return type', async () => {
        // At the call site, not the declaration - hover has to follow the reference.
        assert.equal(await signatureAt('manhattan(p)'), 'fn manhattan(p: Point) -> Distance');
    });

    test('renders type parameters of a generic function', async () => {
        assert.equal(await signatureAt('pick(true'), 'fn pick[T](cond: bool, a: T, b: T) -> T');
    });

    test('renders struct, enum and type alias declarations', async () => {
        // First occurrence of each name is its declaration.
        assert.equal(await signatureAt('Point'), 'struct Point');
        assert.equal(await signatureAt('Shape'), 'enum Shape');
        assert.equal(await signatureAt('Distance'), 'type Distance = i32');
    });

    test('renders an enum option qualified by its enumeration', async () => {
        assert.equal(await signatureAt('Dot(corner)'), 'Shape::Dot(Point)');
    });

    test('renders a static with its declared type', async () => {
        assert.equal(await signatureAt('ORIGIN.x'), 'static ORIGIN: Point');
    });

    test('renders a struct field with its type', async () => {
        assert.equal(await signatureAt('x: i32'), 'x: i32');
    });

    test('renders a local binding with its inferred type', async () => {
        assert.equal(await signatureAt('corner = Point'), 'corner: Point');
    });

    test('reports the range of the identifier under the cursor, not of the declaration', async () => {
        const callSite = locate(text, 'manhattan(p)');
        const result = await hoverAt('manhattan(p)');
        assert.equal(result.range.start.line, callSite.line);
        assert.equal(result.range.start.character, callSite.character);
        assert.equal(result.range.end.character, callSite.character + 'manhattan'.length);
    });

    test('returns null where there is no symbol', async () => {
        const result = await hoverAt('Fixture: one');
        assert.equal(result, null, `expected null inside a comment, got ${JSON.stringify(result)}`);
    });
});
