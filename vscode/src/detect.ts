import * as path from 'path';

/** A build file that was found in the workspace, together with its contents. */
export interface BuildFile {
    fsPath: string;
    content: string;
}

function pathKey(fsPath: string): string {
    const resolved = path.resolve(fsPath);
    return process.platform === 'win32' ? resolved.toLowerCase() : resolved;
}

function mentionsArtic(content: string): boolean {
    return content.toLowerCase().includes('artic');
}

/** Absolute paths of the .vcxproj files a solution lists. Solution folders reuse the same syntax. */
export function solutionProjectPaths(solutionPath: string, content: string): string[] {
    const dir = path.dirname(solutionPath);
    const entry = /^Project\("\{[^"}]*\}"\)\s*=\s*"[^"]*"\s*,\s*"([^"]*)"/gm;
    const paths: string[] = [];
    for (let match = entry.exec(content); match; match = entry.exec(content)) {
        if (path.extname(match[1]).toLowerCase() === '.vcxproj') {
            paths.push(pathKey(path.resolve(dir, match[1])));
        }
    }
    return paths;
}

/**
 * Picks the build files that should be written into `artic.json`, strongest match first.
 * A solution supersedes the projects it lists and a ninja file supersedes the projects
 * next to it; including both would define every project twice.
 */
export function selectWorkspaceConfigFiles(files: BuildFile[]): string[] {
    const byKind = (predicate: (file: BuildFile) => boolean) =>
        files.filter(predicate).sort((left, right) => left.fsPath.localeCompare(right.fsPath));

    const extensionIs = (ext: string) => (file: BuildFile) =>
        path.extname(file.fsPath).toLowerCase() === ext;

    const solutions = byKind(extensionIs('.sln'));
    const ninjaFiles = byKind((file) => path.basename(file.fsPath).toLowerCase() === 'build.ninja');
    const projectFiles = byKind(extensionIs('.vcxproj'));

    // A .sln contains nothing but project names and GUIDs, so it never mentions artic
    // itself; it qualifies when one of the projects it lists does.
    const articProjects = new Set(
        projectFiles.filter((file) => mentionsArtic(file.content)).map((file) => pathKey(file.fsPath)),
    );

    const kept: string[] = [];
    const coveredDirs: string[] = [];
    const isCovered = (dir: string) =>
        coveredDirs.some((covered) => dir === covered || dir.startsWith(covered + path.sep));

    for (const file of solutions) {
        if (!solutionProjectPaths(file.fsPath, file.content).some((p) => articProjects.has(p))) continue;
        kept.push(file.fsPath);
        coveredDirs.push(path.dirname(file.fsPath));
    }

    for (const file of ninjaFiles) {
        if (isCovered(path.dirname(file.fsPath)) || !mentionsArtic(file.content)) continue;
        kept.push(file.fsPath);
        coveredDirs.push(path.dirname(file.fsPath));
    }

    for (const file of projectFiles) {
        if (!articProjects.has(pathKey(file.fsPath))) continue;
        if (isCovered(path.dirname(file.fsPath))) continue;
        kept.push(file.fsPath);
    }

    return kept.sort((left, right) => left.localeCompare(right));
}
