// The "Detect workspace configuration" command picks which build files end up in
// artic.json. A .sln never contains the word "artic" — it only lists project names and
// GUIDs — so it has to be judged by the projects it references.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';
import { resolve, sep } from 'node:path';

import { importExtensionModule } from './helpers.mjs';

const root = process.platform === 'win32' ? 'D:\\ws' : '/ws';
const p = (...parts) => resolve(root, ...parts);

const articProject = `<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0">
  <ItemGroup><CustomBuild Include="kernel.impala">
    <Command>D:\\anydsl\\artic.exe kernel.impala -emit-llvm</Command>
  </CustomBuild></ItemGroup>
</Project>
`;
const plainProject = `<?xml version="1.0" encoding="utf-8"?>
<Project ToolsVersion="4.0"><ItemGroup /></Project>
`;

function solution(entries) {
    const lines = ['Microsoft Visual Studio Solution File, Format Version 12.00'];
    for (const [name, relativePath] of entries) {
        const type = relativePath.endsWith('.vcxproj')
            ? '8BC9CEB8-8B4A-11D0-8D11-00A0C91BC942'
            : '2150E333-8FDC-42A3-9474-1B2A953CF13B'; // solution folder
        lines.push(`Project("{${type}}") = "${name}", "${relativePath}", "{00000000-0000-0000-0000-00000000000${lines.length}}"`);
        lines.push('EndProject');
    }
    return lines.join('\n');
}

describe('workspace configuration detection', () => {
    let selectWorkspaceConfigFiles;
    let cleanup;

    before(async () => {
        // The logic lives in TypeScript next to the extension; bundle it so it can be
        // exercised without a VS Code instance.
        const bundle = await importExtensionModule('detect.ts');
        cleanup = bundle.cleanup;
        ({ selectWorkspaceConfigFiles } = bundle.module);
    });

    test('prefers a solution over the projects it lists', () => {
        const selected = selectWorkspaceConfigFiles([
            { fsPath: p('build', 'STINCILLA.sln'), content: solution([
                ['ALL_BUILD', 'ALL_BUILD.vcxproj'],
                ['tests', 'tests'],
                ['bilateral', `test${sep}bilateral.vcxproj`],
            ]) },
            { fsPath: p('build', 'ALL_BUILD.vcxproj'), content: plainProject },
            { fsPath: p('build', 'test', 'bilateral.vcxproj'), content: articProject },
        ]);
        assert.deepEqual(selected, [p('build', 'STINCILLA.sln')]);
    });

    test('a solution that lists no artic project is not a configuration', () => {
        const selected = selectWorkspaceConfigFiles([
            { fsPath: p('build', 'Other.sln'), content: solution([['ALL_BUILD', 'ALL_BUILD.vcxproj']]) },
            { fsPath: p('build', 'ALL_BUILD.vcxproj'), content: plainProject },
        ]);
        assert.deepEqual(selected, []);
    });

    test('falls back to the artic projects when there is no solution', () => {
        const selected = selectWorkspaceConfigFiles([
            { fsPath: p('build', 'ALL_BUILD.vcxproj'), content: plainProject },
            { fsPath: p('build', 'kernel.vcxproj'), content: articProject },
        ]);
        assert.deepEqual(selected, [p('build', 'kernel.vcxproj')]);
    });

    test('a ninja file supersedes the projects beside it', () => {
        const selected = selectWorkspaceConfigFiles([
            { fsPath: p('build', 'build.ninja'), content: '  COMMAND = artic a.art -o out\n' },
            { fsPath: p('build', 'sub', 'kernel.vcxproj'), content: articProject },
        ]);
        assert.deepEqual(selected, [p('build', 'build.ninja')]);
    });

    test('a ninja file without artic commands is ignored', () => {
        const selected = selectWorkspaceConfigFiles([
            { fsPath: p('build', 'build.ninja'), content: 'build all: phony\n' },
        ]);
        assert.deepEqual(selected, []);
    });

    test('keeps projects that live outside the covered directory', () => {
        const selected = selectWorkspaceConfigFiles([
            { fsPath: p('build', 'a.sln'), content: solution([['k', 'kernel.vcxproj']]) },
            { fsPath: p('build', 'kernel.vcxproj'), content: articProject },
            { fsPath: p('other', 'standalone.vcxproj'), content: articProject },
        ]);
        assert.deepEqual(selected, [p('build', 'a.sln'), p('other', 'standalone.vcxproj')]);
    });

    after(() => cleanup());
});
