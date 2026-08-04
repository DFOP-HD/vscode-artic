// A workspace that is already built with CMake or MSBuild needs no artic.json: the build
// system knows which files are compiled together, and it is the only thing that does.
//
// Without this the fallback is a single-file compile, so every cross-file reference is
// reported as an unknown identifier -- the first thing a new user hits.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFiles, fixturesDir, locate } from './helpers.mjs';

const geometryText = readFileSync(join(fixturesDir, 'simple', 'src', 'geometry.art'), 'utf8');
const mainText = readFileSync(join(fixturesDir, 'simple', 'src', 'main.art'), 'utf8');

const projectForFile = (client, uri) =>
    client.request('workspace/executeCommand', {
        command: 'artic.projectForFile',
        arguments: [uri],
    });

/** A build.ninja in the shape CMake generates. */
function buildNinja(buildDir, sources) {
    const files = sources.map((p) => p.replace(/\\/g, '/')).join(' ');
    const call = process.platform === 'win32'
        ? `C:\\WINDOWS\\system32\\cmd.exe /C "cd /D ${buildDir.replace(/\//g, '\\')} && D:\\anydsl\\bin\\artic.exe ${files} -emit-llvm -o src/demo.ll"`
        : `cd ${buildDir} && /opt/anydsl/bin/artic ${files} -emit-llvm -o src/demo.ll`;
    return [
        'ninja_required_version = 1.11',
        'build src/demo.ll | ${cmake_ninja_workdir}src/demo.ll: CUSTOM_COMMAND',
        `  COMMAND = ${call}`,
        '  DESC = Generating demo.ll',
        '',
    ].join('\n');
}

function vcxproj(sources) {
    const files = sources.map((p) => p.replace(/\//g, '\\')).join(' ');
    return [
        '<?xml version="1.0" encoding="utf-8"?>',
        '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
        '  <ItemGroup>',
        '    <CustomBuild Include="build.rule">',
        `      <Command>artic.exe ${files} --emit-llvm</Command>`,
        '    </CustomBuild>',
        '  </ItemGroup>',
        '</Project>',
    ].join('\n');
}

/** Opens both sources of the `simple` fixture and returns the URI of main.art. */
async function openSimpleSources(client, ws) {
    const mainUri = ws.fileUri('src', 'main.art');
    client.openDocument(ws.fileUri('src', 'geometry.art'), geometryText);
    client.openDocument(mainUri, mainText);
    return mainUri;
}

describe('a ninja build tree with no configuration file', () => {
    let ws, client, mainUri, diagnostics;

    before(async () => {
        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
        });
        ws.write('build/build.ninja', buildNinja(
            ws.path('build'),
            [ws.path('src', 'geometry.art'), ws.path('src', 'main.art')]));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        mainUri = await openSimpleSources(client, ws);
        ({ diagnostics } = await client.waitForDiagnostics(mainUri));
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('compiles the files the build file lists, so nothing is unknown', () => {
        assert.deepEqual(diagnostics.filter((d) => d.severity === 1), []);
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

    test('the answer is reported as coming from a build file, not a config', async () => {
        const info = await projectForFile(client, mainUri);
        assert.equal(info.provenance, 'detected');
        assert.match(info.origin.replace(/\\/g, '/'), /build\/build\.ninja$/);
        assert.equal(info.fileCount, 2);
    });
});

describe('a vcxproj with no configuration file', () => {
    let ws, client, mainUri, diagnostics;

    before(async () => {
        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
        });
        ws.write('build/demo.vcxproj', vcxproj(
            [ws.path('src', 'geometry.art'), ws.path('src', 'main.art')]));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        mainUri = await openSimpleSources(client, ws);
        ({ diagnostics } = await client.waitForDiagnostics(mainUri));
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('is picked up too', () => {
        assert.deepEqual(diagnostics.filter((d) => d.severity === 1), []);
    });

    test('and named after the project file', async () => {
        const info = await projectForFile(client, mainUri);
        assert.equal(info.provenance, 'detected');
        assert.equal(info.name, 'demo');
        assert.equal(info.fileCount, 2);
    });
});

describe('a configuration file next to a build file', () => {
    let ws, client, mainUri;

    before(async () => {
        // The config deliberately covers only one of the two sources, so "which one won"
        // is answerable from the file count alone.
        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
            'artic.json': JSON.stringify({
                'artic-config': '2.0',
                projects: [{ name: 'zero-config-explicit', files: ['src/**/*.art'] }],
            }, null, 4),
        });
        ws.write('build/build.ninja', buildNinja(
            ws.path('build'), [ws.path('src', 'main.art')]));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        mainUri = await openSimpleSources(client, ws);
        await client.waitForDiagnostics(mainUri);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('wins: what the user wrote is never overridden by a detected build file', async () => {
        const info = await projectForFile(client, mainUri);
        assert.equal(info.provenance, 'config');
        assert.equal(info.name, 'zero-config-explicit');
    });
});

describe('a workspace with nothing to detect', () => {
    let ws, client, mainUri;

    before(async () => {
        // Build files in directories the scan must not descend into. If it does, the
        // detection turns a deliberate single-file compile into someone else's project.
        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
        });
        for (const dir of ['node_modules/pkg/build', '.hidden/build', 'out/build']) {
            ws.write(`${dir}/build.ninja`, buildNinja(
                ws.path(...dir.split('/')),
                [ws.path('src', 'geometry.art'), ws.path('src', 'main.art')]));
        }

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        mainUri = await openSimpleSources(client, ws);
        await client.waitForDiagnostics(mainUri);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('stays a single-file compile', async () => {
        const info = await projectForFile(client, mainUri);
        assert.equal(info.provenance, 'single-file');
        assert.equal(info.fileCount, 1);
    });
});

describe('a file outside every workspace root', () => {
    let ws, outside, client;

    before(async () => {
        ws = stageFiles({ 'src/main.art': mainText });
        ws.write('build/build.ninja', buildNinja(ws.path('build'), [ws.path('src', 'main.art')]));
        outside = stageFiles({ 'stray.art': 'fn stray() -> i32 { 1 }\n' });

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        const strayUri = outside.fileUri('stray.art');
        client.openDocument(strayUri, outside.read('stray.art'));
        await client.waitForDiagnostics(strayUri);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); outside?.cleanup(); });

    test('is not swept into a project the scan found elsewhere', async () => {
        const info = await projectForFile(client, outside.fileUri('stray.art'));
        assert.equal(info.provenance, 'single-file');
    });
});

// A metaproject checkout holds many independent trees side by side, so the workspace root
// is the worst place to start looking: it is the one directory whose scan has to cross
// every unrelated tree in it.
describe('two build files claiming the same source', () => {
    let ws, client, mainUri;

    before(async () => {
        ws = stageFiles({
            'app/src/geometry.art': geometryText,
            'app/src/main.art': mainText,
        });
        // Sorts before `app/`, so it is what a scan starting at the workspace root reaches
        // first. It lists only one of the two sources, so which one won is answerable
        // from the file count alone.
        ws.write('aaa-build/outer.vcxproj', vcxproj([ws.path('app', 'src', 'main.art')]));
        ws.write('app/build/inner.vcxproj', vcxproj(
            [ws.path('app', 'src', 'geometry.art'), ws.path('app', 'src', 'main.art')]));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        mainUri = ws.fileUri('app', 'src', 'main.art');
        client.openDocument(ws.fileUri('app', 'src', 'geometry.art'), geometryText);
        client.openDocument(mainUri, mainText);
        await client.waitForDiagnostics(mainUri);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('the one nearest the source wins', async () => {
        const info = await projectForFile(client, mainUri);
        assert.equal(info.provenance, 'detected');
        assert.equal(info.name, 'inner');
        assert.equal(info.fileCount, 2);
    });
});

describe('a workspace whose scan budget is spent on an unrelated tree', () => {
    let ws, client, mainUri, diagnostics;

    before(async () => {
        ws = stageFiles({
            'app/src/geometry.art': geometryText,
            'app/src/main.art': mainText,
        });
        // More build files than the scan is allowed to collect, in a tree that sorts
        // before the one that matters -- an LLVM checkout beside the artic sources. A scan
        // that starts at the workspace root spends its whole budget here and reports that
        // the workspace contains nothing to detect.
        for (let i = 0; i < 2100; i++)
            ws.write(`aaa-noise/p${i}.vcxproj`, '<Project ToolsVersion="4.0" />');
        ws.write('app/build/demo.vcxproj', vcxproj(
            [ws.path('app', 'src', 'geometry.art'), ws.path('app', 'src', 'main.art')]));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        mainUri = ws.fileUri('app', 'src', 'main.art');
        client.openDocument(ws.fileUri('app', 'src', 'geometry.art'), geometryText);
        client.openDocument(mainUri, mainText);
        ({ diagnostics } = await client.waitForDiagnostics(mainUri));
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('still finds the build file beside the source', async () => {
        const info = await projectForFile(client, mainUri);
        assert.equal(info.provenance, 'detected');
        assert.equal(info.name, 'demo');
        assert.equal(info.fileCount, 2);
    });

    test('so nothing is unknown', () => {
        assert.deepEqual(diagnostics.filter((d) => d.severity === 1), []);
    });
});

describe('two checkouts of the same project side by side', () => {
    let ws, client, first, second, firstInfo, secondInfo, firstDiagnostics, secondDiagnostics;

    before(async () => {
        // A branch checked out next to its trunk is the normal way to work on a metaproject,
        // and both build trees name their project after the same CMake target. The registry
        // is keyed by that name, so the second checkout used to be dropped as a duplicate:
        // every file in it silently had no project, and which checkout lost depended on
        // which file the editor happened to open first.
        ws = stageFiles({
            'trunk/src/geometry.art': geometryText,
            'trunk/src/main.art': mainText,
            'branch/src/geometry.art': geometryText,
            'branch/src/main.art': mainText,
        });
        for (const checkout of ['trunk', 'branch']) {
            ws.write(`${checkout}/build/demo.vcxproj`, vcxproj([
                ws.path(checkout, 'src', 'geometry.art'),
                ws.path(checkout, 'src', 'main.art'),
            ]));
        }

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(ws.uri);
        first = ws.fileUri('trunk', 'src', 'main.art');
        second = ws.fileUri('branch', 'src', 'main.art');
        client.openDocument(first, mainText);
        ({ diagnostics: firstDiagnostics } = await client.waitForDiagnostics(first));
        firstInfo = await projectForFile(client, first);
        client.openDocument(second, mainText);
        ({ diagnostics: secondDiagnostics } = await client.waitForDiagnostics(second));
        secondInfo = await projectForFile(client, second);
    });
    after(async () => { await client?.stop(); ws?.cleanup(); });

    test('both checkouts get their own project', () => {
        assert.equal(firstInfo.provenance, 'detected');
        assert.equal(secondInfo.provenance, 'detected');
        assert.equal(firstInfo.fileCount, 2);
        assert.equal(secondInfo.fileCount, 2);
    });

    test('and each is compiled with the sources of its own checkout', () => {
        assert.ok(firstInfo.origin.includes('trunk'), `origin was ${firstInfo.origin}`);
        assert.ok(secondInfo.origin.includes('branch'), `origin was ${secondInfo.origin}`);
        assert.notEqual(firstInfo.name, secondInfo.name);
    });

    test('so nothing is unknown in either', () => {
        assert.deepEqual(firstDiagnostics.filter((d) => d.severity === 1), []);
        assert.deepEqual(secondDiagnostics.filter((d) => d.severity === 1), []);
    });
});
