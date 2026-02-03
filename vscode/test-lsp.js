const { spawn } = require('child_process');
const path = require('path');

// Find the artic binary
const articPath = path.join(__dirname, '..', 'artic-lsp', 'build', 'bin', 'artic');

console.log('Starting LSP server:', articPath);
console.log('=====================================\n');

const server = spawn(articPath, ['--lsp'], {
    cwd: process.cwd()
});

let messageId = 0;
let buffer = '';

// Parse and display messages
server.stdout.on('data', (data) => {
    buffer += data.toString();
    
    // Try to parse LSP messages (Content-Length header + JSON body)
    while (true) {
        const headerMatch = buffer.match(/Content-Length: (\d+)\r?\n\r?\n/);
        if (!headerMatch) break;
        
        const contentLength = parseInt(headerMatch[1]);
        const headerEnd = headerMatch.index + headerMatch[0].length;
        
        if (buffer.length < headerEnd + contentLength) break; // Not enough data yet
        
        const messageContent = buffer.substring(headerEnd, headerEnd + contentLength);
        buffer = buffer.substring(headerEnd + contentLength);
        
        try {
            const message = JSON.parse(messageContent);
            console.log('\n<<< RECEIVED FROM SERVER <<<');
            console.log(JSON.stringify(message, null, 2));
            console.log('');
        } catch (e) {
            console.error('Failed to parse message:', messageContent);
        }
    }
    
    // Show any data that doesn't match LSP format (this is the problem!)
    const nonLspData = buffer.match(/^[^C][^\r\n]*/);
    if (nonLspData) {
        console.error('\n!!! NON-LSP DATA ON STDOUT (THIS IS THE BUG!) !!!');
        console.error(nonLspData[0]);
        console.error('!!!\n');
    }
});

server.stderr.on('data', (data) => {
    console.log('[STDERR]:', data.toString());
});

server.on('close', (code) => {
    console.log('\n=====================================');
    console.log('Server exited with code:', code);
    process.exit(code);
});

// Helper to send LSP messages
function sendMessage(message) {
    const content = JSON.stringify(message);
    const header = `Content-Length: ${Buffer.byteLength(content)}\r\n\r\n`;
    
    console.log('\n>>> SENDING TO SERVER >>>');
    console.log(JSON.stringify(message, null, 2));
    console.log('');
    
    server.stdin.write(header + content);
}

// Wait a bit for server to start, then send messages
setTimeout(() => {
    // 1. Initialize
    sendMessage({
        jsonrpc: '2.0',
        id: ++messageId,
        method: 'initialize',
        params: {
            processId: process.pid,
            clientInfo: { name: 'test-client', version: '1.0' },
            rootUri: `file://${process.cwd()}`,
            capabilities: {
                workspace: {
                    didChangeConfiguration: { dynamicRegistration: true }
                },
                textDocument: {
                    inlayHint: { dynamicRegistration: true }
                }
            }
        }
    });
    
    setTimeout(() => {
        // 2. Initialized notification
        sendMessage({
            jsonrpc: '2.0',
            method: 'initialized',
            params: {}
        });
        
        setTimeout(() => {
            // 3. workspace/didChangeConfiguration
            sendMessage({
                jsonrpc: '2.0',
                method: 'workspace/didChangeConfiguration',
                params: {
                    settings: {
                        artic: {
                            serverPath: '',
                            trace: {
                                server: 'verbose'
                            },
                            globalConfig: '/home/gruen/artic-global.json'
                        }
                    }
                }
            });
            
            setTimeout(() => {
                // 4. textDocument/inlayHint
                sendMessage({
                    jsonrpc: '2.0',
                    id: ++messageId,
                    method: 'textDocument/inlayHint',
                    params: {
                        textDocument: {
                            uri: 'file:///home/gruen/repos/anydsl/stincilla/backend_cpu.impala'
                        },
                        range: {
                            start: { line: 0, character: 0 },
                            end: { line: 28, character: 0 }
                        }
                    }
                });
                
                // Wait for responses then exit
                setTimeout(() => {
                    console.log('\n=====================================');
                    console.log('Test complete. Shutting down...');
                    sendMessage({
                        jsonrpc: '2.0',
                        id: ++messageId,
                        method: 'shutdown',
                        params: null
                    });
                    setTimeout(() => {
                        sendMessage({
                            jsonrpc: '2.0',
                            method: 'exit',
                            params: null
                        });
                    }, 500);
                }, 2000);
            }, 500);
        }, 500);
    }, 500);
}, 500);

// Handle Ctrl+C
process.on('SIGINT', () => {
    console.log('\nShutting down...');
    server.kill();
    process.exit(0);
});
