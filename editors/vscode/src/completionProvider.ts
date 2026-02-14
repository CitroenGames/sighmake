import * as vscode from 'vscode';
import { KEYWORDS, FUNCTIONS, SECTION_TYPES, PLATFORM_CONDITIONS, VISIBILITY_KEYWORDS } from './keywordData';

export class BuildscriptCompletionProvider implements vscode.CompletionItemProvider {

    provideCompletionItems(
        document: vscode.TextDocument,
        position: vscode.Position,
        _token: vscode.CancellationToken,
        _context: vscode.CompletionContext
    ): vscode.CompletionItem[] {
        const lineText = document.lineAt(position).text;
        const textBeforeCursor = lineText.substring(0, position.character);

        // Context 1: Inside section header (after "[")
        if (this.isInSectionHeader(textBeforeCursor)) {
            return this.getSectionCompletions();
        }

        // Context 2: Inside if() condition
        if (this.isInIfCondition(textBeforeCursor)) {
            return this.getConditionCompletions();
        }

        // Context 3: Inside function argument list
        if (this.isInFunctionArgs(textBeforeCursor, document, position)) {
            return this.getFunctionArgCompletions(textBeforeCursor);
        }

        // Context 4: After "=" (value completion)
        if (this.isAfterEquals(textBeforeCursor)) {
            const keyName = this.extractKeyName(textBeforeCursor);
            return this.getValueCompletions(keyName);
        }

        // Context 5: At line start — offer keywords and functions
        const currentSection = this.findCurrentSection(document, position);
        return this.getKeywordCompletions(currentSection);
    }

    private isInSectionHeader(text: string): boolean {
        const trimmed = text.trimStart();
        return trimmed.startsWith('[') && !trimmed.includes(']');
    }

    private isInIfCondition(text: string): boolean {
        return /\bif\s*\(\s*!?\s*$/.test(text) || /\bif\s*\(\s*!?[a-zA-Z]*$/.test(text);
    }

    private isInFunctionArgs(text: string, document: vscode.TextDocument, position: vscode.Position): boolean {
        // Check current line and lines above for an unclosed function call
        const funcPattern = /\b(target_link_libraries|find_package|uses_pch|file_properties|set_file_properties)\s*\(/;
        for (let i = position.line; i >= Math.max(0, position.line - 20); i--) {
            const line = document.lineAt(i).text;
            const searchText = i === position.line ? text : line;
            if (funcPattern.test(searchText)) {
                // Check if the paren is closed before cursor
                const fullText = this.getTextFromLineToPosition(document, i, position);
                let depth = 0;
                for (const ch of fullText) {
                    if (ch === '(') depth++;
                    if (ch === ')') depth--;
                }
                return depth > 0;
            }
            // If we hit a section header, stop looking
            if (line.trimStart().startsWith('[')) break;
        }
        return false;
    }

    private getTextFromLineToPosition(document: vscode.TextDocument, startLine: number, position: vscode.Position): string {
        let text = '';
        for (let i = startLine; i <= position.line; i++) {
            if (i === position.line) {
                text += document.lineAt(i).text.substring(0, position.character);
            } else {
                text += document.lineAt(i).text + '\n';
            }
        }
        return text;
    }

    private isAfterEquals(text: string): boolean {
        // Check if we're past an "=" sign, accounting for config qualifiers
        const match = text.match(/[a-zA-Z_][a-zA-Z0-9_]*(?:\[[^\]]*\])?\s*=\s*/);
        return match !== null && text.indexOf('=') < text.length;
    }

    private extractKeyName(text: string): string {
        // Extract key name from patterns like:
        // "key = "
        // "key[Config|Platform] = "
        // "filepath.cpp:key = "
        // "filepath.cpp:key[Config] = "
        const match = text.match(/(?:[^:=\[\]]+:)?([a-zA-Z_][a-zA-Z0-9_]*)(?:\[[^\]]*\])?\s*=\s*[^=]*$/);
        return match ? match[1] : '';
    }

    private findCurrentSection(document: vscode.TextDocument, position: vscode.Position): string {
        for (let i = position.line; i >= 0; i--) {
            const line = document.lineAt(i).text.trim();
            if (/^\[solution\]/.test(line)) return 'solution';
            if (/^\[project:/.test(line)) return 'project';
            if (/^\[config:/.test(line)) return 'config';
            if (/^\[file:/.test(line)) return 'file';
        }
        return 'solution';
    }

    private getSectionCompletions(): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];

        const solution = new vscode.CompletionItem('solution', vscode.CompletionItemKind.Module);
        solution.insertText = new vscode.SnippetString('solution]\nname = ${1:MySolution}\nconfigurations = ${2:Debug, Release}\nplatforms = ${3:Win32, x64}\n');
        solution.documentation = new vscode.MarkdownString(SECTION_TYPES['solution']);
        items.push(solution);

        const project = new vscode.CompletionItem('project:', vscode.CompletionItemKind.Module);
        project.insertText = new vscode.SnippetString('project:${1:ProjectName}]\ntype = ${2|exe,lib,dll|}\nsources = ${3:src/**/*.cpp}\n');
        project.documentation = new vscode.MarkdownString(SECTION_TYPES['project']);
        items.push(project);

        const config = new vscode.CompletionItem('config:', vscode.CompletionItemKind.Module);
        config.insertText = new vscode.SnippetString('config:${1:Debug}|${2:Win32}]\n');
        config.documentation = new vscode.MarkdownString(SECTION_TYPES['config']);
        items.push(config);

        const file = new vscode.CompletionItem('file:', vscode.CompletionItemKind.Module);
        file.insertText = new vscode.SnippetString('file:${1:src/file.cpp}]\n');
        file.documentation = new vscode.MarkdownString(SECTION_TYPES['file']);
        items.push(file);

        return items;
    }

    private getConditionCompletions(): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];
        for (const [name, doc] of Object.entries(PLATFORM_CONDITIONS)) {
            const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Constant);
            item.documentation = new vscode.MarkdownString(doc);
            items.push(item);
        }
        return items;
    }

    private getFunctionArgCompletions(text: string): vscode.CompletionItem[] {
        // If inside target_link_libraries, offer visibility keywords
        if (/target_link_libraries/.test(text)) {
            return Object.entries(VISIBILITY_KEYWORDS).map(([name, doc]) => {
                const item = new vscode.CompletionItem(name, vscode.CompletionItemKind.Keyword);
                item.documentation = new vscode.MarkdownString(doc);
                return item;
            });
        }
        // If inside find_package, offer REQUIRED
        if (/find_package/.test(text)) {
            const packages = ['Vulkan', 'OpenGL', 'SDL2', 'SDL3', 'DirectX9', 'DirectX10', 'DirectX11', 'DirectX12'];
            const items = packages.map(pkg => {
                const item = new vscode.CompletionItem(pkg, vscode.CompletionItemKind.Module);
                item.documentation = new vscode.MarkdownString(`External package: ${pkg}`);
                return item;
            });
            const req = new vscode.CompletionItem('REQUIRED', vscode.CompletionItemKind.Keyword);
            req.documentation = new vscode.MarkdownString('Make the package required. An error is raised if not found.');
            items.push(req);
            return items;
        }
        return [];
    }

    private getValueCompletions(keyName: string): vscode.CompletionItem[] {
        if (!keyName) return [];

        const cleanKey = keyName.trim();

        // Find the keyword definition (check name and aliases)
        const keyword = KEYWORDS.find(kw =>
            kw.name === cleanKey || kw.aliases.includes(cleanKey)
        );

        if (!keyword) return [];

        if (keyword.validValues) {
            return keyword.validValues.map((val, idx) => {
                const item = new vscode.CompletionItem(val, vscode.CompletionItemKind.EnumMember);
                item.detail = `${keyword.name} value`;
                item.sortText = String(idx).padStart(3, '0');
                return item;
            });
        }

        if (keyword.valueType === 'bool') {
            return ['true', 'false'].map(val => {
                const item = new vscode.CompletionItem(val, vscode.CompletionItemKind.Value);
                return item;
            });
        }

        return [];
    }

    private getKeywordCompletions(section: string): vscode.CompletionItem[] {
        const items: vscode.CompletionItem[] = [];

        // Keywords
        const sectionKeywords = KEYWORDS.filter(kw =>
            kw.context.includes(section as 'solution' | 'project' | 'config' | 'file')
        );

        for (const kw of sectionKeywords) {
            const item = new vscode.CompletionItem(kw.name, vscode.CompletionItemKind.Property);
            item.insertText = new vscode.SnippetString(`${kw.name} = $0`);
            item.documentation = new vscode.MarkdownString(kw.documentation);
            if (kw.validValues) {
                item.detail = `Values: ${kw.validValues.join(', ')}`;
            } else {
                item.detail = kw.valueType;
            }
            items.push(item);

            // Aliases with lower sort priority
            for (const alias of kw.aliases) {
                const aliasItem = new vscode.CompletionItem(alias, vscode.CompletionItemKind.Property);
                aliasItem.insertText = new vscode.SnippetString(`${alias} = $0`);
                aliasItem.documentation = new vscode.MarkdownString(`Alias for \`${kw.name}\`.\n\n${kw.documentation}`);
                aliasItem.sortText = `zzz_${alias}`;
                aliasItem.detail = `alias for ${kw.name}`;
                items.push(aliasItem);
            }
        }

        // Functions (only in project context)
        if (section === 'project') {
            for (const func of FUNCTIONS) {
                const item = new vscode.CompletionItem(func.name, vscode.CompletionItemKind.Function);
                item.documentation = new vscode.MarkdownString(func.documentation);
                item.detail = func.signature;
                items.push(item);
            }
        }

        return items;
    }
}
