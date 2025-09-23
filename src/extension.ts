import * as vscode from 'vscode';
import * as path from 'path';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync, writeFileSync } from 'fs';

let client: LanguageClient;

const globalConfigTemplate = `{
    "artic-config": "1.0",
    "default-project": {
        "name": "<unknown project>",
        "dependencies": [
            "anydsl.runtime"
        ],
        "files": []
    },
    "projects": [],
    "include": [
        "repos/anydsl/runtime/artic.json?"
    ]
}`;

const workspaceConfigTemplate = `{
    "artic-config": "1.0",
    "projects": [
        {
            "name": "new project",
            "dependencies": [
                "anydsl.runtime"
            ],
            "files": [
                "**/*.impala",
                "**/*.art"
            ]
        }
    ],
    "include": [
        "<global>",
        "../anydsl/runtime/artic.json?"
    ]
}`;


function getWorkspaceConfigPath(): string | undefined {
    const folders = vscode.workspace.workspaceFolders;
    if (!folders || folders.length === 0) return undefined;
    const p = path.join(folders[0].uri.fsPath, 'artic.json');
    return p;
}

function getGlobalConfigPath(): string | undefined {
    const cfg = vscode.workspace.getConfiguration('artic');
    const p = cfg.get<string>('globalConfig', '').trim();
    if (!p) return undefined;
    return p;
}

// Config > Bundled > PATH
function findArticBinary(): string {
    const config = vscode.workspace.getConfiguration('artic');
    let serverPath = config.get<string>('serverPath', '');
    if (serverPath && existsSync(serverPath)) {
        return serverPath;
    }

    const bundled = path.join(__dirname, '..', 'artic', 'build', 'bin', 'artic');
    if (existsSync(bundled)) {
        return bundled;
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
            documentSelector: [ 
                { scheme: 'file', language: 'artic' },
                { scheme: 'file', language: 'json', pattern: '**/{artic,artic-global}.json' }
             ],
            synchronize: {
                fileEvents: [
                    vscode.workspace.createFileSystemWatcher('**/*.art'),
                    vscode.workspace.createFileSystemWatcher('**/*.impala'),
                    vscode.workspace.createFileSystemWatcher('**/artic.json'),
                ]
            },
            outputChannelName: 'Artic Language Server',
            traceOutputChannel: vscode.window.createOutputChannel('Artic Language Server Trace')
        };

        const workspaceConfigPath = getWorkspaceConfigPath();
        const globalConfigPath = getGlobalConfigPath();

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

            const ensureConfig = async (config: Config) => {
                if (config.path && existsSync(config.path)) return; // already exists
                if (!config.defaultPath) return;
                const choice = await vscode.window.showInformationMessage(config.detailLabel, config.createLabel, 'Dismiss');
                if (choice === config.createLabel) {
                    try {
                        if (!existsSync(config.defaultPath)) writeFileSync(config.defaultPath, config.template, { flag: 'wx' });
                        if (config.isGlobalConfig) {
                            const cfg = vscode.workspace.getConfiguration('artic');
                            cfg.update('globalConfig', config.defaultPath, vscode.ConfigurationTarget.Global);
                        }
                        const doc = await vscode.workspace.openTextDocument(config.defaultPath);
                        await vscode.window.showTextDocument(doc);
                        vscode.window.showInformationMessage(`Created config at ${config.defaultPath}, please restart the language server`);
                        //TODO reload
                    } catch (e: any) {
                        vscode.window.showErrorMessage(`Failed to create artic config: ${e.message}`);
                    }
                }
            }
            type Config = {
                defaultPath: string | undefined
                path: string | undefined
                createLabel: string
                detailLabel: string
                template: string
                isGlobalConfig: boolean
            }
            const workspaceConfig: Config = {
                defaultPath: workspaceRoot ? path.join(workspaceRoot, 'artic.json') : undefined,
                path: client.clientOptions.initializationOptions.workspaceConfig,
                createLabel: 'Create workspace artic.json',
                detailLabel: 'Create an artic.json in the workspace root so projects can be configured.',
                template: workspaceConfigTemplate,
                isGlobalConfig: false
            }
            const globalConfig: Config = {
                defaultPath: path.join(process.env.HOME || '', 'artic-global.json'),
                path: client.clientOptions.initializationOptions.globalConfig,
                createLabel: 'Create global artic-global.json',
                detailLabel: 'Create a global artic-global.json in your home directory for shared projects.',
                template: globalConfigTemplate,
                isGlobalConfig: true
            }

            await ensureConfig(workspaceConfig);
            await ensureConfig(globalConfig);
        }).catch(error => {
            console.error('Failed to start Artic Language Server:', error);
            vscode.window.showErrorMessage(`Failed to start Artic Language Server: ${error.message}`);
        });
        // Register commands
        const restartCommand = vscode.commands.registerCommand('artic.restart', async () => {
            if (client) {
                client.clientOptions.initializationOptions = {
                    workspaceConfig: getWorkspaceConfigPath(),
                    globalConfig: getGlobalConfigPath()
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
