import * as vscode from 'vscode';
import { DccTestDocument } from '../model/dccTestDocument';
import { DirDocument } from '../model/dccDirDocument';

export class DccTestModelCache implements vscode.Disposable {
    private readonly m_docs = new Map<string, { version: number; doc: DccTestDocument }>();
    private readonly m_dirDocs = new Map<string, { version: number; doc: DirDocument }>();
    private readonly m_dirSectionDocs = new Map<string, { key: string; doc: DirDocument }>();

    get(document: vscode.TextDocument): DccTestDocument {
        const uri = document.uri.toString();
        const version = document.version;
        const hit = this.m_docs.get(uri);
        if (hit && hit.version === version && hit.doc.text === document.getText())
            return hit.doc;

        const doc = DccTestDocument.parse(document.getText(), uri);
        this.m_docs.set(uri, { version, doc });
        return doc;
    }

    getDir(text: string, uri: string): DirDocument {
        const hit = this.m_dirDocs.get(uri);
        if (hit && hit.version === text.length && hit.doc.text === text)
            return hit.doc;

        const doc = DirDocument.parse(text, uri);
        this.m_dirDocs.set(uri, { version: text.length, doc });
        return doc;
    }

    getForUri(uri: string): DccTestDocument | undefined {
        return this.m_docs.get(uri)?.doc;
    }

    dispose(): void {
        this.m_docs.clear();
        this.m_dirDocs.clear();
        this.m_dirSectionDocs.clear();
    }
}
