// A build command is a shell command line, so a path containing a space is quoted. Both
// build-file parsers used to split on whitespace and strip stray quotes afterwards, which
// turned `"C:\my sources\main.art"` into two paths that do not exist -- the project then
// silently expanded to nothing and every cross-file reference became an unknown identifier.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles, fixturesDir, locate } from './helpers.mjs';

const geometryText = readFileSync(join(fixturesDir, 'simple', 'src', 'geometry.art'), 'utf8');
const mainText = readFileSync(join(fixturesDir, 'simple', 'src', 'main.art'), 'utf8');

// The workspace root itself cannot be given a space (the temp dir is not ours), so the
// sources live in a directory that has one.
const sourceDir = 'my sources';

const projectForFile = (client, uri) =>
    client.request('workspace/executeCommand', {
        command: 'artic.projectForFile',
        arguments: [uri],
    });

/** A build.ninja whose artic invocation quotes every source path. */
function buildNinja(buildDir, sources) {
    const quoted = (sep) => sources.map((p) => `"${p.replace(/[\\/]/g, sep)}"`).join(' ');
    const call = process.platform === 'win32'
        ? `C:\\WINDOWS\\system32\\cmd.exe /C "cd /D ${buildDir.replace(/\//g, '\\')} && "D:\\any dsl\\bin\\artic.exe" ${quoted('\\')} -emit-llvm -o src/demo.ll"`
        : `cd ${buildDir} && "/opt/any dsl/bin/artic" ${quoted('/')} -emit-llvm -o src/demo.ll`;
    return [
        'ninja_required_version = 1.11',
        'build src/demo.ll | ${cmake_ninja_workdir}src/demo.ll: CUSTOM_COMMAND',
        `  COMMAND = ${call}`,
        '  DESC = Generating demo.ll',
        '',
    ].join('\n');
}

/** A .vcxproj whose custom build step quotes every source path, XML-escaped as MSBuild writes it. */
function vcxproj(sources) {
    const files = sources.map((p) => `&quot;${p.replace(/\//g, '\\')}&quot;`).join(' ');
    return [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
        '  <ItemGroup>',
        '    <CustomBuild Include="build.rule">',
        `      <Command>&quot;C:\\any dsl\\bin\\artic.exe&quot; ${files} -emit-llvm -o out.ll</Command>`,
        '    </CustomBuild>',
        '  </ItemGroup>',
        '</Project>',
    ].join('\n');
}

/** Stages the two `simple` sources under a directory whose name contains a space. */
function stageSpacedSources() {
    return stageFiles({
        [`${sourceDir}/geometry.art`]: geometryText,
        [`${sourceDir}/main.art`]: mainText,
    });
}

/**
 * Every check is the same: the two files must have been compiled together, which only
 * happens if both quoted paths survived tokenisation.
 */
function itResolvesBothFiles(name, writeBuildFile, buildFileName) {
    describe(name, () => {
        let ws, client, mainUri, diagnostics;

        before(async () => {
            ws = stageSpacedSources();
            writeBuildFile(ws);

            client = new LspClient(findServerBinary(), { cwd: ws.dir });
            await client.initialize(ws.uri);
            mainUri = ws.fileUri(sourceDir, 'main.art');
            client.openDocument(ws.fileUri(sourceDir, 'geometry.art'), geometryText);
            client.openDocument(mainUri, mainText);
            ({ diagnostics } = await client.waitForDiagnostics(mainUri));
        });
        after(async () => { await client?.stop(); ws?.cleanup(); });

        test('a quoted path containing a space stays one file', () => {
            assert.deepEqual(diagnostics.filter((d) => d.severity === 1), [],
                'both sources are listed, so nothing may be unknown');
        });

        test('go-to-definition crosses into the other file', async () => {
            const result = await client.request('textDocument/definition', {
                textDocument: { uri: mainUri },
                position: locate(mainText, 'scale(a'),
            });
            const locations = Array.isArray(result) ? result : [result];
            assert.ok(locations.length > 0, 'expected a definition');
            assert.match(locations[0].uri, /geometry\.art$/);
        });

        test('the project holds exactly the two sources, not the options', async () => {
            const info = await projectForFile(client, mainUri);
            assert.equal(info.provenance, 'detected');
            assert.match(info.origin.replace(/\\/g, '/'), new RegExp(`${buildFileName}$`));
            assert.equal(info.fileCount, 2,
                'a single-dash option must not be mistaken for a source file');
        });
    });
}

itResolvesBothFiles(
    'a ninja command with quoted paths',
    (ws) => ws.write('build/build.ninja', buildNinja(
        ws.path('build'),
        [ws.path(sourceDir, 'geometry.art'), ws.path(sourceDir, 'main.art')])),
    'build/build\\.ninja');

itResolvesBothFiles(
    'a vcxproj command with quoted paths',
    (ws) => ws.write('build/demo.vcxproj', vcxproj(
        [ws.path(sourceDir, 'geometry.art'), ws.path(sourceDir, 'main.art')])),
    'build/demo\\.vcxproj');
