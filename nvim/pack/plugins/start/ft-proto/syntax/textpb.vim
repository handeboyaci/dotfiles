" Vim syntax file
" Language:    Text-Format Protocol Buffer
" Author:      Mark Lodato <lodato@google.com>
"
" Optional customizations:
"
"   hi link pbtfFieldName Label
"   hi link pbtfOctalZero Error  " default is PreProc
"
" Modified from proto.vim and c.vim.

if version < 600
  syntax clear
elseif exists('b:current_syntax')
  finish
endif

" Allow "." in identifiers.  Also allows us to use \> after "." in floats.
setlocal iskeyword+=.

syn case match

" Comments
syn keyword     pbtfTodo        contained TODO FIXME XXX
syn region      pbtfComment     start="#" end="$" keepend contains=pbtfTodo,@Spell

" String constants
" Highlight special characters (those which have a backslash) differently
syn match       pbtfSpecial     display contained "\\\(x\x\+\|\o\{1,3}\|.\|$\)"
syn match       pbtfSpecial     display contained "\\\(u\x\{4}\|U\x\{8}\)"
syn region      pbtfString      oneline start=/"/ skip=/\\"/ end=/"/ contains=pbtfSpecial,@Spell
syn region      pbtfString      oneline start=/'/ skip=/\\'/ end=/'/ contains=pbtfSpecial,@Spell
" TODO proto1 raw strings

" Numeric constants
" NOTE: floats ending with "f" are proto1-only
" NOTE: floats MUST begin with a digit (e.g., ".1" is invalid)
syn case ignore
syn match       pbtfNumber      display "\<\d\+\>"
syn match       pbtfNumber      display "\<0x\x\+\>"
syn match       pbtfOctal       display "\<0\o\+\>" contains=pbtfOctalZero
syn match       pbtfOctalZero   display contained "\<0"
syn match       pbtfOctalError  display "\<0\o*[89]\d*\>"
syn match       pbtfFloat       display "\<\d\+f\>"
syn match       pbtfFloat       display "\<\d\+\.\d*f\=\>"
syn match       pbtfFloat       display "\<\d\+\.\=\d*e[-+]\=\d\+f\=\>"
syn case match

" Named constants
" NOTE: "inff" is proto1-only
syn keyword     pbtfConstant    inf infinity nan inff
syn keyword     pbtfBool        true false

" Identifiers.
syn match       pbtfIdentifier  display "\<\K\k*"
syn match       pbtfFieldName   display "\(^\|[;<>{}]\)\s*\K\k*"

" Messages (folding only).
syn region      pbtfMessage     start="{" end="}" transparent fold
syn region      pbtfMessage     start="<" end=">" transparent fold

if version >= 508 || !exists('did_proto_syn_inits')
  if version < 508
    let did_proto_syn_inits = 1
    command -nargs=+ HiLink hi link <args>
  else
    command -nargs=+ HiLink hi def link <args>
  endif

  HiLink pbtfTodo         Todo
  HiLink pbtfBool         Boolean
  HiLink pbtfConstant     Constant
  HiLink pbtfNumber       Number
  HiLink pbtfOctal        Number
  HiLink pbtfOctalZero    PreProc
  HiLink pbtfOctalError   Error
  HiLink pbtfFloat        Float
  HiLink pbtfComment      Comment
  HiLink pbtfString       String
  HiLink pbtfSpecial      SpecialChar
  HiLink pbtfIdentifier   Identifier

  delcommand HiLink
endif

let b:current_syntax = 'textpb'
