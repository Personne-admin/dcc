export const SERVER_TOKEN_TYPES = [
    'namespace',
    'type',
    'class',
    'enum',
    'interface',
    'struct',
    'typeParameter',
    'parameter',
    'variable',
    'property',
    'enumMember',
    'function',
    'method',
    'macro',
    'keyword',
    'modifier',
    'comment',
    'string',
    'number',
    'operator',
    'asmPlaceholder',
    'asmRegister',
] as const;

export const SERVER_TOKEN_MODIFIERS = ['declaration', 'readonly', 'static', 'deprecated', 'defaultLibrary'] as const;

export const SERVER_TOKEN_FALLBACK = 'identifier';

export function serverTokenTypeToDccName(index: number): string {
    const name = index >= 0 && index < SERVER_TOKEN_TYPES.length ? SERVER_TOKEN_TYPES[index] : undefined;
    switch (name) {
        case 'namespace':
        case 'parameter':
        case 'variable':
        case 'property':
        case 'comment':
        case 'asmPlaceholder':
        case 'asmRegister':
            return 'identifier';
        case 'type':
        case 'class':
        case 'enum':
        case 'interface':
        case 'struct':
        case 'typeParameter':
            return 'type';
        case 'enumMember':
            return 'constant';
        case 'function':
        case 'method':
            return 'global';
        case 'macro':
        case 'keyword':
        case 'modifier':
        case 'operator':
            return 'keyword';
        case 'string':
        case 'number':
            return 'constant';
        default:
            return SERVER_TOKEN_FALLBACK;
    }
}

export function serverTokenModifiersToDccMask(mask: number): number {
    return mask & 0b11;
}
