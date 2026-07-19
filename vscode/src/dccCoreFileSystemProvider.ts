import * as vscode from 'vscode';
import { LanguageClient } from 'vscode-languageclient/node';

export class DccCoreFileSystemProvider implements vscode.FileSystemProvider {
    private readonly m_client: LanguageClient;
    private readonly m_emitter: vscode.EventEmitter<vscode.FileChangeEvent[]>;

    constructor(client: LanguageClient) {
        this.m_client = client;
        this.m_emitter = new vscode.EventEmitter<vscode.FileChangeEvent[]>();
        this.onDidChangeFile = this.m_emitter.event;
    }

    readonly onDidChangeFile!: vscode.Event<vscode.FileChangeEvent[]>;

    async stat(_uri: vscode.Uri): Promise<vscode.FileStat> {
        return {
            type: vscode.FileType.File,
            ctime: 0,
            mtime: 0,
            size: 0,
            permissions: vscode.FilePermission.Readonly,
        };
    }

    async readDirectory(_uri: vscode.Uri): Promise<[string, vscode.FileType][]> {
        return [];
    }

    async readFile(uri: vscode.Uri): Promise<Uint8Array> {
        const result: { text: string } = await this.m_client.sendRequest(
            'dccd/virtualDocument',
            { uri: uri.toString() },
        );

        return new TextEncoder().encode(result.text);
    }

    writeFile(
        _uri: vscode.Uri,
        _content: Uint8Array,
        _options: { readonly create: boolean; readonly overwrite: boolean },
    ): void {
        throw vscode.FileSystemError.NoPermissions(
            'dcc-core is a read-only virtual file system',
        );
    }

    createDirectory(_uri: vscode.Uri): void {
        throw vscode.FileSystemError.NoPermissions(
            'dcc-core is a read-only virtual file system',
        );
    }

    delete(_uri: vscode.Uri, _options: { readonly recursive: boolean }): void {
        throw vscode.FileSystemError.NoPermissions(
            'dcc-core is a read-only virtual file system',
        );
    }

    rename(
        _oldUri: vscode.Uri,
        _newUri: vscode.Uri,
        _options: { readonly overwrite: boolean },
    ): void {
        throw vscode.FileSystemError.NoPermissions(
            'dcc-core is a read-only virtual file system',
        );
    }

    watch(
        _uri: vscode.Uri,
        _options: { readonly recursive: boolean; readonly excludes: readonly string[] },
    ): vscode.Disposable {
        return new vscode.Disposable(() => {});
    }
}
