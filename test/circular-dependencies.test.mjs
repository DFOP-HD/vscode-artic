// Cycle detection between projects. Project names are unique per test because the server
// keeps one project registry for the whole session and ignores duplicate definitions.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles } from './helpers.mjs';

const serverPath = findServerBinary();

const project = (name, dependencies) => ({
    name,
    files: [`src/${name}.art`],
    ...(dependencies ? { dependencies } : {}),
});

const sources = (...names) =>
    Object.fromEntries(names.map((n) => [`src/${n}.art`, `fn ${n}() -> i32 { 1 }\n`]));

const config = (projects) =>
    JSON.stringify({ 'artic-config': '2.0', projects }, null, 4);

const isCycle = (d) => d.message.startsWith('Circular dependency detected');
const cycleMessages = (diags) => new Set(diags.filter(isCycle).map((d) => d.message));

describe('circular project dependencies', () => {
    let client;

    before(async () => {
        client = new LspClient(serverPath);
        await client.initialize(null);
    });

    after(async () => {
        await client?.stop();
    });

    test('a three-project cycle is reported exactly once', async () => {
        const ws = stageFiles({
            'artic.json': config([
                project('cyc_a', ['cyc_b']),
                project('cyc_b', ['cyc_c']),
                project('cyc_c', ['cyc_a']),
            ]),
            ...sources('cyc_a', 'cyc_b', 'cyc_c'),
        });
        try {
            const configUri = ws.fileUri('artic.json');
            client.openDocument(configUri, ws.read('artic.json'), 'json');
            await client.waitForDiagnostics(configUri);
            await client.settle();

            // The same message is published at every occurrence of the offending name in
            // the file, so compare distinct messages: one cycle, one report.
            const cycles = cycleMessages(client.diagnosticsFor(configUri));
            assert.equal(cycles.size, 1, `expected one cycle diagnostic, got ${JSON.stringify([...cycles])}`);
        } finally {
            ws.cleanup();
        }
    });

    test('a project that depends on itself is reported once', async () => {
        const ws = stageFiles({
            'artic.json': config([project('self_a', ['self_a'])]),
            ...sources('self_a'),
        });
        try {
            const configUri = ws.fileUri('artic.json');
            client.openDocument(configUri, ws.read('artic.json'), 'json');
            await client.waitForDiagnostics(configUri);
            await client.settle();

            const cycles = cycleMessages(client.diagnosticsFor(configUri));
            assert.equal(cycles.size, 1, `expected one cycle diagnostic, got ${JSON.stringify([...cycles])}`);
        } finally {
            ws.cleanup();
        }
    });

    test('the cycle is broken, so the projects still compile', async () => {
        const ws = stageFiles({
            'artic.json': config([project('pair_a', ['pair_b']), project('pair_b', ['pair_a'])]),
            ...sources('pair_a', 'pair_b'),
        });
        try {
            client.openDocument(ws.fileUri('artic.json'), ws.read('artic.json'), 'json');
            const sourceUri = ws.fileUri('src', 'pair_a.art');
            client.openDocument(sourceUri, ws.read('src', 'pair_a.art'));
            await client.waitForDiagnostics(sourceUri);
            await client.settle();

            assert.deepEqual(client.diagnosticsFor(sourceUri), [],
                'breaking the cycle must leave a compilable project');
        } finally {
            ws.cleanup();
        }
    });
});
