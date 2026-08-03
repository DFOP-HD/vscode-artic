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

// Inserts `line` immediately above `fn main(`, and returns the position at its end.
const withLineAboveMain = (source, line) => {
    const lines = source.split('\n');
    const at = lines.findIndex((l) => l.startsWith('fn main('));
    assert.notEqual(at, -1, 'fixture must declare fn main');
    return {
        text: [...lines.slice(0, at), line, ...lines.slice(at)].join('\n'),
        position: { line: at, character: line.length },
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

    const completeAt = async ({ text, position }) => {
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

    test('does not offer a loop variable after the loop has ended', async () => {
        // `for i in range(...)` desugars into a call taking a closure, so the loop variable
        // lives in an FnExpr rather than in the block it visually belongs to. Walking the
        // enclosing block used to reach it and offer `i` to the whole function body.
        const items = await completeWith('    for i in range(0, 4) { let inner = i; }\n    ');
        const labels = items.map((i) => i.label);
        assert.ok(!labels.includes('i'), 'loop variable is out of scope here');
        assert.ok(!labels.includes('inner'), 'the loop body binding is out of scope too');
    });

    test('offers a loop variable inside the loop', async () => {
        const items = await completeWith('    for i in range(0, 4) { ');
        assert.ok(items.map((i) => i.label).includes('i'), 'loop variable is in scope here');
    });

    test('does not offer a match arm binding outside the arm', async () => {
        const items = await completeWith(
            '    let s = Shape::Empty;\n'
            + '    match s { Shape::Empty => 0, Shape::Dot(bound) => 1 };\n'
            + '    ');
        assert.ok(!items.map((i) => i.label).includes('bound'), 'arm binding is out of scope');
    });

    test('offers the members of a module behind a use path', async () => {
        const items = await completeAt(withLineAboveMain(source, 'use shapes::'));
        const labels = items.map((i) => i.label.replace(/[($].*$/, ''));
        assert.ok(labels.includes('Circle'), 'the module struct is offered');
        assert.ok(labels.includes('area'), 'the module function is offered');
    });
});

// The list is only useful if the editor asks for it and then shows it in the order the
// server chose. Both were broken independently: the widget opened on `.` and `:` only, so
// `let a = ` offered nothing at all, and no item carried a `sortText`, so the client fell
// back to sorting by label and buried every declaration under 30 keywords.
describe('completion is reachable and ranked', () => {
    let ws;
    let client;
    let uri;
    let source;
    let capabilities;

    before(async () => {
        ws = stageFixture('completion');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        const result = await client.initialize(ws.uri);
        capabilities = result.capabilities;

        uri = ws.fileUri('src', 'values.art');
        source = ws.read('src', 'values.art');
        client.openDocument(uri, source);
        await client.waitForDiagnostics(uri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('opens where an expression can begin, not just after a projection', async () => {
        const triggers = capabilities.completionProvider?.triggerCharacters ?? [];
        for (const c of ['.', ':', '=', ' ', '(', ',', '[']) {
            assert.ok(triggers.includes(c), `${JSON.stringify(c)} opens the widget`);
        }
    });

    test('marks the list incomplete so the client re-asks as the cursor moves', async () => {
        const { text, position } = withBody(source, '    ');
        client.changeDocument(uri, text, Date.now());
        await client.settle(300);
        const result = await client.request('textDocument/completion', {
            textDocument: { uri },
            position,
        });
        assert.equal(result.isIncomplete, true);
    });

    test('ranks declarations above keywords on an unfinished let', async () => {
        // The user-visible symptom: `let a = ` with a function above it in the file.
        const { text, position } = withBody(source, '    let a = ');
        client.changeDocument(uri, text, Date.now());
        await client.settle(300);
        const result = await client.request('textDocument/completion', {
            textDocument: { uri },
            position,
        });
        const items = result.items ?? result;

        for (const item of items) {
            assert.ok(item.sortText, `${item.label} carries a sortText`);
        }

        const shown = [...items].sort((a, b) => a.sortText.localeCompare(b.sortText));
        const rank = (predicate) => shown.findIndex(predicate);

        const identity = rank((i) => i.label.startsWith('identity'));
        assert.notEqual(identity, -1, 'the function above is offered');

        const keyword = rank((i) => i.kind === 14 /* Keyword */);
        assert.notEqual(keyword, -1, 'keywords are offered too');
        assert.ok(identity < keyword,
            `declaration ranks above the first keyword (${identity} < ${keyword})`);
    });

    test('ranks a local binding above a module-level declaration', async () => {
        const { text, position } = withBody(source, '    let local_value = 1;\n    ');
        client.changeDocument(uri, text, Date.now());
        await client.settle(300);
        const result = await client.request('textDocument/completion', {
            textDocument: { uri },
            position,
        });
        const shown = [...(result.items ?? result)]
            .sort((a, b) => a.sortText.localeCompare(b.sortText));
        const local = shown.findIndex((i) => i.label === 'local_value');
        const global = shown.findIndex((i) => i.label.startsWith('identity'));
        assert.notEqual(local, -1, 'the local binding is offered');
        assert.notEqual(global, -1, 'the module function is offered');
        assert.ok(local < global, `nearest binding first (${local} < ${global})`);
    });
});
