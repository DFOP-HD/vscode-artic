// A CMake+Ninja build tree can be used as the project configuration: each ninja target
// whose command line invokes artic becomes a project.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFiles, fixturesDir, locate } from './helpers.mjs';

const geometryText = readFileSync(join(fixturesDir, 'simple', 'src', 'geometry.art'), 'utf8');
const mainText = readFileSync(join(fixturesDir, 'simple', 'src', 'main.art'), 'utf8');

/** A build.ninja in the shape CMake generates, including unrelated custom commands. */
function buildNinja(buildDir, sources) {
    const win = process.platform === 'win32';
    const files = sources.map((p) => p.replace(/\\/g, '/')).join(' ');
    const articCall = win
        ? `C:\\WINDOWS\\system32\\cmd.exe /C "cd /D ${buildDir.replace(/\//g, '\\')} && D:\\anydsl\\artic\\build\\bin\\artic.exe ${files} -emit-llvm -o src/demo.ll"`
        : `cd ${buildDir} && /opt/anydsl/bin/artic ${files} -emit-llvm -o src/demo.ll`;

    return [
        'ninja_required_version = 1.11',
        'builddir = .',
        '',
        '#############################################',
        '# Utility command for edit_cache',
        '',
        'build CMakeFiles/edit_cache.util: CUSTOM_COMMAND',
        '  COMMAND = cmake-gui -S.. -B.',
        '  DESC = Running CMake cache editor...',
        '  pool = console',
        '',
        '#############################################',
        '# Custom command for src/demo.ll',
        '',
        'build src/demo.ll | ${cmake_ninja_workdir}src/demo.ll: CUSTOM_COMMAND',
        `  COMMAND = ${articCall}`,
        '  DESC = Generating demo.ll',
        '  restat = 1',
        '',
        'build all: phony src/demo.ll',
    ].join('\n');
}

describe('ninja build files as configuration', () => {
    let ws;
    let client;
    let mainUri;

    before(async () => {
        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
            'artic.json': JSON.stringify({
                'artic-config': '2.0',
                include: ['build/build.ninja'],
            }, null, 4),
        });
        ws.write('build/build.ninja', buildNinja(
            ws.path('build'),
            [ws.path('src', 'geometry.art'), ws.path('src', 'main.art')]));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);

        mainUri = ws.fileUri('src', 'main.art');
        client.openDocument(ws.fileUri('src', 'geometry.art'), geometryText);
        client.openDocument(mainUri, mainText);
        await client.waitForDiagnostics(mainUri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('resolves a project taken from a ninja target', async () => {
        const result = await client.request('textDocument/definition', {
            textDocument: { uri: mainUri },
            position: locate(mainText, 'dot(v, v)'),
        });
        const locations = Array.isArray(result) ? result : [result];
        assert.ok(locations.length > 0 && locations[0],
            'both sources must be compiled together, which only the ninja target says');
        assert.equal(normalizeUri(locations[0].uri),
            normalizeUri(ws.fileUri('src', 'geometry.art')));
    });

    test('ignores custom commands that do not invoke artic', async () => {
        const errors = client.diagnosticsFor(ws.fileUri('build', 'build.ninja'))
            .filter((d) => d.severity === 1);
        assert.deepEqual(errors, [],
            `the cmake-gui command must not be mistaken for a project: ${JSON.stringify(errors)}`);
    });

    test('warns when a ninja file contains no artic commands', async () => {
        const bare = stageFiles({
            'src/main.art': 'fn main() -> i32 { 0 }\n',
            'artic.json': JSON.stringify({
                'artic-config': '2.0',
                include: ['build/build.ninja'],
            }, null, 4),
        });
        bare.write('build/build.ninja', 'build all: phony\n');

        const bareClient = new LspClient(findServerBinary(), { cwd: bare.dir });
        try {
            await bareClient.initialize(bare.uri);
            bareClient.openDocument(bare.fileUri('src', 'main.art'), bare.read('src', 'main.art'));
            await bareClient.settle();

            const messages = bareClient.diagnosticsFor(bare.fileUri('build', 'build.ninja'))
                .map((d) => d.message);
            assert.ok(messages.some((m) => /No artic build commands found/.test(m)),
                `expected a warning, got ${JSON.stringify(messages)}`);
        } finally {
            await bareClient.stop();
            bare.cleanup();
        }
    });
});
