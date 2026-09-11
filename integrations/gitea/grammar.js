(function (Prism) {
  if (!Prism || !Prism.languages)
    return;

  var dcEscape = /\\(?:x[0-9a-fA-F]{2}|u\{[0-9a-fA-F]{1,6}\}|u[0-9a-fA-F]{4}|U[0-9a-fA-F]{8}|[ntr0\\'"])/;

  Prism.languages.dc = {
    'string': [
      {
        pattern: /u"(?:\\.|[^"\\\r\n])*"/,
        greedy: true,
        inside: {
          'escape': dcEscape,
          'prefix': {
            pattern: /^u/,
            alias: 'operator'
          }
        }
      },
      {
        pattern: /"(?:\\.|[^"\\\r\n])*"/,
        greedy: true,
        inside: {
          'escape': dcEscape
        }
      }
    ],
    'char': [
      {
        pattern: /u'(?:\\.|[^'\\\r\n])+'/,
        greedy: true,
        inside: {
          'escape': dcEscape,
          'prefix': {
            pattern: /^u/,
            alias: 'operator'
          }
        }
      },
      {
        pattern: /'(?:\\.|[^'\\\r\n])+'/,
        greedy: true,
        inside: {
          'escape': dcEscape
        }
      }
    ],
    'comment': [
      {
        pattern: /(^|[^\\:])\/\/.*/,
        lookbehind: true,
        greedy: true
      },
      {
        pattern: /\/\*[\s\S]*?\*\//,
        greedy: true
      }
    ],
    'annotation': {
      pattern: /@[a-zA-Z_][a-zA-Z0-9_]*/,
      alias: 'directive'
    },
    'keyword': [
      {
        pattern: /\b(?:if|else|while|for|do|match|break|continue|return|defer|as|in|static|module|import|public|struct|enum|union|using|const|restrict|volatile|extern|asm|sizeof|alignof|offsetof|compiles)\b/
      }
    ],
    'boolean': {
      pattern: /\b(?:true|false|null)\b/
    },
    'builtin': {
      pattern: /\b(?:u8|i8|u16|i16|u32|i32|u64|i64|usize|isize|f32|f64|char|bool|void|null_t)\b/
    },
    'number': [
      {
        pattern: /\b0[xX][0-9a-fA-F][0-9a-fA-F_]*/
      },
      {
        pattern: /\b0[bB][01][01_]*/
      },
      {
        pattern: /\b0[oO][0-7][0-7_]*/
      },
      {
        pattern: /\b\d[\d_]*(?:\.\d[\d_]*(?:[eE][+-]?\d[\d_]*)?|[eE][+-]?\d[\d_]*)\b/
      },
      {
        pattern: /\b\d[\d_]*\b/
      }
    ],
    'function': {
      pattern: /[a-zA-Z_]\w*(?=\s*(?:!\s*\([^()]*\)\s*)?\()/
    },
    'class-name': {
      pattern: /\b[A-Z][a-zA-Z0-9_]*\b/
    },
    'operator': {
      pattern: /(?:=>|->|::|\.\.\.|<<=|>>=|\.\.=|\+\+|--|&&|\|\||==|!=|<=|>=|<<|>>|\+=|-=|\*=|\/=|%=|&=|\|=|\^=|\.\.|[+\-*/%=<>!&|^~?.$#])/
    },
    'punctuation': {
      pattern: /[{}[\]();:,]/
    }
  };

  Prism.languages.dcc = Prism.languages.dc;
})(typeof window !== 'undefined' ? window.Prism : undefined);
