// Covers textDocument/documentSymbol: the outline tree, the kinds it reports, and the
// two ranges every symbol carries.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture } from './helpers.mjs';

// The subset of lsp.SymbolKind this server can emit, by protocol number.
const Kind = {
    Module: 2,
    Field: 8,
    Enum: 10,
    Function: 12,
    Variable: 13,
    Constant: 14,
    EnumMember: 22,
    Struct: 23,
    TypeParameter: 26,
};

describe('document symbols', () => {
    let ws;
    let client;
    let uri;
    let symbols;
    let initResult;

    const byName = (name, within = symbols) => within.find((s) => s.name === name);

    before(async () => {
        ws = stageFixture('document-symbols');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        initResult = await client.initialize(ws.uri);

        uri = ws.fileUri('src', 'outline.art');
        client.openDocument(uri, ws.read('src', 'outline.art'));
        await client.waitForDiagnostics(uri);

        symbols = await client.request('textDocument/documentSymbol', { textDocument: { uri } });
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('the server advertises documentSymbolProvider', () => {
        assert.equal(initResult.capabilities.documentSymbolProvider, true);
    });

    test('reports the top-level declarations in source order', () => {
        assert.deepEqual(
            symbols.map((s) => s.name),
            [
                'Distance', 'Point', 'Pair', 'Shape', 'ORIGIN', 'counter',
                'implicit', 'offset', 'metrics', 'manhattan', 'area', 'main',
            ],
        );
    });

    test('maps every declaration kind onto the matching symbol kind', () => {
        assert.equal(byName('Distance').kind, Kind.TypeParameter);
        assert.equal(byName('Point').kind, Kind.Struct);
        assert.equal(byName('Shape').kind, Kind.Enum);
        assert.equal(byName('ORIGIN').kind, Kind.Constant);
        assert.equal(byName('counter').kind, Kind.Variable);
        assert.equal(byName('implicit').kind, Kind.Constant);
        assert.equal(byName('metrics').kind, Kind.Module);
        assert.equal(byName('main').kind, Kind.Function);
    });

    test('nests struct fields under their struct', () => {
        const point = byName('Point');
        assert.deepEqual(point.children.map((c) => c.name), ['x', 'y']);
        assert.equal(point.children[0].kind, Kind.Field);
        assert.equal(point.children[0].detail, 'x: i32');
    });

    test('omits the numbered fields of a tuple-like struct', () => {
        assert.equal(byName('Pair').children, undefined);
    });

    test('nests enum options, and their fields, under the enumeration', () => {
        const shape = byName('Shape');
        assert.deepEqual(shape.children.map((c) => c.name), ['Empty', 'Dot', 'Box']);
        assert.equal(shape.children[0].kind, Kind.EnumMember);
        assert.equal(shape.children[1].detail, 'Shape::Dot(Point)');
        assert.deepEqual(shape.children[2].children.map((c) => c.name), ['width', 'height']);
    });

    test('nests the members of a module', () => {
        const metrics = byName('metrics');
        assert.deepEqual(metrics.children.map((c) => c.name), ['Sample', 'double']);
        assert.deepEqual(byName('Sample', metrics.children).children.map((c) => c.name), ['value']);
    });

    test('carries the declaration signature as the detail', () => {
        assert.equal(byName('manhattan').detail, 'fn manhattan(p: Point) -> Distance');
        assert.equal(byName('ORIGIN').detail, 'static ORIGIN: Point');
        assert.equal(byName('Distance').detail, 'type Distance = i32');
        assert.equal(byName('implicit').detail, 'Point');
    });

    test('selects the identifier and encloses the whole declaration', () => {
        const main = byName('main');
        // `fn main() -> i32 {` starts at column 1; the identifier is four characters in.
        assert.equal(main.selectionRange.start.character, 3);
        assert.equal(main.selectionRange.end.character, 7);
        assert.equal(main.selectionRange.start.line, main.range.start.line);
        assert.ok(main.range.end.line > main.range.start.line, 'the range must span the body');
    });

    test('reports nothing for a file that is not artic source', async () => {
        const result = await client.request('textDocument/documentSymbol', {
            textDocument: { uri: ws.fileUri('artic.json') },
        });
        assert.equal(result, null);
    });
});
