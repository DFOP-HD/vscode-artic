// Regression guard for path identity.
//
// A file reached through a CMake-generated .vcxproj is spelled `D:\...` (upper-case
// drive letter, native separators) while VS Code opens the same file as
// `file:///d:/...`. `std::filesystem::weakly_canonical` normalises neither, so the
// server used to end up with two identities for one file: everything keyed by path
// (the locator, the name map, `Loc::file`) missed, and semantic tokens, inlay hints
// and go-to-definition all silently returned nothing while diagnostics still worked.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

import { LspClient, normalizeUri } from './lsp-client.mjs';
import { findServerBinary, stageFiles, fixturesDir, locate } from './helpers.mjs';

/** `file:///C:/x` -> `file:///c:/x`, the spelling VS Code actually sends. */
function lowerCaseDrive(uri) {
    return uri.replace(/^file:\/\/\/([A-Za-z]):/, (_, d) => `file:///${d.toLowerCase()}:`);
}

/** `C:\x\y` with an upper-case drive and native separators, the way CMake writes it. */
function msBuildStylePath(p) {
    const native = p.replace(/\//g, '\\');
    return native.replace(/^([A-Za-z]):/, (_, d) => `${d.toUpperCase()}:`);
}

const isWindows = process.platform === 'win32';

describe('path identity across config sources', { skip: isWindows ? false : 'drive-letter casing is Windows-only' }, () => {
    let ws;
    let client;
    let mainUri;
    let mainText;
    let geometryText;

    before(async () => {
        geometryText = readFileSync(join(fixturesDir, 'simple', 'src', 'geometry.art'), 'utf8');
        mainText = readFileSync(join(fixturesDir, 'simple', 'src', 'main.art'), 'utf8');

        ws = stageFiles({
            'src/geometry.art': geometryText,
            'src/main.art': mainText,
            'artic.json': JSON.stringify({
                'artic-config': '2.0',
                include: ['build/mixedcase.vcxproj'],
            }, null, 4),
        });

        // Mimic the generated build system: absolute, back-slashed, upper-case drive.
        const files = [ws.path('src', 'geometry.art'), ws.path('src', 'main.art')]
            .map(msBuildStylePath)
            .join(' ');
        ws.write('build/mixedcase.vcxproj', [
            '<?xml version="1.0" encoding="utf-8"?>',
            '<Project ToolsVersion="4.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">',
            '  <ItemGroup>',
            '    <CustomBuild Include="build.rule">',
            `      <Command>artic.exe ${files} --emit-llvm</Command>`,
            '    </CustomBuild>',
            '  </ItemGroup>',
            '</Project>',
        ].join('\n'));

        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        await client.initialize(lowerCaseDrive(ws.uri));

        // Open with the drive letter spelled the way the editor spells it, which is the
        // opposite of what the .vcxproj contains.
        mainUri = lowerCaseDrive(ws.fileUri('src', 'main.art'));
        client.openDocument(lowerCaseDrive(ws.fileUri('src', 'geometry.art')), geometryText);
        client.openDocument(mainUri, mainText);
        await client.waitForDiagnostics(mainUri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('the fixture really does mix drive-letter casing', () => {
        const vcxproj = ws.read('build', 'mixedcase.vcxproj');
        const drive = ws.dir[0];
        assert.ok(vcxproj.includes(`${drive.toUpperCase()}:\\`),
            'the .vcxproj must reference an upper-case drive letter');
        assert.ok(mainUri.includes(`file:///${drive.toLowerCase()}:`),
            'the document must be opened with a lower-case drive letter');
    });

    test('semantic tokens still resolve for a vcxproj-provided file', async () => {
        const result = await client.request('textDocument/semanticTokens/full', {
            textDocument: { uri: mainUri },
        });
        assert.ok(result, 'server answered null: the file was not recognised as compiled');
        assert.ok(result.data.length > 0, 'expected semantic tokens, got none');
    });

    test('inlay hints still resolve for a vcxproj-provided file', async () => {
        const hints = await client.request('textDocument/inlayHint', {
            textDocument: { uri: mainUri },
            range: {
                start: { line: 0, character: 0 },
                end: { line: mainText.split('\n').length, character: 0 },
            },
        });
        assert.ok(Array.isArray(hints), 'server answered null instead of a hint array');
        assert.ok(hints.length > 0, 'expected inlay hints, got none');
    });

    test('go-to-definition still resolves for a vcxproj-provided file', async () => {
        const result = await client.request('textDocument/definition', {
            textDocument: { uri: mainUri },
            position: locate(mainText, 'dot(v, v)'),
        });
        const locations = Array.isArray(result) ? result : [result];
        assert.ok(locations.length > 0 && locations[0],
            'definition was not found: the request URI did not match the compiled file');
        assert.equal(normalizeUri(locations[0].uri),
            normalizeUri(lowerCaseDrive(ws.fileUri('src', 'geometry.art'))));
    });

    test('diagnostics are published under the URI spelling the editor used', async () => {
        const published = await client.waitForDiagnostics(mainUri);
        assert.match(published.uri, /^file:\/\/\/[a-z](:|%3A)/,
            `diagnostic URI must keep the editor's lower-case drive letter: ${published.uri}`);
    });
});
