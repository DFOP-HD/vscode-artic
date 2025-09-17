import * as vscode from 'vscode';
import * as path from 'path';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync, writeFileSync } from 'fs';

let client: LanguageClient;

// Option A (pull) implementation: server will parse artic.json/global config.
// Client only supplies paths via initializationOptions.

function discoverWorkspaceConfigPath(): string | undefined {
    const folders = vscode.workspace.workspaceFolders;
    if (!folders || folders.length === 0) return undefined;
    const candidate = path.join(folders[0].uri.fsPath, 'artic.json');
    return existsSync(candidate) ? candidate : undefined;
}

function resolveGlobalConfigPath(): string | undefined {
    const cfg = vscode.workspace.getConfiguration('artic');
    const p = cfg.get<string>('globalConfig', '').trim();
    if (!p) return undefined;
    if (p.startsWith('~')) return path.join(process.env.HOME || '', p.slice(1));
    return p;
}

function findArticBinary(): string {
    // Prefer bundled binary inside the extension, then config, then PATH, then workspace
    const bundled = path.join(__dirname, '..', 'artic', 'build', 'bin', 'artic');
    if (existsSync(bundled)) {
        return bundled;
    }

    const config = vscode.workspace.getConfiguration('artic');
    let serverPath = config.get<string>('serverPath', '');
    if (serverPath && existsSync(serverPath)) {
        return serverPath;
    }

    const extensionArticPath = path.join(__dirname, '..', 'artic-lsp', 'build', 'bin', 'artic');

    const possiblePaths = [
        extensionArticPath, // Packaged
        'artic', // on PATH
        path.join(vscode.workspace.workspaceFolders?.[0]?.uri.fsPath || '', 'artic-lsp/build/bin/artic'), // Workspace
    ];

    for (const testPath of possiblePaths) {
        try {
            if (testPath === 'artic') {
                execSync('which artic', { stdio: 'ignore' });
                return 'artic';
            } else if (existsSync(testPath)) {
                return testPath;
            }
        } catch {
            // continue
        }
    }

    throw new Error('Artic binary not found. Packaged binary missing and none found in settings or PATH.');
}

export function activate(context: vscode.ExtensionContext) {
    try {
        const serverPath = findArticBinary();
        
        // Server options - run the artic binary with --lsp flag
        const serverOptions: ServerOptions = {
            command: serverPath,
            args: ['--lsp'],
            transport: TransportKind.stdio,
            options: {
                // Set working directory to workspace root if available
                cwd: vscode.workspace.workspaceFolders?.[0]?.uri.fsPath
            }
        };

        // Client options
        const clientOptions: LanguageClientOptions = {
            documentSelector: [ { scheme: 'file', language: 'artic' } ],
            synchronize: {
                fileEvents: [
                    vscode.workspace.createFileSystemWatcher('**/*.art'),
                    vscode.workspace.createFileSystemWatcher('**/*.impala'),
                    // Watch workspace project configuration file(s)
                    vscode.workspace.createFileSystemWatcher('**/artic.json'),
                    vscode.workspace.createFileSystemWatcher('**/artic-global.json')
                ]
            },
            outputChannelName: 'Artic Language Server',
            traceOutputChannel: vscode.window.createOutputChannel('Artic Language Server Trace')
        };

        const workspaceConfigPath = discoverWorkspaceConfigPath();
        const globalConfigPath = resolveGlobalConfigPath();

        // Create the language client
        client = new LanguageClient(
            'articLanguageServer',
            'Artic Language Server',
            serverOptions,
            {
                ...clientOptions,
                initializationOptions: {
                    workspaceConfig: workspaceConfigPath,
                    globalConfig: globalConfigPath
                }
            }
        );

        // Start the client (which also starts the server)
        client.start().then(async () => {
            console.log('Artic Language Server started successfully');
            client.outputChannel?.show(true);

            const workspaceRoot = vscode.workspace.workspaceFolders?.[0]?.uri.fsPath;
            const defaultWorkspaceConfig = workspaceRoot ? path.join(workspaceRoot, 'artic.json') : undefined;
            const defaultGlobalConfig = path.join(process.env.HOME || '', 'artic-global.json');

            const ensureConfig = async (kind: 'workspace' | 'global') => {
                const targetPath = kind === 'workspace' ? defaultWorkspaceConfig : defaultGlobalConfig;
                if (!targetPath) return;
                if (existsSync(targetPath)) return; // already exists
                const createLabel = kind === 'workspace' ? 'Create workspace artic.json' : 'Create global artic-global.json';
                const detail = kind === 'workspace'
                    ? 'Create an artic.json in the workspace root so projects can be configured.'
                    : 'Create a global artic-global.json in your home directory for shared projects.';
                const choice = await vscode.window.showInformationMessage(detail, createLabel, 'Dismiss');
                                if (choice === createLabel) {
                                        const template = `{
    "artic-config": "1.0",
    "projects": [
        {
            "name": "main",
            "files": ["src/**/*.impala", "src/**/*.art", "*.impala", "*.art"],
            "dependencies": []
        }
    ],
    "default-project": { "name": "main" }
}`;
                                        try {
                        writeFileSync(targetPath, template, { flag: 'wx' });
                        const doc = await vscode.workspace.openTextDocument(targetPath);
                        await vscode.window.showTextDocument(doc);
                        // Update initializationOptions & restart so server sees new config path
                        (client.clientOptions as any).initializationOptions = {
                            workspaceConfig: (kind === 'workspace') ? targetPath : (workspaceConfigPath || defaultWorkspaceConfig),
                            globalConfig: (kind === 'global') ? targetPath : (globalConfigPath || defaultGlobalConfig)
                        };
                        await client.restart();
                        vscode.window.showInformationMessage(`${kind === 'workspace' ? 'Workspace' : 'Global'} Artic config created and server restarted.`);
                    } catch (e: any) {
                        vscode.window.showErrorMessage(`Failed to create ${kind} config: ${e.message}`);
                    }
                }
            };

            if (!workspaceConfigPath && defaultWorkspaceConfig) {
                await ensureConfig('workspace');
            }
            if (!globalConfigPath) {
                await ensureConfig('global');
            }
        }).catch(error => {
            console.error('Failed to start Artic Language Server:', error);
            vscode.window.showErrorMessage(`Failed to start Artic Language Server: ${error.message}`);
        });
        // Register commands
        const restartCommand = vscode.commands.registerCommand('artic.restart', async () => {
            if (client) {
                client.clientOptions.initializationOptions = {
                    workspaceConfig: discoverWorkspaceConfigPath(),
                    globalConfig: resolveGlobalConfigPath()
                }
                if (client.isRunning()) await client.restart();
                client.outputChannel?.show(true);
                vscode.window.showInformationMessage('Artic Language Server restarted');
            }
        });
        context.subscriptions.push(restartCommand);

        const reloadConfigCommand = vscode.commands.registerCommand('artic.reloadConfig', async () => {
            try {
                if (!client) return;
                // Send a workspace/didChangeConfiguration to encourage servers that rely on it
                await client.sendNotification('workspace/didChangeConfiguration', { settings: {} });
                // Also send custom request (fire and forget) if server implements it
                // We name it artic/reloadWorkspace (request or notification). Using notification for simplicity.
                await client.sendNotification('artic/reloadWorkspace');
                client.outputChannel?.appendLine('Sent reload configuration notification');
                vscode.window.showInformationMessage('Artic configuration reload requested');
            } catch (e:any) {
                vscode.window.showErrorMessage(`Failed to send reload: ${e.message}`);
            }
        });
        context.subscriptions.push(reloadConfigCommand);
    
    } catch (error: any) {
        console.error('Failed to start Artic Language Server:', error);
        vscode.window.showErrorMessage(`Failed to start Artic Language Server: ${error.message}`);
    }
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}
