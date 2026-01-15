import * as vscode from 'vscode';
import * as path from 'path';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind, State } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync, writeFileSync } from 'fs';

let client: LanguageClient;
let expectedStop = false;

const workspaceConfigTemplate = `{
    "artic-config": "2.0",
    "projects": [
        {
            "name": "new project",
            "dependencies": [],
            "files": [
                "**/*.impala",
                "**/*.art"
            ]
        }
    ],
    "include": [
    ]
}`;

// Config > Bundled > PATH
function findArticBinary(): string {
    const config = vscode.workspace.getConfiguration('artic');
    let serverPath = config.get<string>('serverPath', '');
    if (serverPath && existsSync(serverPath)) {
        return serverPath;
    }

    const bundled = path.join(__dirname, '..', 'build', 'bin', 'artic');
    if (existsSync(bundled)) {
        return bundled;
    }

    try {
        execSync('which artic', { stdio: 'ignore' });
        return 'artic';
    } catch {
        // continue
    }

    throw new Error('Artic binary not found. Packaged binary missing and none found in settings or PATH.');
}

function startClient(context: vscode.ExtensionContext) {
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
        let restartFromCrash = false;
        expectedStop = false;

        // Client options
        const clientOptions: LanguageClientOptions = {
            documentSelector: [
                { scheme: 'file', language: 'artic' },
                { scheme: 'file', language: 'json', pattern: '**/artic.json' },
                { scheme: 'file', language: 'json', pattern: '**/.artic-lsp' }
            ],
            synchronize: {
                fileEvents: [
                    vscode.workspace.createFileSystemWatcher('**/*.art'),
                    vscode.workspace.createFileSystemWatcher('**/*.impala'),
                    vscode.workspace.createFileSystemWatcher('**/artic.json'),
                    vscode.workspace.createFileSystemWatcher('**/.artic-lsp'),
                ]
            },
            outputChannelName: 'Artic Language Server',
            traceOutputChannel: vscode.window.createOutputChannel('Artic Language Server Trace'),
            // Enable semantic tokens
            middleware: {
                provideDocumentSemanticTokens: (document, token, next) => {
                    return next(document, token);
                }
            },
            initializationOptions: () => {
                console.log('Artic Language Server started successfully');
                // client.outputChannel?.show(true);

                let hasCrashed = restartFromCrash;
                restartFromCrash = false;

                return {
                    restartFromCrash: hasCrashed
                };
            },
            connectionOptions: {
                maxRestartCount: 5,
            }
        };

        // Create the language client
        client = new LanguageClient(
            'articLanguageServer',
            'Artic Language Server',
            serverOptions,
            clientOptions,
        );
        client.onDidChangeState((event) => {
            if (expectedStop) {
                expectedStop = false;
                return;
            }
            if (event.oldState === State.Running && event.newState === State.Stopped) { // Running -> Starting
                restartFromCrash = true;
                vscode.window.showWarningMessage(
                    `Artic Language Server has crashed. Restarting in safe mode...`,
                    'Show Output'
                ).then(choice => {
                    if (choice === 'Show Output') {
                        client.outputChannel?.show();
                    }
                });
            }
        });

        // Start the client (which also starts the server)
        client.start();
    } catch (error: any) {
        console.error('Failed to start Artic Language Server:', error);
        vscode.window.showErrorMessage(`Failed to start Artic Language Server: ${error.message}`);
    }
}

export function activate(context: vscode.ExtensionContext) {
    startClient(context);

    // Register commands
    const restartCommand = vscode.commands.registerCommand('artic.restart', async () => {
        if (client) {
            if(client.isRunning()){
                expectedStop = true;
                await client.restart();
            } else {
                client.start();
            }
        } else {
            startClient(context);
        }
        
    });
    context.subscriptions.push(restartCommand);

    const reloadConfigCommand = vscode.commands.registerCommand('artic.reloadConfig', async () => {
        try {
            if (!client) return;
            await client.sendNotification('workspace/didChangeConfiguration', { settings: {} });
        } catch (e: any) {
            vscode.window.showErrorMessage(`Failed to send reload: ${e.message}`);
        }
    });
    context.subscriptions.push(reloadConfigCommand);

    const debugAstCommand = vscode.commands.registerCommand('artic.debugAst', async () => {
        try {
            if (!client || !client.isRunning()) {
                vscode.window.showWarningMessage('Artic Language Server is not running');
                return;
            }

            const editor = vscode.window.activeTextEditor;
            if (!editor) {
                vscode.window.showWarningMessage('No active editor');
                return;
            }

            const document = editor.document;
            const position = editor.selection.active;

            // Send custom request to the language server
            const params = {
                textDocument: { uri: document.uri.toString() },
                position: { line: position.line, character: position.character }
            };

            const result = await client.sendRequest('artic/debugAst', params);
            if(result === null || result === undefined){
                vscode.window.showInformationMessage('No AST node found at cursor position.');
                return;
            }
            
            // Show the AST in a new document
            const astDoc = await vscode.workspace.openTextDocument({
                content: result as string,
                language: 'plaintext'
            });
            await vscode.window.showTextDocument(astDoc, vscode.ViewColumn.Beside);
        } catch (e: any) {
            vscode.window.showErrorMessage(`Failed to get AST: ${e.message}`);
            console.error('Debug AST error:', e);
        }
    });
    context.subscriptions.push(debugAstCommand);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}