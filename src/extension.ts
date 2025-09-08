import * as vscode from 'vscode';
import * as path from 'path';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync } from 'fs';

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
                    vscode.workspace.createFileSystemWatcher('**/artic.json')
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
        client.start().then(() => {
            console.log('Artic Language Server started successfully');
            vscode.window.showInformationMessage(`global config: ${globalConfigPath}`);
            vscode.window.showInformationMessage(`workspace config: ${workspaceConfigPath}`);
            client.outputChannel?.show(true);
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
