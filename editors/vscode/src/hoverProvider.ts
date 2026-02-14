import * as vscode from 'vscode';
import {
    KEYWORDS, FUNCTIONS,
    SECTION_TYPES, PLATFORM_CONDITIONS, VISIBILITY_KEYWORDS,
    KeywordInfo
} from './keywordData';

export class BuildscriptHoverProvider implements vscode.HoverProvider {

    provideHover(
        document: vscode.TextDocument,
        position: vscode.Position,
        _token: vscode.CancellationToken
    ): vscode.Hover | undefined {
        const wordRange = document.getWordRangeAtPosition(position, /[a-zA-Z_][a-zA-Z0-9_]*/);
        if (!wordRange) return undefined;

        const word = document.getText(wordRange);
        const lineText = document.lineAt(position).text;

        // Check if hovering inside a section header
        if (this.isInSectionHeader(lineText, position.character)) {
            return this.getSectionHover(word);
        }

        // Check if hovering inside an if() condition
        if (this.isInIfCondition(lineText, position.character)) {
            return this.getPlatformHover(word);
        }

        // Determine if word is on key side or value side of "="
        const eqIndex = lineText.indexOf('=');
        const isKeywordSide = eqIndex === -1 || wordRange.end.character <= eqIndex;

        if (isKeywordSide) {
            // Check keywords
            const keyword = this.findKeyword(word);
            if (keyword) {
                return this.createKeywordHover(keyword, word);
            }

            // Check functions
            const func = FUNCTIONS.find(f => f.name === word);
            if (func) {
                return this.createFunctionHover(func);
            }

            // Check section type keywords
            if (word in SECTION_TYPES) {
                return this.createSectionTypeHover(word);
            }
        }

        // Value side
        if (!isKeywordSide) {
            // Check known enum values
            const result = this.findKeywordForValue(word, lineText);
            if (result) {
                return this.createValueHover(result.keyword, word);
            }

            // Check visibility keywords
            if (word in VISIBILITY_KEYWORDS) {
                return this.createVisibilityHover(word);
            }

            // Check platform conditions in file conditions like [!linux]
            const lowerWord = word.toLowerCase();
            const platformMatch = Object.keys(PLATFORM_CONDITIONS).find(p => p.toLowerCase() === lowerWord);
            if (platformMatch) {
                return this.getPlatformHover(platformMatch);
            }
        }

        // Check config qualifier context [word|word]
        if (this.isInConfigQualifier(lineText, position.character)) {
            return this.createConfigQualifierHover(word);
        }

        // Fallback: check if it's a keyword anywhere on the line
        const keyword = this.findKeyword(word);
        if (keyword) {
            return this.createKeywordHover(keyword, word);
        }

        return undefined;
    }

    private isInSectionHeader(line: string, charPos: number): boolean {
        const openBracket = line.indexOf('[');
        const closeBracket = line.indexOf(']');
        return openBracket !== -1 && closeBracket !== -1 &&
            charPos > openBracket && charPos <= closeBracket &&
            line.trimStart().startsWith('[');
    }

    private isInIfCondition(line: string, charPos: number): boolean {
        const match = line.match(/\bif\s*\(([^)]*)\)/);
        if (!match) return false;
        const ifStart = line.indexOf(match[0]);
        const parenOpen = line.indexOf('(', ifStart);
        const parenClose = line.indexOf(')', parenOpen);
        return charPos > parenOpen && charPos <= parenClose;
    }

    private isInConfigQualifier(line: string, charPos: number): boolean {
        // Find config qualifiers like key[Config|Platform] = value
        // but not section headers
        if (line.trimStart().startsWith('[')) return false;
        const regex = /\[([^\]]*)\]/g;
        let match;
        while ((match = regex.exec(line)) !== null) {
            const start = match.index;
            const end = start + match[0].length;
            if (charPos > start && charPos < end) {
                return true;
            }
        }
        return false;
    }

    private findKeyword(word: string): KeywordInfo | undefined {
        return KEYWORDS.find(kw =>
            kw.name === word || kw.aliases.includes(word)
        );
    }

    private findKeywordForValue(value: string, line: string): { keyword: KeywordInfo } | undefined {
        // Extract key from the line
        const match = line.match(/(?:[^:=\[\]]+:)?([a-zA-Z_][a-zA-Z0-9_]*)(?:\[[^\]]*\])?\s*=/);
        if (!match) return undefined;

        const keyName = match[1];
        const keyword = this.findKeyword(keyName);
        if (!keyword || !keyword.validValues) return undefined;

        if (keyword.validValues.includes(value)) {
            return { keyword };
        }
        return undefined;
    }

    private getSectionHover(word: string): vscode.Hover | undefined {
        if (word in SECTION_TYPES) {
            return this.createSectionTypeHover(word);
        }
        // Could be a project name, config name, etc. — no hover for those
        return undefined;
    }

    private getPlatformHover(word: string): vscode.Hover | undefined {
        const doc = PLATFORM_CONDITIONS[word];
        if (!doc) return undefined;

        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${word}** *(platform condition)*\n\n`);
        md.appendMarkdown(doc);
        return new vscode.Hover(md);
    }

    private createKeywordHover(keyword: KeywordInfo, actualWord: string): vscode.Hover {
        const md = new vscode.MarkdownString();
        md.isTrusted = true;

        if (actualWord !== keyword.name) {
            md.appendMarkdown(`**${actualWord}** *(alias for* \`${keyword.name}\`*)*\n\n`);
        } else {
            md.appendMarkdown(`**${keyword.name}**\n\n`);
        }

        md.appendMarkdown(keyword.documentation + '\n\n');

        if (keyword.aliases.length > 0) {
            md.appendMarkdown(`*Aliases:* ${keyword.aliases.map(a => '`' + a + '`').join(', ')}\n\n`);
        }

        if (keyword.validValues) {
            md.appendMarkdown(`*Valid values:* ${keyword.validValues.map(v => '`' + v + '`').join(', ')}\n\n`);
        }

        const typeLabel = keyword.valueType === 'bool' ? 'boolean' : keyword.valueType;
        md.appendMarkdown(`*Type:* ${typeLabel}`);

        if (keyword.supportsConfigQualifier) {
            md.appendMarkdown(` | *Supports* \`[Config|Platform]\` *qualifier*`);
        }

        return new vscode.Hover(md);
    }

    private createFunctionHover(func: { name: string; signature: string; documentation: string }): vscode.Hover {
        const md = new vscode.MarkdownString();
        md.isTrusted = true;
        md.appendCodeblock(func.signature, 'buildscript');
        md.appendMarkdown('\n\n' + func.documentation);
        return new vscode.Hover(md);
    }

    private createSectionTypeHover(word: string): vscode.Hover {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**[${word}]** *(section)*\n\n`);
        md.appendMarkdown(SECTION_TYPES[word]);
        return new vscode.Hover(md);
    }

    private createValueHover(keyword: KeywordInfo, value: string): vscode.Hover {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${value}** *(value for* \`${keyword.name}\`*)*\n\n`);
        md.appendMarkdown(keyword.documentation);
        return new vscode.Hover(md);
    }

    private createVisibilityHover(word: string): vscode.Hover {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${word}** *(visibility modifier)*\n\n`);
        md.appendMarkdown(VISIBILITY_KEYWORDS[word]);
        return new vscode.Hover(md);
    }

    private createConfigQualifierHover(word: string): vscode.Hover {
        const md = new vscode.MarkdownString();
        md.appendMarkdown(`**${word}** *(configuration/platform qualifier)*\n\n`);
        md.appendMarkdown('Restricts this setting to a specific configuration and/or platform.\n\n');
        md.appendMarkdown('Syntax: `key[Config|Platform] = value`\n\n');
        md.appendMarkdown('Examples: `[Debug|Win32]`, `[Release|x64]`, `[*]` (all configs)');
        return new vscode.Hover(md);
    }
}
