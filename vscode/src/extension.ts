import * as vscode from 'vscode';
import * as path from 'path';
import * as os from 'os';
import { LanguageClient, LanguageClientOptions, ServerOptions, TransportKind, State, Trace } from 'vscode-languageclient/node';
import { execSync } from 'child_process';
import { existsSync } from 'fs';
import { BuildFile, selectWorkspaceConfigFiles } from './detect';

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
                    // Build files can be used as configuration, so a rebuild that adds or
                    // removes sources must invalidate the workspace too.
                    vscode.workspace.createFileSystemWatcher('**/*.sln'),
                    vscode.workspace.createFileSystemWatcher('**/*.vcxproj'),
                    vscode.workspace.createFileSystemWatcher('**/build.ninja'),
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

function pathKey(fsPath: string): string {
    const resolved = path.resolve(fsPath);
    return process.platform === 'win32' ? resolved.toLowerCase() : resolved;
}

async function readTextFile(file: vscode.Uri): Promise<string | undefined> {
    try {
        return toUtf8(await vscode.workspace.fs.readFile(file));
    } catch (error) {
        console.warn(`Failed to inspect build file ${file.fsPath}:`, error);
        return undefined;
    }
}

async function findWorkspaceConfigCandidates(workspaceFolder: vscode.WorkspaceFolder): Promise<vscode.Uri[]> {
    const find = (pattern: string) => vscode.workspace.findFiles(
        new vscode.RelativePattern(workspaceFolder, pattern),
        workspaceConfigExcludeGlob,
    );
    const found = (await Promise.all([
        find('**/*.sln'), find('**/build.ninja'), find('**/*.vcxproj'),
    ])).flat();

    const byPath = new Map<string, vscode.Uri>();
    const buildFiles: BuildFile[] = [];
    for (const file of found) {
        const content = await readTextFile(file);
        if (content === undefined) continue;
        byPath.set(pathKey(file.fsPath), file);
        buildFiles.push({ fsPath: file.fsPath, content });
    }

    return selectWorkspaceConfigFiles(buildFiles)
        .map((fsPath) => byPath.get(pathKey(fsPath)))
        .filter((uri): uri is vscode.Uri => uri !== undefined);
}

async function updateWorkspaceConfigIncludes(workspaceFolder: vscode.WorkspaceFolder, buildFiles: vscode.Uri[]): Promise<{ created: boolean; added: number; configPath: string; }> {
    const configUri = vscode.Uri.joinPath(workspaceFolder.uri, 'artic.json');
    const configDir = path.dirname(configUri.fsPath);
    // Detected files live in a build directory, which does not exist on a fresh checkout.
    // Writing them as optional (trailing `?`) keeps the config valid until the build runs.
    const discoveredIncludes = buildFiles.map((file) => `${toPosixRelativePath(configDir, file.fsPath)}?`);

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
        ? includeValue as string[]
        : [];

    // An include the user already wrote as required must not be duplicated as optional.
    const alreadyIncluded = new Set(existingIncludes.map((value) => value.replace(/\?$/, '')));

    const mergedIncludes = [...existingIncludes];
    let added = 0;
    for (const includePath of discoveredIncludes) {
        if (!alreadyIncluded.has(includePath.slice(0, -1))) {
            alreadyIncluded.add(includePath.slice(0, -1));
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

async function detectWorkspaceConfiguration(): Promise<void> {
    const workspaceFolders = vscode.workspace.workspaceFolders;
    if (!workspaceFolders || workspaceFolders.length === 0) {
        vscode.window.showWarningMessage('Open a workspace folder before detecting the Artic configuration.');
        return;
    }

    await vscode.window.withProgress({
        location: vscode.ProgressLocation.Notification,
        title: 'Detecting Artic workspace configuration',
        cancellable: false,
    }, async (progress) => {
        const summaries: string[] = [];
        for (const workspaceFolder of workspaceFolders) {
            progress.report({ message: `Scanning ${workspaceFolder.name}` });
            const buildFiles = await findWorkspaceConfigCandidates(workspaceFolder);
            const result = await updateWorkspaceConfigIncludes(workspaceFolder, buildFiles);
            summaries.push(`${workspaceFolder.name}: ${buildFiles.length} build files, ${result.added} added${result.created ? ', created artic.json' : ''}`);
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

    const detectConfigCommand = vscode.commands.registerCommand('artic.detectWorkspaceConfiguration', async () => {
        try {
            await detectWorkspaceConfiguration();
        } catch (error: any) {
            vscode.window.showErrorMessage(`Failed to detect the workspace configuration: ${error.message}`);
            console.error('Detect workspace configuration error:', error);
        }
    });
    context.subscriptions.push(detectConfigCommand);
}

export function deactivate(): Thenable<void> | undefined {
    if (!client) {
        return undefined;
    }
    return client.stop();
}