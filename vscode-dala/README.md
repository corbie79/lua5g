# Dala for VSCode

Syntax highlighting, snippets, and language support for Dala.

## Features

### Syntax Highlighting
- All Dala keywords: `class`, `extends`, `interface`, `enum`, `try`/`except`/`finally`, `match`/`case`
- Type annotations: `: number`, `: string`, `: ClassName`
- Access modifiers: `public`, `private`, `protected`, `readonly`
- Lambda expressions: `|x| x * 2`
- String interpolation: `f"hello {name}"`
- Nullable chaining: `?.`

### Snippets
- `class` - Class declaration
- `classext` - Class with extends
- `interface` - Interface declaration
- `enum` - Enum declaration
- `try` - Try/except/finally
- `match` - Pattern matching
- `lambda` - Lambda expression
- `fstr` - F-string interpolation
- `property` - Getter/setter property
- `describe` - Test suite
- `profile` - Profiler block
- `import` - Import module
- `ltype` - Typed local variable

### Code Folding
Automatic folding for `class`, `interface`, `enum`, `function`, `if`, `for`, `try`, `match` blocks.

### Auto-Indentation
Smart indentation for all block structures.

## Installation

1. Copy `vscode-dala` folder to `~/.vscode/extensions/`
2. Restart VSCode
3. Open any `.lua` or `.dala` file

## Color Themes

The extension uses standard TextMate scopes:
- `keyword.declaration.class` - class/extends/interface/enum
- `keyword.control` - try/except/finally/match/case
- `storage.modifier` - public/private/protected/readonly
- `support.type` - number/string/boolean/table/function
- `entity.name.type` - class/interface/enum names
- `variable.language` - self/super
