// How the extension finds the language server binary. The published VSIX shipped a
// Linux binary only, and the PATH fallback ran `which`, which does not exist on
// Windows — so on Windows the extension failed with "Artic binary not found" and
// there was no test to notice.

import { test, describe, before } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { pathToFileURL } from 'node:url';

import { repoRoot } from './helpers.mjs';

const extensionRoot = join('ext', 'root');

/** A host where only the listed files exist and nothing is on PATH. */
function host(platform, present, onPath = {}) {
    const madeExecutable = [];
    return {
        madeExecutable,
        platform,
        exists: (file) => present.includes(file),
        lookupOnPath: (command) => onPath[command],
        makeExecutable: (file) => madeExecutable.push(file),
    };
}

describe('server binary resolution', () => {
    let resolveServerPath;
    let bundledServerPath;
    let serverBinaryName;

    before(async () => {
        const bundleDir = mkdtempSync(join(tmpdir(), 'artic-server-path-'));
        const outFile = join(bundleDir, 'server-path.mjs');
        const esbuild = join(repoRoot, 'vscode', 'node_modules', 'esbuild', 'bin', 'esbuild');
        execFileSync(
            process.execPath,
            [esbuild, 'src/server-path.ts', '--bundle', '--format=esm', '--platform=node', `--outfile=${outFile}`],
            { cwd: join(repoRoot, 'vscode'), stdio: 'pipe' },
        );
        ({ resolveServerPath, bundledServerPath, serverBinaryName } = await import(pathToFileURL(outFile).href));
    });

    test('looks for the .exe on Windows and the bare name elsewhere', () => {
        assert.equal(serverBinaryName('win32'), 'artic-lsp.exe');
        assert.equal(serverBinaryName('linux'), 'artic-lsp');
        assert.match(bundledServerPath(extensionRoot, 'win32'), /artic-lsp\.exe$/);
        assert.match(bundledServerPath(extensionRoot, 'linux'), /artic-lsp$/);
    });

    test('finds the bundled Windows binary', () => {
        const bundled = bundledServerPath(extensionRoot, 'win32');
        assert.equal(resolveServerPath('', extensionRoot, host('win32', [bundled])), bundled);
    });

    test('falls back to PATH on Windows, where `which` does not exist', () => {
        const found = 'C:\\tools\\artic-lsp.exe';
        const h = host('win32', [], { 'artic-lsp.exe': found });
        assert.equal(resolveServerPath('', extensionRoot, h), found);
    });

    test('the setting wins over the bundled binary, but only when it exists', () => {
        const bundled = bundledServerPath(extensionRoot, 'linux');
        const configured = '/opt/artic/artic-lsp';
        assert.equal(
            resolveServerPath(configured, extensionRoot, host('linux', [configured, bundled])),
            configured,
        );
        assert.equal(
            resolveServerPath(configured, extensionRoot, host('linux', [bundled])),
            bundled,
            'a stale setting must not hide the bundled binary',
        );
    });

    test('restores the executable bit that vsce strips when packaging on Windows', () => {
        const bundled = bundledServerPath(extensionRoot, 'linux');
        const h = host('linux', [bundled]);
        resolveServerPath('', extensionRoot, h);
        assert.deepEqual(h.madeExecutable, [bundled]);

        const win = host('win32', [bundledServerPath(extensionRoot, 'win32')]);
        resolveServerPath('', extensionRoot, win);
        assert.deepEqual(win.madeExecutable, [], 'pointless on Windows');
    });

    test('reports every location it searched', () => {
        const configured = '/opt/artic/artic-lsp';
        assert.throws(
            () => resolveServerPath(configured, extensionRoot, host('linux', [])),
            (err) => {
                assert.match(err.message, /artic\.serverPath setting: \/opt\/artic\/artic-lsp/);
                assert.match(err.message, /bundled binary: /);
                assert.match(err.message, /artic-lsp on PATH/);
                return true;
            },
        );
    });
});
