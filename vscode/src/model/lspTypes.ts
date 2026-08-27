export interface LspPosition {
    line: number;
    character: number;
}

export interface LspRange {
    start: LspPosition;
    end: LspPosition;
}

export interface LspLocation {
    uri: string;
    range: LspRange;
}

export interface LspLocationLink {
    originSelectionRange?: LspRange;
    targetUri: string;
    targetRange: LspRange;
    targetSelectionRange?: LspRange;
}

export interface LspTextEdit {
    range: LspRange;
    newText: string;
}

export interface LspWorkspaceEdit {
    changes?: Record<string, LspTextEdit[]>;
}

export interface LspDiagnostic {
    range: LspRange;
    message: string;
    severity?: number;
    code?: string | number;
    source?: string;
}

export interface LspSymbol {
    name: string;
    detail?: string;
    kind: number;
    range: LspRange;
    selectionRange?: LspRange;
    children?: LspSymbol[];
}

export interface LspSemanticTokens {
    data?: number[];
}

export interface LspHover {
    contents?: unknown;
    range?: LspRange;
}

export interface LspParameterInformation {
    label: string;
    documentation?: string;
}

export interface LspSignatureInformation {
    label: string;
    documentation?: string;
    parameters?: LspParameterInformation[];
    activeParameter?: number;
}

export interface LspSignatureHelp {
    signatures?: LspSignatureInformation[];
    activeSignature?: number;
    activeParameter?: number;
}

export interface LspDocumentHighlight {
    range: LspRange;
    kind?: number;
}

export interface LspPrepareRenameResult {
    range: LspRange;
    placeholder?: string;
}

export interface LspCodeAction {
    title: string;
    kind?: string;
    diagnostics?: LspDiagnostic[];
    edit?: LspWorkspaceEdit;
}

export interface LspInlayHint {
    position: LspPosition;
    label?: string;
    kind?: number;
    paddingLeft?: boolean;
    paddingRight?: boolean;
}

export interface LspCompletionItem {
    label?: string;
    kind?: number;
    detail?: string;
    documentation?: string | { kind?: string; value?: string };
    sortText?: string;
    preselect?: boolean;
    insertText?: string;
    insertTextFormat?: number;
    textEdit?: LspTextEdit;
    additionalTextEdits?: LspTextEdit[];
    command?: unknown;
}

export interface LspCompletionList {
    items?: LspCompletionItem[];
    isIncomplete?: boolean;
}
