" Vim syntax file for Artic (.art)
" Fallback highlighting: when the language server provides semantic tokens,
" Neovim layers them on top of this file.
" Based on demo.art from DFOP-HD/vscode-artic and AnyDSL/artic.

if exists("b:current_syntax")
  finish
endif

" ---- Keywords ----
" Deliberately generous: an identifier that merely looks like a keyword is
" harmless, because the compiler reports real errors via the language server.
syn keyword articKeyword fn let mut if else while for in match return as
syn keyword articKeyword struct enum type static mod use implicit
syn keyword articKeyword break continue
syn keyword articKeyword trait impl where pub extern with super simd asm
syn keyword articKeyword self Self ptr
syn keyword articBoolean true false

" ---- Primitive types ----
syn keyword articType i8 i16 i32 i64 u8 u16 u32 u64 f16 f32 f64 bool char str

" ---- Numbers (later definitions win a same-column tie, so float is last) ----
syn match articNumber '\<\d\+\%(_\d\+\)*\%([iu]\%(8\|16\|32\|64\)\|f\%(16\|32\|64\)\)\?\>'
syn match articNumber '\<0x\x\+\%(_\x\+\)*\%([iu]\%(8\|16\|32\|64\)\)\?\>'
syn match articNumber '\<\d\+\%(_\d\+\)*\.\d\+\%([eE][+-]\?\d\+\)\?\%(f\%(16\|32\|64\)\)\?\>'

" ---- Operators ----
" Includes '/', which collides with comment starts at the same column.
" The comment regions below are defined later, so they win.
syn match articOperator '[+\-*/%=<>!&|^~]\+'

" ---- Calls, then declarations ----
" The declaration match must come AFTER the call match: both start on the
" same column and the last definition wins.
syn match articFunctionCall '\w\+\ze\s*[(\[]' contains=NONE
syn match articFunctionDecl '\<fn\>\s*@\?\zs\w\+\ze\s*[\[(]' contains=NONE

" ---- Artic sigils ----
syn match articAtSigil '@'
syn match articDollarSigil '\$\w*'
syn match articQuestionSigil '?\w\+'

" ==== Regions last, so they win any same-column start tie ====

" ---- Attributes: #[import(cc = "builtin")] ----
syn keyword articAttrKey import cc name contained
syn region articAttribute start='#\[' end='\]' contains=articString,articNumber,articAttrKey

" ---- Strings and chars ----
syn match articEscape contained '\\.'
syn region articString start=+"+ skip=+\\"+ end=+"+ contains=articEscape
syn region articChar start=+'+ skip=+\\'+ end=+'+ contains=articEscape

" ---- Comments ----
syn region articComment start='//' end='$' contains=@Spell keepend
syn region articBlockComment start='/\*' end='\*/' contains=@Spell

hi def link articKeyword       Keyword
hi def link articBoolean       Boolean
hi def link articType          Type
hi def link articAttribute     PreProc
hi def link articAttrKey       Identifier
hi def link articAtSigil       Special
hi def link articDollarSigil   Special
hi def link articQuestionSigil Special
hi def link articNumber        Number
hi def link articString        String
hi def link articChar          Character
hi def link articEscape        SpecialChar
hi def link articComment       Comment
hi def link articBlockComment  Comment
hi def link articOperator      Operator
hi def link articFunctionDecl  Function
hi def link articFunctionCall  Function

let b:current_syntax = "artic"
