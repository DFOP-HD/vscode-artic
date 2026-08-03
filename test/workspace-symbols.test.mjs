// Workspace symbols (Ctrl+T) and the reference-count code lens.
//
// The index behind `workspace/symbol` parses rather than compiles, and it spans every
// project the configuration declares, not just the one the active file belongs to.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFixture, locate } from './helpers.mjs';

describe('workspace symbols and code lens', () => {
    let client;
    let ws;
    let geometryUri;
    let geometry;
    let initResult;

    const symbols = (query) => client.request('workspace/symbol', { query });

    before(async () => {
        client = new LspClient(findServerBinary());
        initResult = await client.initialize(null);

        ws = stageFixture('workspace-symbols');
        geometryUri = ws.fileUri('core/geometry.art');
        geometry = ws.read('core/geometry.art');
        // Opening a source file is what makes the server find the config, and with it
        // every project the config declares.
        client.openDocument(geometryUri, geometry);
        await client.settle(300);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('advertises both capabilities', () => {
        assert.equal(initResult.capabilities.workspaceSymbolProvider, true);
        assert.ok(initResult.capabilities.codeLensProvider);
    });

    test('finds a declaration by a substring of its name', async () => {
        const result = await symbols('anhat');
        const names = result.map((s) => s.name).sort();
        assert.deepEqual(names, ['double_manhattan', 'manhattan']);
        const manhattan = result.find((s) => s.name === 'manhattan');
        assert.equal(manhattan.kind, 12); // Function
        assert.equal(normalizeUri(manhattan.location.uri), normalizeUri(geometryUri));
    });

    test('spans every project in the config, not just the active one', async () => {
        // `total` lives in `ws-app`; nothing in the session ever opened or compiled it.
        const result = await symbols('total');
        assert.equal(result.length, 1);
        assert.match(result[0].location.uri, /app\/main\.art$/);
    });

    test('reports the enclosing declaration as the container', async () => {
        const result = await symbols('helper');
        assert.equal(result.length, 1);
        assert.equal(result[0].containerName, 'inner');
    });

    test('matches everything on an empty query', async () => {
        const names = (await symbols('')).map((s) => s.name);
        for (const expected of ['Point', 'x', 'Shape', 'Dot', 'Length', 'origin', 'inner', 'total']) {
            assert.ok(names.includes(expected), `expected ${expected} in ${names.join(', ')}`);
        }
    });

    test('counts the references above a declaration', async () => {
        const lenses = await client.request('textDocument/codeLens', {
            textDocument: { uri: geometryUri },
        });
        const titleAt = (needle) => {
            const { line, character } = locate(geometry, needle);
            const lens = lenses.find(
                (l) => l.range.start.line === line && l.range.start.character === character);
            return lens?.command?.title;
        };
        // `double_manhattan` calls it twice; `app/main.art` is in another project and so
        // is not part of this file's compile.
        assert.equal(titleAt('manhattan(p: Point)'), '2 references');
        assert.equal(titleAt('double_manhattan'), '0 references');
        assert.match(titleAt('Point {'), /^\d+ references$/);
    });

    test('puts no lens on a field or an enum option', async () => {
        const lenses = await client.request('textDocument/codeLens', {
            textDocument: { uri: geometryUri },
        });
        for (const needle of ['x: i32', 'Dot(Point)']) {
            const { line, character } = locate(geometry, needle);
            assert.equal(
                lenses.find((l) => l.range.start.line === line && l.range.start.character === character),
                undefined,
                `${needle} should carry no code lens`);
        }
    });
});
