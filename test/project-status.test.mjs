// What the status bar tells the user about the project a file belongs to.
//
// The whole point of the feature is the wording: a status bar that says "single file"
// when the file really is in a project, or stays quiet when it is not, is worse than none.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { importExtensionModule } from './helpers.mjs';

const inProject = {
    file: '/ws/src/main.art',
    provenance: 'config',
    name: 'my_project',
    origin: '/ws/artic.json',
    fileCount: 4,
};

const inDefaultProject = {
    file: '/ws/scratch/try.art',
    provenance: 'default-project',
    name: 'default project',
    origin: '/ws/artic.json',
    fileCount: 2,
};

const alone = {
    file: '/elsewhere/lonely.art',
    provenance: 'single-file',
    name: '',
    origin: '',
    fileCount: 1,
};

const detected = {
    file: '/ws/src/main.art',
    provenance: 'detected',
    name: 'demo',
    origin: '/ws/build/build.ninja',
    fileCount: 2,
};

describe('project status presentation', () => {
    let statusBarText, statusBarTooltip, isFallback, cleanup;

    before(async () => {
        const bundle = await importExtensionModule('project-status.ts');
        ({ statusBarText, statusBarTooltip, isFallback } = bundle.module);
        cleanup = bundle.cleanup;
    });
    after(() => cleanup?.());

    test('a configured project shows its name and where it came from', () => {
        assert.match(statusBarText(inProject), /my_project/);
        const tooltip = statusBarTooltip(inProject);
        assert.match(tooltip, /my_project/);
        assert.match(tooltip, /4 files/);
        assert.match(tooltip, /\/ws\/artic\.json/);
        assert.equal(isFallback(inProject), false);
    });

    test('the default project says the file is not listed', () => {
        assert.match(statusBarText(inDefaultProject), /default project/);
        assert.match(statusBarTooltip(inDefaultProject), /not listed/);
        assert.equal(isFallback(inDefaultProject), false);
    });

    test('a single-file compile is called out, and says what to do about it', () => {
        assert.match(statusBarText(alone), /single file/);
        const tooltip = statusBarTooltip(alone);
        assert.match(tooltip, /artic\.json/);
        assert.match(tooltip, /unknown\s+identifier/);
        assert.match(tooltip, /build file/);
        assert.equal(isFallback(alone), true);
    });

    test('a detected build file says so, so it is not mistaken for a config', () => {
        assert.match(statusBarText(detected), /demo/);
        const tooltip = statusBarTooltip(detected);
        assert.match(tooltip, /Detected/);
        assert.match(tooltip, /build\/build\.ninja/);
        assert.equal(isFallback(detected), false);
    });

    test('no answer yet is a fallback too, and never shows a bare project name', () => {
        assert.equal(isFallback(undefined), true);
        assert.doesNotMatch(statusBarText(undefined), /single file/);
        assert.match(statusBarTooltip(undefined), /not reported/);
    });

    test('one file is not pluralised', () => {
        assert.match(statusBarTooltip({ ...inProject, fileCount: 1 }), /1 file[^s]/);
    });
});
