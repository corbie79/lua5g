// Dala VSCode Extension - LSP Client
const vscode = require('vscode');
const { LanguageClient, TransportKind } = require('vscode-languageclient/node');
const path = require('path');

let client;

function activate(context) {
  const config = vscode.workspace.getConfiguration('dala');
  const luaPath = config.get('luaPath', 'dala');
  const lspEnabled = config.get('lspEnabled', true);

  if (!lspEnabled) return;

  // LSP server is a Lua script run by dala
  const serverScript = path.join(context.extensionPath, '..', 'tools', 'lsp', 'server.lua');

  const serverOptions = {
    run: { command: luaPath, args: [serverScript], transport: TransportKind.stdio },
    debug: { command: luaPath, args: [serverScript], transport: TransportKind.stdio }
  };

  const clientOptions = {
    documentSelector: [
      { scheme: 'file', language: 'dala' }
    ],
    synchronize: {
      fileEvents: vscode.workspace.createFileSystemWatcher('**/*.{lua,dala,d.lua}')
    }
  };

  client = new LanguageClient('dala', 'Dala Language Server', serverOptions, clientOptions);
  client.start();

  context.subscriptions.push(client);

  // Status bar
  const statusBar = vscode.window.createStatusBarItem(vscode.StatusBarAlignment.Right, 100);
  statusBar.text = '$(symbol-class) Dala';
  statusBar.tooltip = 'Dala Language Server';
  statusBar.show();
  context.subscriptions.push(statusBar);
}

function deactivate() {
  if (client) return client.stop();
}

module.exports = { activate, deactivate };
