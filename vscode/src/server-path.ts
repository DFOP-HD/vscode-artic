import * as path from 'path';

/** Everything the lookup needs from the host, so it can be exercised without a real filesystem. */
export interface ServerPathHost {
    platform: NodeJS.Platform;
    exists(file: string): boolean;
    /** Resolves an executable through the system PATH, or undefined when it is not there. */
    lookupOnPath(command: string): string | undefined;
    makeExecutable(file: string): void;
}

export function serverBinaryName(platform: NodeJS.Platform): string {
    return platform === 'win32' ? 'artic-lsp.exe' : 'artic-lsp';
}

export function bundledServerPath(extensionRoot: string, platform: NodeJS.Platform): string {
    return path.join(extensionRoot, 'build', 'bin', serverBinaryName(platform));
}

/** Setting, then the binary shipped in the extension, then PATH. */
export function resolveServerPath(configured: string, extensionRoot: string, host: ServerPathHost): string {
    const searched: string[] = [];

    if (configured) {
        if (host.exists(configured)) {
            return configured;
        }
        searched.push(`artic.serverPath setting: ${configured}`);
    }

    const bundled = bundledServerPath(extensionRoot, host.platform);
    if (host.exists(bundled)) {
        // vsce drops POSIX permissions when the package is built on Windows, so a bundled
        // Linux binary can arrive without its executable bit.
        if (host.platform !== 'win32') {
            host.makeExecutable(bundled);
        }
        return bundled;
    }
    searched.push(`bundled binary: ${bundled}`);

    const name = serverBinaryName(host.platform);
    const onPath = host.lookupOnPath(name);
    if (onPath) {
        return onPath;
    }
    searched.push(`${name} on PATH`);

    throw new Error(`Artic language server binary not found. Searched:\n  ${searched.join('\n  ')}`);
}
