// How the server's answer to `artic.projectForFile` reads in the UI.
//
// Kept free of the `vscode` module so it can be unit tested: the wording is the whole point
// of the feature, and a status bar that silently says the wrong thing is worse than none.

export type Provenance = 'config' | 'default-project' | 'single-file';

export interface ProjectForFile {
    file: string;
    provenance: Provenance;
    /** Empty for a single-file compile. */
    name: string;
    /** Configuration file that declared the project. Empty for a single-file compile. */
    origin: string;
    /** How many files are compiled together, this one included. */
    fileCount: number;
}

const files = (n: number) => `${n} file${n === 1 ? '' : 's'}`;

/** Text of the status bar entry. Short enough not to crowd out everything else. */
export function statusBarText(info: ProjectForFile | undefined): string {
    if (!info) return '$(question) Artic';
    if (info.provenance === 'single-file') return '$(warning) Artic: single file';
    return `$(file-submodule) ${info.name}`;
}

/** Hover text of the status bar entry, and the body of the detail message. */
export function statusBarTooltip(info: ProjectForFile | undefined): string {
    if (!info) return 'The Artic language server has not reported a project for this file yet.';

    switch (info.provenance) {
        case 'config':
            return [
                `Artic project "${info.name}", ${files(info.fileCount)} compiled together.`,
                `Declared in ${info.origin}.`,
            ].join('\n');
        case 'default-project':
            return [
                `Artic default project "${info.name}", ${files(info.fileCount)} compiled together.`,
                `This file is not listed in ${info.origin}, so it is compiled on its own`,
                'alongside whatever the default project depends on.',
            ].join('\n');
        case 'single-file':
            return [
                'No artic.json or .artic-lsp was found above this file, so it is compiled on',
                'its own. Anything it expects from another file is reported as an unknown',
                'identifier. Add a configuration file, or run "Artic: Detect workspace',
                'configuration" if the project is built with CMake or MSBuild.',
            ].join('\n');
    }
}

/** Whether the entry should stand out. A single-file compile is usually not what was wanted. */
export function isFallback(info: ProjectForFile | undefined): boolean {
    return !info || info.provenance === 'single-file';
}
