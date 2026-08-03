// Inlay hints on the configuration document itself: how many files each project ends up
// with, and what each `files` pattern contributed. This used to be an Information-severity
// diagnostic, which put a perfectly healthy configuration in the Problems panel.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles } from './helpers.mjs';

const config = {
    'artic-config': '2.0',
    projects: [
        { name: 'hints-core', files: ['core/*.art'] },
        {
            name: 'hints-app',
            files: ['app/*.art', '!app/skip.art'],
            dependencies: ['hints-core'],
        },
    ],
};

describe('config inlay hints', () => {
    let client;
    let ws;
    let uri;
    let text;

    const hints = () => client.request('textDocument/inlayHint', {
        textDocument: { uri },
        range: {
            start: { line: 0, character: 0 },
            end: { line: text.split('\n').length, character: 0 },
        },
    });

    // The hint for a literal is anchored just past its closing quote.
    const labelFor = (result, literal) => {
        const lines = text.split('\n');
        const quoted = JSON.stringify(literal);
        const line = lines.findIndex((l) => l.includes(quoted));
        assert.notEqual(line, -1, `fixture must contain ${quoted}`);
        const character = lines[line].indexOf(quoted) + quoted.length;
        const hint = result.find((h) => h.position.line === line && h.position.character === character);
        return hint?.label;
    };

    before(async () => {
        client = new LspClient(findServerBinary());
        await client.initialize(null);

        ws = stageFiles({
            'artic.json': JSON.stringify(config, null, 2),
            'core/a.art': 'fn core_a() -> i32 { 1 }\n',
            'core/b.art': 'fn core_b() -> i32 { 2 }\n',
            'app/main.art': 'fn app_main() -> i32 { core_a() + core_b() }\n',
            'app/skip.art': 'fn app_skip() -> i32 { 3 }\n',
        });
        uri = ws.fileUri('artic.json');
        text = ws.read('artic.json');
        client.openDocument(uri, text, 'json');
        await client.settle(300);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('annotates a project with the number of files it compiles', async () => {
        const result = await hints();
        assert.equal(labelFor(result, 'hints-core'), '2 files');
    });

    test('separates a project\'s own files from the ones it inherits', async () => {
        const result = await hints();
        assert.equal(labelFor(result, 'hints-app'), '1 file, 3 with dependencies');
    });

    test('annotates each include pattern with what it matched', async () => {
        const result = await hints();
        assert.equal(labelFor(result, 'core/*.art'), '2 files');
        assert.equal(labelFor(result, 'app/*.art'), '2 files');
    });

    test('annotates an exclude pattern with what it removed', async () => {
        const result = await hints();
        assert.equal(labelFor(result, '!app/skip.art'), '1 file excluded');
    });

    test('a healthy config produces no diagnostics at all', async () => {
        // The whole point of moving this to inlay hints: nothing here is a problem.
        assert.deepEqual(client.diagnosticsFor(uri), []);
    });

    test('honours the requested range', async () => {
        const result = await client.request('textDocument/inlayHint', {
            textDocument: { uri },
            range: { start: { line: 0, character: 0 }, end: { line: 1, character: 0 } },
        });
        assert.deepEqual(result, []);
    });
});
