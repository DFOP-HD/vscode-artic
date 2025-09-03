import * as vscode from 'vscode';
import * as path from 'path';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync } from 'fs';

let client: LanguageClient;

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
            // Register the server for Artic files
            documentSelector: [
                { scheme: 'file', language: 'artic' }
            ],
            synchronize: {
                // Notify the server about file changes to .art files
                fileEvents: [vscode.workspace.createFileSystemWatcher('**/*.art'), vscode.workspace.createFileSystemWatcher('**/*.impala')]
            },
            // Output channel for debugging
            outputChannelName: 'Artic Language Server',
            // Trace setting
            traceOutputChannel: vscode.window.createOutputChannel('Artic Language Server Trace')
        };

        // Create the language client
        client = new LanguageClient(
            'articLanguageServer',
            'Artic Language Server',
            serverOptions,
            clientOptions
        );

        // Start the client (which also starts the server)
        client.start().then(() => {
            console.log('Artic Language Server started successfully');
        }).catch(error => {
            console.error('Failed to start Artic Language Server:', error);
            vscode.window.showErrorMessage(`Failed to start Artic Language Server: ${error.message}`);
        });

        // Register commands
        const restartCommand = vscode.commands.registerCommand('artic.restart', async () => {
            if (client) {
                await client.restart();
                vscode.window.showInformationMessage('Artic Language Server restarted');
            }
        });

        context.subscriptions.push(restartCommand);
    
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
