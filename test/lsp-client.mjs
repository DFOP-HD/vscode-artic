// Minimal dependency-free LSP client used by the test suite.
// Speaks JSON-RPC over stdio to a real artic-lsp process.

import { spawn } from 'node:child_process';
import { once } from 'node:events';
import { fileURLToPath, pathToFileURL } from 'node:url';

/**
 * Canonical form for comparing file URIs.
 * VS Code encodes the drive colon (`file:///C%3A/...`) while Node does not
 * (`file:///C:/...`); both denote the same document.
 */
export function normalizeUri(uri) {
    try {
        const href = pathToFileURL(fileURLToPath(uri)).href;
        return process.platform === 'win32' ? href.toLowerCase() : href;
    } catch {
        return uri;
    }
}

export class LspClient {
    #proc;
    #buffer = Buffer.alloc(0);
    #nextId = 1;
    #pending = new Map();
    #notificationWaiters = [];

    /** Every diagnostics notification received, in arrival order. */
    diagnosticsLog = [];
    /** Latest diagnostics per document URI. */
    diagnostics = new Map();
    stderr = '';

    constructor(serverPath, { cwd } = {}) {
        this.#proc = spawn(serverPath, ['--lsp'], { cwd, stdio: ['pipe', 'pipe', 'pipe'] });
        this.#proc.stdout.on('data', (chunk) => this.#onStdout(chunk));
        this.#proc.stderr.on('data', (chunk) => { this.stderr += chunk.toString(); });
        // A write that loses the race with the server closing stdin raises EPIPE on
        // Linux; unhandled, it surfaces as an uncaughtException long after the test.
        this.#proc.stdin.on('error', () => {});
    }

    #onStdout(chunk) {
        this.#buffer = Buffer.concat([this.#buffer, chunk]);
        for (;;) {
            const headerEnd = this.#buffer.indexOf('\r\n\r\n');
            if (headerEnd === -1) return;
            const header = this.#buffer.subarray(0, headerEnd).toString('ascii');
            const match = /Content-Length:\s*(\d+)/i.exec(header);
            if (!match) {
                // Not a valid header: drop the junk and resynchronise.
                this.#buffer = this.#buffer.subarray(headerEnd + 4);
                continue;
            }
            const length = Number(match[1]);
            const bodyStart = headerEnd + 4;
            if (this.#buffer.length < bodyStart + length) return;
            const body = this.#buffer.subarray(bodyStart, bodyStart + length).toString('utf8');
            this.#buffer = this.#buffer.subarray(bodyStart + length);
            this.#dispatch(JSON.parse(body));
        }
    }

    #dispatch(msg) {
        if (msg.id !== undefined && (msg.result !== undefined || msg.error !== undefined)) {
            const pending = this.#pending.get(msg.id);
            if (!pending) return;
            this.#pending.delete(msg.id);
            msg.error ? pending.reject(new Error(JSON.stringify(msg.error))) : pending.resolve(msg.result);
            return;
        }
        if (msg.method === 'textDocument/publishDiagnostics') {
            this.diagnosticsLog.push(msg.params);
            this.diagnostics.set(normalizeUri(msg.params.uri), msg.params.diagnostics);
        }
        if (msg.method !== undefined && msg.id !== undefined) {
            // Server-to-client request: nothing needs a real answer yet.
            this.#send({ jsonrpc: '2.0', id: msg.id, result: null });
        }
        for (const waiter of this.#notificationWaiters.slice()) {
            if (waiter.predicate(msg)) {
                this.#notificationWaiters.splice(this.#notificationWaiters.indexOf(waiter), 1);
                waiter.resolve(msg);
            }
        }
    }

    #send(msg) {
        const stdin = this.#proc.stdin;
        if (this.#proc.exitCode !== null || !stdin.writable) return;
        const content = Buffer.from(JSON.stringify(msg), 'utf8');
        stdin.write(`Content-Length: ${content.length}\r\n\r\n`);
        stdin.write(content);
    }

    request(method, params, { timeout = 20000 } = {}) {
        const id = this.#nextId++;
        this.#send({ jsonrpc: '2.0', id, method, params });
        return new Promise((resolve, reject) => {
            const timer = setTimeout(
                () => reject(new Error(`Timed out waiting for response to '${method}'.\nServer stderr:\n${this.stderr}`)),
                timeout);
            this.#pending.set(id, {
                resolve: (v) => { clearTimeout(timer); resolve(v); },
                reject: (e) => { clearTimeout(timer); reject(e); },
            });
        });
    }

    notify(method, params) {
        this.#send({ jsonrpc: '2.0', method, params });
    }

    /** Resolves with the first incoming message matching `predicate`. */
    waitForNotification(predicate, { timeout = 20000, description = 'notification' } = {}) {
        return new Promise((resolve, reject) => {
            const waiter = { predicate, resolve: (m) => { clearTimeout(timer); resolve(m); } };
            const timer = setTimeout(() => {
                const i = this.#notificationWaiters.indexOf(waiter);
                if (i !== -1) this.#notificationWaiters.splice(i, 1);
                reject(new Error(`Timed out waiting for ${description}.\nServer stderr:\n${this.stderr}`));
            }, timeout);
            this.#notificationWaiters.push(waiter);
        });
    }

    /** Waits for a diagnostics notification targeting `uri`. */
    waitForDiagnostics(uri, { timeout = 20000 } = {}) {
        const wanted = normalizeUri(uri);
        const existing = this.diagnosticsLog.find((p) => normalizeUri(p.uri) === wanted);
        if (existing) return Promise.resolve(existing);
        return this.waitForNotification(
            (m) => m.method === 'textDocument/publishDiagnostics' && normalizeUri(m.params.uri) === wanted,
            { timeout, description: `diagnostics for ${uri}` },
        ).then((m) => m.params);
    }

    /** Latest diagnostics for a document, regardless of URI spelling. */
    diagnosticsFor(uri) {
        return this.diagnostics.get(normalizeUri(uri)) ?? [];
    }

    /**
     * Gives the server `ms` of quiet time to emit any further notifications.
     * Used to assert that something is *not* published.
     */
    settle(ms = 600) {
        return new Promise((resolve) => setTimeout(resolve, ms));
    }

    async initialize(rootUri) {
        const result = await this.request('initialize', {
            processId: process.pid,
            rootUri,
            capabilities: {
                textDocument: {
                    publishDiagnostics: {},
                    definition: { linkSupport: false },
                    completion: { completionItem: { snippetSupport: true } },
                },
                workspace: { workspaceFolders: true },
            },
            workspaceFolders: rootUri ? [{ uri: rootUri, name: 'test' }] : null,
        });
        this.notify('initialized', {});
        return result;
    }

    openDocument(uri, text, languageId = 'artic') {
        this.notify('textDocument/didOpen', {
            textDocument: { uri, languageId, version: 1, text },
        });
    }

    changeDocument(uri, text, version = 2) {
        this.notify('textDocument/didChange', {
            textDocument: { uri, version },
            contentChanges: [{ text }],
        });
    }

    saveDocument(uri, text) {
        this.notify('textDocument/didSave', { textDocument: { uri }, text });
    }

    closeDocument(uri) {
        this.notify('textDocument/didClose', { textDocument: { uri } });
    }

    /**
     * Forgets every diagnostics notification received so far, so a later
     * `waitForDiagnostics()` waits for a fresh one instead of replaying the log.
     */
    clearDiagnosticsLog() {
        this.diagnosticsLog.length = 0;
    }

    async stop() {
        try {
            await this.request('shutdown', null, { timeout: 3000 });
            this.notify('exit', null);
        } catch {
            // Server already gone or unresponsive; fall through to kill.
        }
        if (this.#proc.exitCode === null) {
            this.#proc.kill();
            await Promise.race([once(this.#proc, 'exit'), new Promise((r) => setTimeout(r, 2000))]);
        }
    }
}
