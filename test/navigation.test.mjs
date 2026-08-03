// Covers the cursor-driven navigation requests that resolve through the NameMap and the
// AST: textDocument/definition (on a declaration as well as a reference),
// textDocument/documentHighlight, textDocument/typeDefinition and
// textDocument/selectionRange.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture, locate, samePath, uriToPath } from './helpers.mjs';

// LSP DocumentHighlightKind
const READ = 2;
const WRITE = 3;

describe('navigation', () => {
    let ws;
    let client;
    let initResult;

    const shapes = {};
    const uses = {};
    const implicits = {};

    const request = (method, doc, needle, occurrence = 1) => client.request(method, {
        textDocument: { uri: doc.uri },
        position: locate(doc.text, needle, occurrence),
    });

    /** Normalises a Location / Location[] / LocationLink[] response to a plain array. */
    const asLocations = (result) => {
        if (result === null || result === undefined) return [];
        return Array.isArray(result) ? result : [result];
    };

    before(async () => {
        ws = stageFixture('navigation');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        initResult = await client.initialize(ws.uri);

        for (const [handle, name] of [[shapes, 'shapes.art'], [uses, 'uses.art'], [implicits, 'implicits.art']]) {
            handle.uri = ws.fileUri('src', name);
            handle.path = ws.path('src', name);
            handle.text = ws.read('src', name);
            client.openDocument(handle.uri, handle.text);
            await client.waitForDiagnostics(handle.uri);
        }
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('the server advertises the new navigation capabilities', () => {
        assert.equal(initResult.capabilities.documentHighlightProvider, true);
        assert.equal(initResult.capabilities.typeDefinitionProvider, true);
        assert.equal(initResult.capabilities.implementationProvider, true);
        assert.equal(initResult.capabilities.selectionRangeProvider, true);
    });

    // ---------------------------------------------------------------- definition

    test('definition on a reference jumps to the declaration', async () => {
        const locations = asLocations(await request('textDocument/definition', uses, 'scale(ORIGIN'));
        assert.equal(locations.length, 1);
        assert.ok(samePath(uriToPath(locations[0].uri), shapes.path), 'must land in shapes.art');
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'scale(v: Vec2'));
    });

    test('definition on a declaration returns the declaration itself, not its references', async () => {
        // The regression this guards: the handler used to answer with find_refs(decl), so
        // Go-to-Definition on `fn scale` jumped into uses.art instead of staying put.
        const locations = asLocations(await request('textDocument/definition', shapes, 'scale(v: Vec2'));
        assert.equal(locations.length, 1, 'a declaration has exactly one definition');
        assert.ok(samePath(uriToPath(locations[0].uri), shapes.path), 'must not jump to a reference in uses.art');
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'scale(v: Vec2'));
    });

    test('definition on a type name in a signature resolves the struct', async () => {
        const locations = asLocations(await request('textDocument/definition', shapes, 'Vec2, k: i32'));
        assert.equal(locations.length, 1);
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'Vec2 {'));
    });

    test('definition answers null where nothing is named', async () => {
        assert.equal(await request('textDocument/definition', shapes, 'struct'), null);
    });

    // --------------------------------------------------------- document highlight

    test('highlights a declaration and every reference in the same file', async () => {
        const highlights = await request('textDocument/documentHighlight', shapes, 'v: Vec2, k');
        assert.ok(Array.isArray(highlights));
        // `v` is declared once and read twice in `scale`.
        assert.equal(highlights.length, 3);
        assert.equal(highlights.filter((h) => h.kind === WRITE).length, 1);
        assert.equal(highlights.filter((h) => h.kind === READ).length, 2);
        assert.deepEqual(
            highlights.find((h) => h.kind === WRITE).range.start,
            locate(shapes.text, 'v: Vec2, k'));
    });

    test('highlights never leak in from another file', async () => {
        // `scale` is declared in shapes.art and only ever called from uses.art.
        const atDecl = await request('textDocument/documentHighlight', shapes, 'scale(v: Vec2');
        assert.equal(atDecl.length, 1, 'the two call sites live in uses.art and must be dropped');
        assert.equal(atDecl[0].kind, WRITE);

        // And the other way round: from uses.art the declaration must be dropped.
        const atRef = await request('textDocument/documentHighlight', uses, 'scale(ORIGIN');
        assert.equal(atRef.length, 2, 'both call sites, but not the declaration');
        assert.ok(atRef.every((h) => h.kind === READ));
    });

    test('highlight answers null where nothing is named', async () => {
        assert.equal(await request('textDocument/documentHighlight', shapes, 'struct'), null);
    });

    // ----------------------------------------------------------- type definition

    test('type definition of a parameter resolves its struct declaration', async () => {
        const locations = asLocations(await request('textDocument/typeDefinition', shapes, 'v: Vec2, k'));
        assert.equal(locations.length, 1);
        assert.ok(samePath(uriToPath(locations[0].uri), shapes.path));
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'Vec2 {'));
    });

    test('type definition follows a let binding across files', async () => {
        // `a` has no type annotation at all - the type comes from inference.
        const locations = asLocations(await request('textDocument/typeDefinition', uses, 'a = scale'));
        assert.equal(locations.length, 1);
        assert.ok(samePath(uriToPath(locations[0].uri), shapes.path), 'the type is declared in shapes.art');
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'Vec2 {'));
    });

    test('type definition of an enum-typed binding resolves the enum', async () => {
        const locations = asLocations(await request('textDocument/typeDefinition', uses, 'c = Cell'));
        assert.equal(locations.length, 1);
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'Cell {'));
    });

    test('type definition of a static resolves through its annotation', async () => {
        const locations = asLocations(await request('textDocument/typeDefinition', shapes, 'ORIGIN: Vec2'));
        assert.equal(locations.length, 1);
        assert.deepEqual(locations[0].range.start, locate(shapes.text, 'Vec2 {'));
    });

    test('type definition answers null for a primitive type', async () => {
        // `dx` is an i32: there is no declaration to go to.
        assert.equal(await request('textDocument/typeDefinition', shapes, 'dx = v.x'), null);
    });

    test('type definition answers null where nothing is named', async () => {
        assert.equal(await request('textDocument/typeDefinition', shapes, 'struct'), null);
    });

    // ------------------------------------------------------------- implementation

    test('implementation of an omitted implicit argument resolves the instance', async () => {
        // Nothing in `apply(v)` names the implicit; the Summoner chose it, and its choice is
        // the only thing that can answer this request. The synthesised SummonExpr carries the
        // argument list's location, so the cursor has to be inside the parentheses.
        const locations = asLocations(await request('textDocument/implementation', implicits, 'v)'));
        assert.equal(locations.length, 1);
        assert.ok(samePath(uriToPath(locations[0].uri), implicits.path));
        assert.deepEqual(locations[0].range.start, locate(implicits.text, 'Factor { value = 7 }'));
    });

    test('implementation of an explicit summon resolves the same instance', async () => {
        const locations = asLocations(await request('textDocument/implementation', implicits, 'summon[Factor]'));
        assert.equal(locations.length, 1);
        assert.deepEqual(locations[0].range.start, locate(implicits.text, 'Factor { value = 7 }'));
    });

    test('implementation answers null where no implicit is summoned', async () => {
        assert.equal(await request('textDocument/implementation', shapes, 'scale(v: Vec2'), null);
    });

    // ------------------------------------------------------------ selection range

    /** Flattens the parent chain into innermost-first ranges. */
    const chainOf = (node) => {
        const ranges = [];
        for (let n = node; n; n = n.parent) ranges.push(n.range);
        return ranges;
    };

    const before_ = (a, b) => a.line < b.line || (a.line === b.line && a.character <= b.character);
    const encloses = (outer, inner) =>
        before_(outer.start, inner.start) && before_(inner.end, outer.end);

    test('returns a strictly widening chain of ranges', async () => {
        const position = locate(shapes.text, 'x = v.x * k');
        const result = await client.request('textDocument/selectionRange', {
            textDocument: { uri: shapes.uri },
            positions: [position],
        });
        assert.ok(Array.isArray(result));
        assert.equal(result.length, 1, 'one result per requested position');

        const ranges = chainOf(result[0]);
        assert.ok(ranges.length > 2, `expected a nested chain, got ${ranges.length} range(s)`);

        for (let i = 1; i < ranges.length; i++) {
            assert.ok(encloses(ranges[i], ranges[i - 1]),
                `range ${i} must enclose range ${i - 1}: ${JSON.stringify(ranges[i])} vs ${JSON.stringify(ranges[i - 1])}`);
            assert.notDeepEqual(ranges[i], ranges[i - 1], 'consecutive ranges must differ');
        }

        // The innermost range must contain the cursor, the outermost is the whole function.
        assert.ok(encloses(ranges[0], { start: position, end: position }) ||
            before_(ranges[0].start, position), 'innermost range must start at or before the cursor');
        assert.deepEqual(ranges[ranges.length - 1].start, locate(shapes.text, 'fn scale'));
    });

    test('answers every requested position, including ones outside any node', async () => {
        const inside = locate(shapes.text, 'dx + dy');
        const outside = { line: 0, character: 0 }; // a comment line - no AST node covers it
        const result = await client.request('textDocument/selectionRange', {
            textDocument: { uri: shapes.uri },
            positions: [inside, outside],
        });
        assert.equal(result.length, 2);
        assert.ok(chainOf(result[0]).length > 1);
        // Positions the AST does not cover still need a slot, or the client cannot line the
        // answers up with its requests.
        assert.deepEqual(result[1].range, { start: outside, end: outside });
        assert.equal(result[1].parent, undefined);
    });
});
