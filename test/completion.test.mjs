// Covers textDocument/completion. The two cases here are regressions: a projection on an
// enum used to read the fields of a null StructType, and a generic function lost its
// `detail` because the ForallType was never unwrapped.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture } from './helpers.mjs';

// Replaces the body of `main` with `text`, leaving everything above it untouched, and
// returns the zero-based position just past the last character of `text`.
const withBody = (source, text) => {
    const lines = source.split('\n');
    const start = lines.findIndex((l) => l.startsWith('fn main('));
    assert.notEqual(start, -1, 'fixture must declare fn main');
    const head = lines.slice(0, start + 1);
    const body = text.split('\n');
    return {
        text: [...head, ...body, '}', ''].join('\n'),
        position: { line: head.length + body.length - 1, character: body.at(-1).length },
    };
};

describe('completion', () => {
    let ws;
    let client;
    let uri;
    let source;

    const completeWith = async (body) => {
        const { text, position } = withBody(source, body);
        client.changeDocument(uri, text, Date.now());
        await client.settle(300);
        const result = await client.request('textDocument/completion', {
            textDocument: { uri },
            position,
        });
        return result?.items ?? result ?? [];
    };

    before(async () => {
        ws = stageFixture('completion');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);

        uri = ws.fileUri('src', 'values.art');
        source = ws.read('src', 'values.art');
        client.openDocument(uri, source);
        await client.waitForDiagnostics(uri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('offers the fields of a struct behind a projection', async () => {
        const items = await completeWith('    let p = Point { x = 1, y = 2 };\n    p.');
        assert.deepEqual(items.map((i) => i.label).sort(), ['x', 'y']);
    });

    test('offers the options of an enum behind a projection, without crashing', async () => {
        const items = await completeWith('    let s = Shape::Empty;\n    s.');
        assert.deepEqual(items.map((i) => i.label).sort(), ['Dot', 'Empty']);
    });

    test('the server is still alive after the enum projection', async () => {
        // A null-dereference in the enum branch used to take the whole process down, so
        // every later request in the session failed rather than this one.
        const items = await completeWith('    let s = Shape::Empty;\n    s.');
        assert.ok(items.length > 0, 'server answered a second request');
    });

    test('reports the inferred return type of a generic function', async () => {
        const items = await completeWith('    ');
        const identity = items.find((i) => i.label.startsWith('identity'));
        assert.ok(identity, 'generic function is offered');
        assert.equal(identity.detail, 'T');
    });
});
