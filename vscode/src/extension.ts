import * as vscode from 'vscode';
import * as path from 'path';
import * as os from 'os';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind, State, Trace } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync } from 'fs';

let client: LanguageClient | undefined = undefined;
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

const workspaceConfigExcludeGlob = '**/{.git,.vs,node_modules,out,dist}/**';

// Config > Bundled > PATH
function findArticBinary(): string {
    const config = vscode.workspace.getConfiguration('artic');
    let serverPath = config.get<string>('serverPath', '');
    if (serverPath && existsSync(serverPath)) {
        return serverPath;
    }

    const bundled = os.platform() ==='win32' 
                        ? path.join(__dirname, '..', 'build', 'bin', 'artic-lsp.exe')
                        : path.join(__dirname, '..', 'build', 'bin', 'artic-lsp');
    if (existsSync(bundled)) {
        return bundled;
    }

    try {
        execSync('which artic-lsp', { stdio: 'ignore' });
        return 'artic-lsp';
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
                ],
                configurationSection: 'artic'
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
                maxRestartCount: 11,
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
                // vscode.window.showWarningMessage(
                //     `Artic Language Server has crashed. Restarting in safe mode...`,
                //     'Show Output'
                // ).then(choice => {
                //     if (choice === 'Show Output') {
                //         client.outputChannel?.show();
                //     }
                // });
            }
        });

        // Start the client (which also starts the server)
        client.start().then(() => {
            // Set trace after client has started
            client?.setTrace(Trace.Verbose);
        });
    } catch (error: any) {
        console.error('Failed to start Artic Language Server:', error);
        vscode.window.showErrorMessage(`Failed to start Artic Language Server: ${error.message}`);
    }
}

function toUtf8(bytes: Uint8Array): string {
    return Buffer.from(bytes).toString('utf8');
}

function toPosixRelativePath(fromDir: string, toFile: string): string {
    return path.relative(fromDir, toFile).replace(/\\/g, '/');
}

async function findArticVcxprojFiles(workspaceFolder: vscode.WorkspaceFolder): Promise<vscode.Uri[]> {
    const vcxprojFiles = await vscode.workspace.findFiles(
        new vscode.RelativePattern(workspaceFolder, '**/*.vcxproj'),
        workspaceConfigExcludeGlob,
    );

    const matches: vscode.Uri[] = [];
    for (const file of vcxprojFiles) {
        try {
            const content = toUtf8(await vscode.workspace.fs.readFile(file));
            if (content.toLowerCase().includes('artic')) {
                matches.push(file);
            }
        } catch (error) {
            console.warn(`Failed to inspect vcxproj file ${file.fsPath}:`, error);
        }
    }

    matches.sort((left, right) => left.fsPath.localeCompare(right.fsPath));
    return matches;
}

async function updateWorkspaceConfigIncludes(workspaceFolder: vscode.WorkspaceFolder, vcxprojFiles: vscode.Uri[]): Promise<{ created: boolean; added: number; configPath: string; }> {
    const configUri = vscode.Uri.joinPath(workspaceFolder.uri, 'artic.json');
    const configDir = path.dirname(configUri.fsPath);
    const discoveredIncludes = vcxprojFiles.map((file) => toPosixRelativePath(configDir, file.fsPath));

    let created = false;
    let config: Record<string, unknown>;
    try {
        const existing = toUtf8(await vscode.workspace.fs.readFile(configUri));
        config = JSON.parse(existing) as Record<string, unknown>;
    } catch (error) {
        if (!(error instanceof vscode.FileSystemError) || error.code !== 'FileNotFound') {
            throw new Error(`Failed to read ${configUri.fsPath}: ${(error as Error).message}`);
        }
        created = true;
        config = {
            'artic-config': '2.0',
            include: []
        };
    }

    const includeValue = config.include;
    if (includeValue !== undefined && !Array.isArray(includeValue)) {
        throw new Error(`'include' in ${configUri.fsPath} must be an array`);
    }
    if (Array.isArray(includeValue) && includeValue.some((value) => typeof value !== 'string')) {
        throw new Error(`'include' in ${configUri.fsPath} must only contain strings`);
    }

    const existingIncludes = Array.isArray(includeValue)
        ? includeValue
        : [];

    const mergedIncludes = [...existingIncludes];
    let added = 0;
    for (const includePath of discoveredIncludes) {
        if (!mergedIncludes.includes(includePath)) {
            mergedIncludes.push(includePath);
            added += 1;
        }
    }

    config['artic-config'] = typeof config['artic-config'] === 'string' ? config['artic-config'] : '2.0';
    config.include = mergedIncludes;

    const serialized = `${JSON.stringify(config, null, 4)}\n`;
    await vscode.workspace.fs.writeFile(configUri, Buffer.from(serialized, 'utf8'));

    return {
        created,
        added,
        configPath: configUri.fsPath,
    };
}

async function discoverVcxprojConfigs(): Promise<void> {
    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (!workspaceFolders || workspaceFolders.length === 0) {
        vscode.window.showWarningMessage('Open a workspace folder before discovering vcxproj files.');
        return;
    }

    await vscode.window.withProgress({
        location: vscode.ProgressLocation.Notification,
        title: 'Discovering Artic vcxproj files',
        cancellable: false,
    }, async (progress) => {
        const summaries: string[] = [];
        for (const workspaceFolder of workspaceFolders) {
            progress.report({ message: `Scanning ${workspaceFolder.name}` });
            const vcxprojFiles = await findArticVcxprojFiles(workspaceFolder);
            const result = await updateWorkspaceConfigIncludes(workspaceFolder, vcxprojFiles);
            summaries.push(`${workspaceFolder.name}: ${vcxprojFiles.length} matches, ${result.added} added${result.created ? ', created artic.json' : ''}`);
        }

        vscode.window.showInformationMessage(summaries.join(' | '));
    });
}

export function activate(context: vscode.ExtensionContext) {
    startClient(context);

    // Register commands
    const restartCommand = vscode.commands.registerCommand('artic.restart', async () => {
        if (client) {
            await client.stop().catch(_ => {});
            client = undefined;
        } 
        startClient(context);
    });
    context.subscriptions.push(restartCommand);

    const discoverVcxprojCommand = vscode.commands.registerCommand('artic.discoverVcxprojConfigs', async () => {
        try {
            await discoverVcxprojConfigs();
        } catch (error: any) {
            vscode.window.showErrorMessage(`Failed to discover vcxproj files: ${error.message}`);
            console.error('Discover vcxproj configs error:', error);
        }
    });
    context.subscriptions.push(discoverVcxprojCommand);

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