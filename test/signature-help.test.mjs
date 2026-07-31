// Covers textDocument/signatureHelp: which signature is reported for a call site, how the
// active parameter tracks the cursor, and the cases where the server must answer null.

import { test, describe, before, after } from 'node:test';
import assert from 'node:assert/strict';

import { LspClient } from './lsp-client.mjs';
import { findServerBinary, stageFixture, locate } from './helpers.mjs';

describe('signature help', () => {
    let ws;
    let client;
    let uri;
    let text;
    let initResult;
    let version = 1;

    /** Asks at `offset` characters past the start of `needle`. */
    const helpAt = (source, needle, offset, occurrence = 1) => {
        const position = locate(source, needle, occurrence);
        position.character += offset;
        return client.request('textDocument/signatureHelp', { textDocument: { uri }, position });
    };

    const signatureAt = async (...args) => {
        const result = await helpAt(text, ...args);
        assert.ok(result, `expected signature help for ${JSON.stringify(args[0])}, got null`);
        assert.equal(result.signatures.length, 1);
        assert.equal(result.activeSignature, 0);
        return result.signatures[0];
    };

    /** The parameter substrings the client would highlight, resolved from their label spans. */
    const parameterTexts = (signature) =>
        (signature.parameters ?? []).map(({ label: [begin, end] }) => signature.label.slice(begin, end));

    before(async () => {
        ws = stageFixture('signature-help');
        client = new LspClient(findServerBinary(), { cwd: ws.dir });
        initResult = await client.initialize(ws.uri);

        uri = ws.fileUri('src', 'calls.art');
        text = ws.read('src', 'calls.art');
        client.openDocument(uri, text);
        await client.waitForDiagnostics(uri);
    });

    after(async () => {
        await client?.stop();
        ws?.cleanup();
    });

    test('the server advertises signatureHelpProvider', () => {
        const provider = initResult.capabilities.signatureHelpProvider;
        assert.ok(provider, 'signatureHelpProvider is missing from the capabilities');
        assert.deepEqual(provider.triggerCharacters, ['(', ',']);
        assert.deepEqual(provider.retriggerCharacters, [')']);
    });

    test('reports the callee signature and the parameter under the cursor', async () => {
        const first = await signatureAt('add(1, 2)', 4);
        assert.equal(first.label, 'fn add(a: i32, b: i32) -> i32');
        assert.equal(first.activeParameter, 0);

        const second = await signatureAt('add(1, 2)', 7);
        assert.equal(second.activeParameter, 1);
    });

    test('the parameter spans point at the parameters inside the label', async () => {
        const signature = await signatureAt('add(1, 2)', 4);
        assert.deepEqual(parameterTexts(signature), ['a: i32', 'b: i32']);
    });

    test('renders type parameters of a generic function', async () => {
        // The `[i32]` between the callee and the argument list must not hide the callee.
        const signature = await signatureAt('identity[i32](sum)', 14);
        assert.equal(signature.label, 'fn identity[T](value: T) -> T');
        assert.deepEqual(parameterTexts(signature), ['value: T']);
    });

    test('renders implicit parameters, which the caller never writes', async () => {
        const signature = await signatureAt('scaled(2)', 7);
        assert.equal(signature.label, 'fn scaled(factor: i32, implicit unit: i32) -> i32');
        assert.deepEqual(parameterTexts(signature), ['factor: i32', 'implicit unit: i32']);
    });

    test('renders an enum option constructor qualified by its enumeration', async () => {
        const signature = await signatureAt('Shape::Dot(Point', 11);
        assert.equal(signature.label, 'Shape::Dot(Point)');
        assert.deepEqual(parameterTexts(signature), ['Point']);
    });

    test('a function without parameters has a signature but no active parameter', async () => {
        const signature = await signatureAt('no_args()', 8, 2);
        assert.equal(signature.label, 'fn no_args() -> i32');
        assert.equal(signature.parameters, undefined);
        assert.equal(signature.activeParameter, undefined);
    });

    test('resolves the innermost call when calls are nested', async () => {
        const signature = await signatureAt('add(p.x, same + p.y)', 12);
        assert.equal(signature.label, 'fn add(a: i32, b: i32) -> i32');
        assert.equal(signature.activeParameter, 1);
    });

    test('answers null where there is nothing to call', async () => {
        assert.equal(await helpAt(text, 'let sum', 0), null);
        // A struct literal is braces, not an argument list.
        assert.equal(await helpAt(text, 'Point { x = big', 12), null);
    });

    test('answers a half-written call, which never reaches the AST', async () => {
        // The parser cannot build a CallExpr for this, so the AST offers no call site at all.
        const broken = text.replace('add(1, 2);', 'add(1, ');
        client.changeDocument(uri, broken, ++version);

        const result = await helpAt(broken, 'add(1, ', 7);
        assert.ok(result, 'expected signature help inside an unterminated call');
        assert.equal(result.signatures[0].label, 'fn add(a: i32, b: i32) -> i32');
        assert.equal(result.signatures[0].activeParameter, 1);
    });

    test('a surplus argument keeps the last parameter active', async () => {
        // An out-of-range index makes the client highlight the first parameter instead.
        const surplus = text.replace('add(1, 2)', 'add(1, 2, 3)');
        client.changeDocument(uri, surplus, ++version);

        const result = await helpAt(surplus, 'add(1, 2, 3)', 10);
        assert.ok(result);
        assert.equal(result.signatures[0].activeParameter, 1);
    });
});
