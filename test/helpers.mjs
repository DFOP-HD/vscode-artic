// Shared helpers: locating the built server and staging fixture workspaces.

import { cpSync, existsSync, mkdirSync, mkdtempSync, rmSync, writeFileSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve, dirname } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

export const testDir = dirname(fileURLToPath(import.meta.url));
export const repoRoot = resolve(testDir, '..');
export const fixturesDir = join(testDir, 'fixtures');

const exe = process.platform === 'win32' ? '.exe' : '';

/**
 * Locates the artic-lsp binary. Override with ARTIC_LSP_BIN.
 * Searches every `artic-lsp/build*` directory so any of the documented
 * build configurations works without extra setup.
 */
export function findServerBinary() {
    if (process.env.ARTIC_LSP_BIN) {
        if (!existsSync(process.env.ARTIC_LSP_BIN)) {
            throw new Error(`ARTIC_LSP_BIN is set but does not exist: ${process.env.ARTIC_LSP_BIN}`);
        }
        return process.env.ARTIC_LSP_BIN;
    }
    const candidates = [];
    for (const dir of ['buildGcc', 'build', 'buildVS2022', 'build-test-msvc-ninja']) {
        candidates.push(join(repoRoot, 'artic-lsp', dir, 'bin', `artic-lsp${exe}`));
        candidates.push(join(repoRoot, 'artic-lsp', dir, 'bin', 'Release', `artic-lsp${exe}`));
    }
    candidates.push(join(repoRoot, 'vscode', 'build', 'bin', `artic-lsp${exe}`));

    const found = candidates.find((c) => existsSync(c));
    if (!found) {
        throw new Error(
            'Could not find the artic-lsp binary. Build it first (see AGENTS.md) or set ARTIC_LSP_BIN.\n' +
            `Looked in:\n  ${candidates.join('\n  ')}`);
    }
    return found;
}

/** Same discovery for the standalone compiler, used to validate fixtures. */
export function findArticBinary() {
    if (process.env.ARTIC_BIN) return process.env.ARTIC_BIN;
    for (const dir of ['buildGcc', 'build', 'buildVS2022']) {
        for (const sub of ['bin', join('bin', 'Release')]) {
            const p = join(repoRoot, 'artic-lsp', dir, sub, `artic${exe}`);
            if (existsSync(p)) return p;
        }
    }
    return null;
}

/**
 * Copies a fixture into a temp directory so tests may freely modify it.
 * Returns handles plus a `cleanup()` that the caller must invoke.
 */
export function stageFixture(name) {
    const src = join(fixturesDir, name);
    if (!existsSync(src)) throw new Error(`No such fixture: ${name}`);
    const dir = mkdtempSync(join(tmpdir(), `artic-lsp-${name}-`));
    cpSync(src, dir, { recursive: true });
    return {
        dir,
        uri: pathToFileURL(dir).href,
        path: (...parts) => join(dir, ...parts),
        fileUri: (...parts) => pathToFileURL(join(dir, ...parts)).href,
        read: (...parts) => readFileSync(join(dir, ...parts), 'utf8'),
        write: (relPath, content) => writeFileSync(join(dir, relPath), content),
        cleanup: () => rmSync(dir, { recursive: true, force: true }),
    };
}

/** Builds an empty workspace with the given files. Keys may contain subdirectories. */
export function stageFiles(files) {
    const dir = mkdtempSync(join(tmpdir(), 'artic-lsp-adhoc-'));
    const put = (relPath, content) => {
        const target = join(dir, relPath);
        mkdirSync(dirname(target), { recursive: true });
        writeFileSync(target, content);
    };
    for (const [relPath, content] of Object.entries(files)) put(relPath, content);
    return {
        dir,
        uri: pathToFileURL(dir).href,
        path: (...parts) => join(dir, ...parts),
        fileUri: (...parts) => pathToFileURL(join(dir, ...parts)).href,
        read: (...parts) => readFileSync(join(dir, ...parts), 'utf8'),
        write: put,
        cleanup: () => rmSync(dir, { recursive: true, force: true }),
    };
}

/** The server normalises URIs, so compare on the resolved path instead. */
export function uriToPath(uri) {
    return resolve(fileURLToPath(uri));
}

export function samePath(a, b) {
    const norm = (p) => resolve(p).replace(/\\/g, '/').toLowerCase();
    return norm(a) === norm(b);
}

/**
 * Zero-based LSP position of the `occurrence`-th appearance of `needle` in `text`.
 * Keeps navigation tests readable and immune to fixtures gaining or losing lines.
 */
export function locate(text, needle, occurrence = 1) {
    let idx = -1;
    for (let i = 0; i < occurrence; i++) {
        idx = text.indexOf(needle, idx + 1);
        if (idx === -1) {
            throw new Error(`Could not find occurrence ${occurrence} of ${JSON.stringify(needle)}`);
        }
    }
    const before = text.slice(0, idx);
    return {
        line: before.split('\n').length - 1,
        character: idx - (before.lastIndexOf('\n') + 1),
    };
}
