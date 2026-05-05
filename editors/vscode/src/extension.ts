import * as vscode from 'vscode';
import { BuildscriptCompletionProvider } from './completionProvider';
import { BuildscriptHoverProvider } from './hoverProvider';
import { registerSighmakeCommands } from './commandRunner';

export function activate(context: vscode.ExtensionContext) {
    const selector: vscode.DocumentSelector = { language: 'buildscript', scheme: 'file' };

    // Register completion provider
    const completionProvider = new BuildscriptCompletionProvider();
    context.subscriptions.push(
        vscode.languages.registerCompletionItemProvider(
            selector,
            completionProvider,
            '=', '['
        )
    );

    // Register hover provider
    const hoverProvider = new BuildscriptHoverProvider();
    context.subscriptions.push(
        vscode.languages.registerHoverProvider(selector, hoverProvider)
    );

    registerSighmakeCommands(context);
}

export function deactivate() {}
