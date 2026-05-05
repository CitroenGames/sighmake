import * as fs from 'fs';
import * as path from 'path';
import * as vscode from 'vscode';

const GENERATORS = ['vcxproj', 'cmake', 'makefile', 'buildscript'];

interface BuildCache {
    configurations: string[];
}

export function registerSighmakeCommands(context: vscode.ExtensionContext) {
    const runner = new SighmakeCommandRunner();

    context.subscriptions.push(
        vscode.commands.registerCommand('sighmake.generate', () => runner.generate(false)),
        vscode.commands.registerCommand('sighmake.generateDependencyReport', () => runner.generate(true)),
        vscode.commands.registerCommand('sighmake.build', () => runner.build({})),
        vscode.commands.registerCommand('sighmake.buildTarget', () => runner.build({ promptTarget: true })),
        vscode.commands.registerCommand('sighmake.rebuild', () => runner.build({ cleanFirst: true })),
        vscode.commands.registerCommand('sighmake.clean', () => runner.build({ cleanOnly: true })),
        vscode.commands.registerCommand('sighmake.convertSolution', () => runner.convertSolution()),
        vscode.commands.registerCommand('sighmake.convertVpc', () => runner.convertVpc()),
        vscode.commands.registerCommand('sighmake.listGenerators', () => runner.runInfoCommand(['--list'])),
        vscode.commands.registerCommand('sighmake.listToolsets', () => runner.runInfoCommand(['--list-toolsets']))
    );
}

class SighmakeCommandRunner {
    private get config(): vscode.WorkspaceConfiguration {
        return vscode.workspace.getConfiguration('sighmake');
    }

    async generate(forceExportDeps: boolean): Promise<void> {
        const input = await this.pickInputFile();
        if (!input) return;

        const cwd = this.getCommandCwd(input);
        const generator = await this.pickGenerator();
        if (!generator) return;

        const args = [input.fsPath, '-g', generator];

        if (generator === 'vcxproj') {
            const buildDir = this.config.get<string>('defaultBuildDir', 'build').trim();
            if (buildDir.length > 0) {
                args.push('-B', buildDir);
            }
        }

        const toolset = this.config.get<string>('defaultToolset', '').trim();
        if (toolset.length > 0) {
            args.push('-t', toolset);
        }

        this.addDefines(args);

        if (forceExportDeps || this.config.get<boolean>('exportDeps', false)) {
            args.push('--export-deps');
        }

        this.runSighmake(args, cwd);
    }

    async build(options: { cleanFirst?: boolean; cleanOnly?: boolean; promptTarget?: boolean }): Promise<void> {
        const cwd = await this.pickWorkspaceRoot();
        if (!cwd) return;

        const buildRoot = this.config.get<string>('buildRoot', '.').trim() || '.';
        const cacheRoot = path.resolve(cwd, buildRoot);
        const cache = this.readBuildCache(cacheRoot);

        const config = await this.pickConfiguration(cache);
        if (!config) return;

        const args = ['--build', buildRoot, '--config', config];

        if (options.promptTarget) {
            const target = await vscode.window.showInputBox({
                prompt: 'Build target',
                placeHolder: 'Leave empty to build the default target'
            });
            if (target === undefined) return;
            if (target.trim().length > 0) {
                args.push('--target', target.trim());
            }
        }

        if (options.cleanFirst) {
            args.push('--clean-first');
        }
        if (options.cleanOnly) {
            args.push('--clean');
        }

        const parallel = this.config.get<number>('defaultParallelJobs', 0);
        if (parallel > 0) {
            args.push('--parallel', String(parallel));
        }

        this.runSighmake(args, cwd);
    }

    async convertSolution(): Promise<void> {
        const files = await vscode.window.showOpenDialog({
            canSelectFiles: true,
            canSelectFolders: false,
            canSelectMany: false,
            filters: {
                'Visual Studio solutions': ['sln', 'slnx']
            },
            openLabel: 'Convert'
        });

        const file = files?.[0];
        if (!file) return;

        const args = ['--convert', file.fsPath];
        if (this.config.get<boolean>('exportDeps', false)) {
            args.push('--export-deps');
        }

        this.runSighmake(args, path.dirname(file.fsPath));
    }

    async convertVpc(): Promise<void> {
        const files = await vscode.window.showOpenDialog({
            canSelectFiles: true,
            canSelectFolders: false,
            canSelectMany: false,
            filters: {
                'Valve VPC files': ['vpc']
            },
            openLabel: 'Convert'
        });

        const file = files?.[0];
        if (!file) return;

        this.runSighmake(['convert', 'vpc', file.fsPath], path.dirname(file.fsPath));
    }

    async runInfoCommand(args: string[]): Promise<void> {
        const cwd = await this.pickWorkspaceRoot();
        if (!cwd) return;
        this.runSighmake(args, cwd);
    }

    private async pickInputFile(): Promise<vscode.Uri | undefined> {
        const active = vscode.window.activeTextEditor?.document;
        if (active?.uri.scheme === 'file' && this.isSupportedInput(active.uri)) {
            return active.uri;
        }

        const files = await vscode.window.showOpenDialog({
            canSelectFiles: true,
            canSelectFolders: false,
            canSelectMany: false,
            filters: {
                'sighmake inputs': ['buildscript', 'cmake', 'txt'],
                'All files': ['*']
            },
            openLabel: 'Generate'
        });

        return files?.[0];
    }

    private isSupportedInput(uri: vscode.Uri): boolean {
        const basename = path.basename(uri.fsPath).toLowerCase();
        const ext = path.extname(uri.fsPath).toLowerCase();
        return ext === '.buildscript' || ext === '.cmake' || basename === 'cmakelists.txt';
    }

    private getCommandCwd(input: vscode.Uri): string {
        const workspaceFolder = vscode.workspace.getWorkspaceFolder(input);
        return workspaceFolder?.uri.fsPath ?? path.dirname(input.fsPath);
    }

    private async pickWorkspaceRoot(): Promise<string | undefined> {
        const active = vscode.window.activeTextEditor?.document.uri;
        if (active?.scheme === 'file') {
            const activeFolder = vscode.workspace.getWorkspaceFolder(active);
            if (activeFolder) {
                return activeFolder.uri.fsPath;
            }
        }

        const folders = vscode.workspace.workspaceFolders ?? [];
        if (folders.length === 1) {
            return folders[0].uri.fsPath;
        }

        if (folders.length > 1) {
            const picked = await vscode.window.showWorkspaceFolderPick({
                placeHolder: 'Workspace folder to run sighmake in'
            });
            return picked?.uri.fsPath;
        }

        if (active?.scheme === 'file') {
            return path.dirname(active.fsPath);
        }

        vscode.window.showErrorMessage('Open a workspace or a file before running sighmake.');
        return undefined;
    }

    private async pickGenerator(): Promise<string | undefined> {
        const configuredDefault = this.config.get<string>('defaultGenerator', 'vcxproj');
        const items = GENERATORS.map(label => ({
            label,
            description: label === configuredDefault ? 'default' : undefined
        }));

        const picked = await vscode.window.showQuickPick(items, {
            placeHolder: 'Generator'
        });

        return picked?.label;
    }

    private async pickConfiguration(cache: BuildCache | undefined): Promise<string | undefined> {
        const configuredDefault = this.config.get<string>('defaultConfig', 'Debug').trim() || 'Debug';
        const configs = this.unique([...(cache?.configurations ?? []), configuredDefault, 'Debug', 'Release']);
        const items = configs.map(label => ({
            label,
            description: label === configuredDefault ? 'default' : undefined
        }));

        const picked = await vscode.window.showQuickPick(items, {
            placeHolder: 'Build configuration'
        });

        return picked?.label;
    }

    private readBuildCache(dir: string): BuildCache | undefined {
        const cachePath = path.join(dir, '.sighmake_cache');
        if (!fs.existsSync(cachePath)) {
            return undefined;
        }

        const cache: BuildCache = { configurations: [] };
        const content = fs.readFileSync(cachePath, 'utf8');

        for (const line of content.split(/\r?\n/)) {
            if (line.length === 0 || line.startsWith('#')) continue;
            const eq = line.indexOf('=');
            if (eq === -1) continue;

            const key = line.slice(0, eq);
            const value = line.slice(eq + 1);
            if (key === 'configurations') {
                cache.configurations = value.split(',').map(v => v.trim()).filter(v => v.length > 0);
            }
        }

        return cache;
    }

    private addDefines(args: string[]): void {
        const defines = this.config.get<Record<string, string>>('defines', {});
        for (const [name, value] of Object.entries(defines)) {
            if (name.trim().length === 0 || value === undefined) continue;
            args.push('-D', `${name}=${String(value)}`);
        }
    }

    private runSighmake(args: string[], cwd: string): void {
        const terminalName = this.config.get<string>('terminalName', 'sighmake');
        const terminal = vscode.window.createTerminal({ name: terminalName, cwd });
        terminal.show(true);
        terminal.sendText(this.formatCommand(args));
    }

    private formatCommand(args: string[]): string {
        const exe = this.config.get<string>('executablePath', 'sighmake').trim() || 'sighmake';
        const quoted = [exe, ...args].map(arg => this.quoteArg(arg));

        if (process.platform === 'win32') {
            return `& ${quoted.join(' ')}`;
        }

        return quoted.join(' ');
    }

    private quoteArg(value: string): string {
        if (process.platform === 'win32') {
            return `'${value.replace(/'/g, "''")}'`;
        }

        return `'${value.replace(/'/g, "'\\''")}'`;
    }

    private unique(values: string[]): string[] {
        const seen = new Set<string>();
        const result: string[] = [];

        for (const value of values) {
            if (seen.has(value)) continue;
            seen.add(value);
            result.push(value);
        }

        return result;
    }
}
