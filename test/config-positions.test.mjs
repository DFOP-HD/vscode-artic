// Where a configuration diagnostic or hint is placed. nlohmann/json discards source
// positions, so both used to be located by searching the document text for the value they
// were about -- which reports every textually identical string, not the one that is wrong.
// The document is indexed by JSON pointer now; these are the cases where that shows.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles } from './helpers.mjs';

/** Index of the line containing `needle`, asserted to exist. */
function lineOf(text, needle) {
    const line = text.split('\n').findIndex((l) => l.includes(needle));
    assert.notEqual(line, -1, `fixture must contain ${needle}`);
    return line;
}

describe('a value that appears more than once in the config', () => {
    let client, ws, uri, text;

    // The project is named after the folder it lives in, which is the normal thing to do.
    const config = {
        'artic-config': '2.0',
        projects: [
            { name: 'positions', folder: 'positions', files: ['*.art'] },
        ],
    };

    before(async () => {
        client = new LspClient(findServerBinary());
        await client.initialize(null);
        ws = stageFiles({
            'artic.json': JSON.stringify(config, null, 2),
            'main.art': 'fn positions_main() -> i32 { 0 }\n',
        });
        uri = ws.fileUri('artic.json');
        text = ws.read('artic.json');
        client.openDocument(uri, text, 'json');
        await client.settle(300);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('is reported once, not once per occurrence', () => {
        const missing = client.diagnosticsFor(uri).filter((d) => /folder does not exist/i.test(d.message));
        assert.equal(missing.length, 1,
            `the name and the folder are spelled the same; only the folder is wrong: ${JSON.stringify(missing)}`);
    });

    test('is reported on the member that is wrong', () => {
        const [diag] = client.diagnosticsFor(uri).filter((d) => /folder does not exist/i.test(d.message));
        assert.equal(diag.range.start.line, lineOf(text, '"folder"'));
        assert.ok(diag.range.end.line === diag.range.start.line
            && diag.range.end.character > diag.range.start.character,
            `expected a span on one line, got ${JSON.stringify(diag.range)}`);
    });
});

describe('a project named after one it is not', () => {
    let client, ws, uri, text;

    // `pos-lib` occurs twice: as a dependency of the first project, and as the name of the
    // second. Searching the text finds the dependency first, so the second project's file
    // count used to be reported on the wrong line.
    const config = {
        'artic-config': '2.0',
        projects: [
            { name: 'pos-app', dependencies: ['pos-lib'], files: ['app/*.art'] },
            { name: 'pos-lib', files: ['lib/*.art'] },
        ],
    };

    before(async () => {
        client = new LspClient(findServerBinary());
        await client.initialize(null);
        ws = stageFiles({
            'artic.json': JSON.stringify(config, null, 2),
            'app/main.art': 'fn pos_app_main() -> i32 { pos_lib() }\n',
            'lib/lib.art': 'fn pos_lib() -> i32 { 1 }\n',
        });
        uri = ws.fileUri('artic.json');
        text = ws.read('artic.json');
        client.openDocument(uri, text, 'json');
        await client.settle(300);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('is annotated on its own name, not on the reference to it', async () => {
        const hints = await client.request('textDocument/inlayHint', {
            textDocument: { uri },
            range: { start: { line: 0, character: 0 }, end: { line: text.split('\n').length, character: 0 } },
        });

        const dependencyLine = lineOf(text, '"pos-lib"');
        const nameLine = lineOf(text, '"name": "pos-lib"');
        assert.notEqual(dependencyLine, nameLine, 'fixture must put the two on different lines');

        assert.deepEqual(hints.filter((h) => h.position.line === dependencyLine), [],
            'a dependency reference is not a project declaration');
        assert.equal(hints.find((h) => h.position.line === nameLine)?.label, '1 file');
    });
});

describe('a config that is not valid JSON', () => {
    let client, ws, uri, text;

    // A missing comma, the most common way to break a config by hand.
    const broken = [
        '{',
        '    "artic-config": "2.0",',
        '    "projects": [',
        '    ]',
        '    "default-project": "nothing"',
        '}',
        '',
    ].join('\n');

    before(async () => {
        client = new LspClient(findServerBinary());
        await client.initialize(null);
        ws = stageFiles({
            'artic.json': broken,
            'main.art': 'fn broken_main() -> i32 { 0 }\n',
        });
        uri = ws.fileUri('artic.json');
        text = ws.read('artic.json');
        client.openDocument(uri, text, 'json');
        await client.settle(300);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('is reported where the parser gave up, not at the top of the file', () => {
        const [diag] = client.diagnosticsFor(uri).filter((d) => /Failed to parse json/.test(d.message));
        assert.ok(diag, `expected a parse error: ${JSON.stringify(client.diagnosticsFor(uri))}`);
        assert.equal(diag.range.start.line, lineOf(text, '"default-project"'),
            'the parser stops on the member that should have been preceded by a comma');
    });
});
