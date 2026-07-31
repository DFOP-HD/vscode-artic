// A CMake-generated Visual Studio solution may be listed in the config instead of the
// individual .vcxproj files it contains.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFiles, fixturesDir, locate } from './helpers.mjs';

const geometryText = readFileSync(join(fixturesDir, 'simple', 'src', 'geometry.art'), 'utf8');
const mainText = readFileSync(join(fixturesDir, 'simple', 'src', 'main.art'), 'utf8');

function vcxproj(command) {
    return [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
        '  <ItemGroup>',
        '    <CustomBuild Include="build.rule">',
        `      <Command>${command}</Command>`,
        '    </CustomBuild>',
        '  </ItemGroup>',
        '</Project>',
    ].join('\n');
}

/** A solution in the exact shape CMake emits, including a non-artic project. */
function sln(entries) {
    const cxx = '{8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942}';
    const folder = '{2150E333-8FDC-42A3-9474-1B2A953CF13B}';
    const lines = [
        'Microsoft Visual Studio Solution File, Format Version 12.00',
        '# Visual Studio Version 17',
        // A solution folder: same syntax, but the path slot is not a project file.
        `Project("${folder}") = "CMakePredefinedTargets", "CMakePredefinedTargets", "{00000000-0000-0000-0000-0000000000FF}"`,
        'EndProject',
    ];
    entries.forEach(([name, rel], i) => {
        lines.push(`Project("${cxx}") = "${name}", "${rel}", "{00000000-0000-0000-0000-00000000000${i}}"`);
        lines.push('\tProjectSection(ProjectDependencies) = postProject');
        lines.push('\tEndProjectSection');
        lines.push('EndProject');
    });
    lines.push('Global', 'EndGlobal');
    return lines.join('\r\n');
}

describe('solution files', () => {
    let ws;
    let client;
    let mainUri;

    before(async () => {
        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
            'artic.json': JSON.stringify({
                'artic-config': '2.0',
                include: ['build/demo.sln'],
            }, null, 4),
        });

        const files = [ws.path('src', 'geometry.art'), ws.path('src', 'main.art')]
            .map((p) => p.replace(/\//g, '\\'))
            .join(' ');
        ws.write('build/simple.vcxproj', vcxproj(`artic.exe ${files} --emit-llvm`));
        // A project that has nothing to do with artic must be skipped silently.
        ws.write('build/ZERO_CHECK.vcxproj', vcxproj('setlocal &amp; cmake -E echo hi'));
        ws.write('build/demo.sln', sln([
            ['ZERO_CHECK', 'ZERO_CHECK.vcxproj'],
            ['simple', 'simple.vcxproj'],
            ['missing', 'nested\\missing.vcxproj'],
        ]));

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

    test('resolves a project reached through a .sln', async () => {
        const result = await client.request('textDocument/definition', {
            textDocument: { uri: mainUri },
            position: locate(mainText, 'dot(v, v)'),
        });
        const locations = Array.isArray(result) ? result : [result];
        assert.ok(locations.length > 0 && locations[0],
            'the solution must expand to the .vcxproj that lists both sources');
        assert.equal(normalizeUri(locations[0].uri),
            normalizeUri(ws.fileUri('src', 'geometry.art')),
            'cross-file navigation proves geometry.art came from the solution, not from a fallback');
    });

    test('does not complain about solution projects that are not artic projects', async () => {
        const configUri = ws.fileUri('artic.json');
        const slnUri = ws.fileUri('build', 'demo.sln');
        const zeroCheckUri = ws.fileUri('build', 'ZERO_CHECK.vcxproj');

        for (const uri of [configUri, slnUri, zeroCheckUri]) {
            const errors = client.diagnosticsFor(uri).filter((d) => d.severity === 1);
            assert.deepEqual(errors, [],
                `${uri} must not report errors, got ${JSON.stringify(errors)}`);
        }
    });

    test('warns about a project the solution references but that does not exist', async () => {
        const messages = client.diagnosticsFor(ws.fileUri('build', 'demo.sln'))
            .map((d) => d.message);
        assert.ok(messages.some((m) => /does not exist/.test(m)),
            `expected a warning about the missing project, got ${JSON.stringify(messages)}`);
    });
});
