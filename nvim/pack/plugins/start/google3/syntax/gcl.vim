" Copyright 2005-2007 Google Inc.
" All Rights Reserved.
" Author: plakal@google.com (Manoj Plakal), mheule@google.com (Markus Heule)
"
" gcl.vim: Vim syntax file for GCL config files.
"
" GCL Language links:
" http://g3doc/configlang/g3doc/gcl-reference.md
" http://go/gclbuiltins
" //depot/google3/configlang

" For version 5.x: Clear all syntax items
" For version 6.x: Quit when a syntax file was already loaded
if version < 600
  syntax clear
elseif exists("b:current_syntax")
  finish
endif

" Valid characters of identifiers: - 0-9 A-Z _ a-z
if version >= 600
  setlocal iskeyword=45,48-57,65-90,95,97-122
else
  set iskeyword=45,48-57,65-90,95,97-122
endif

" Keywords.
syn keyword gclConstant     true false null external
syn keyword gclProperty     final local validation_ignore template
syn keyword gclInclude      import as
syn keyword gclKeyword      expansion lambda assert super up expect
syn keyword gclKeyword      declare_objtype
syn match   gclComment      "//.*$" contains=gclTodo,@Spell
syn match   gclComment      "#.*$" contains=gclTodo,@Spell
syn keyword gclTodo         contained TODO FIXME

" Strings.
" String literals are delimited by "" and contain the usual escapes.
" String literals can also contain interpolated expressions within %
" (where the % is escaped using double %%).
" We ignore the escaping of ordinary characters for now.
syn region gclString   start=+"+ end=+"+ skip=+\\\\\|\\"+
                        \ contains=gclEscape,gclExpr oneline keepend
" Multi-line string.
syn region gclString   start=+"""+ end=+"""+ skip=+\\\\\|\\"+
                        \ contains=gclEscape,gclExpr keepend
syn match  gclEscape   +\\[abfnrtv'"\\\n]+ contained
syn match  gclEscape   "\\\o\o\+" contained display
syn match  gclEscape   "\\\x\x\+" contained display
" Match escaped %.
syn match  gclEscape   +%%+ contained display
" Match all %expr%.
syn match  gclExpr     "%[^%]\+%" contained
" Raw strings (no escaping/interpolation) are delimited using single quotes.
syn region gclRawStr   start=+'+ end=+'+ oneline keepend
" Raw multi-line string.
syn region gclRawStr   start=+'''+ end=+'''+ keepend

" Numeric literals = integers and floats.
" Integer literals are the usual octal, decimal, hex except that they can
" include underscores and have a trailing unit (K/M/G/T/P). In addition,
" we also allow integers of the form <decimal-fraction>[K/M/G/T/P], e.g., 1.5G
" Octal numbers: 012_34K
syn match   gclNumber  "\<0\(\o[0-7_]*\o\|\o\)[KMGTP]\?\>" display
" Decimal numbers: 12_34K
syn match   gclNumber  "\<\(\d[0-9_]*\d\|\d\)[KMGTP]\?\>" display
" Hexadecimal numbers: 0x12_34K
syn match   gclNumber  "\<0x\(\x[0-9a-fA-F_]*\x\|\x\)[KMGTP]\?\>" display
" Fractional numbers: 12.34K (The unit is mandatory)
syn match   gclNumber  "\<\d\+\.\d\+[KMGTP]\>" display
" Floating-point numbers: 3.1415_9265
syn match   gclNumber  "\<\d*\(\.\(\d+\)\?\)\?\([eE][+-]\?\d\+\)\>" display

" Object definitions: testcase and test.
" Highlight both the keyword and the name of the defined object that
" follows the keyword. Except when they are preceded by a '.'
" which is the borg way of allowing attributes with these special names.
syn region  gclObjectDef    matchgroup=gclObjectKeyword
                            \ start="\<\.\@<!\(testcase\|test\)\>"
                            \ matchgroup=gclObjectName
                            \ end="\<[A-Za-z_][A-Za-z0-9_\-]*\>"
                            \ oneline skipwhite keepend

" Identifiers.
" Normally these don't require highlighting but GCL allows
" identifiers with arbitrary characters inside backquotes.
syn region  gclRawIdent matchgroup=Normal start=+`+ end=+`+ oneline keepend

" Sections within a GCL object. Note that not all sections can
" occur within all objects, but we punt on context-sensitivity
" for now since that would require us to parse GCL
" within the edit buffer. Keep both sections and attributes (below)
" in sync with the GCL source.
syn keyword gclSection      vars

" Attributes within sections. Note that not all attributes can
" occur within all sections, but we punt on context-sensitivity.
" Currently we do not have any registered attribute name in GCL. Use the line
" below as an example for a future attribute.
" syn keyword gclAttribute    attribute_name

" GCL's builtin functions. Keep this in sync with:
" grep ConfFunction::Register google3/configlang/builtins*.cc |
"   cut -d\" -f 2 | sort | grep -v '^__' | fmt -50 |
"   sed 's/^/syn keyword gclBuiltin /'
" and also update borg.vim
syn keyword gclBuiltin attributes base64_decode base64_encode basename
syn keyword gclBuiltin borg_uuid _build_proto_tuple check_assertions
syn keyword gclBuiltin collate_map commonprefix compute_field cond
syn keyword gclBuiltin defined defined_expr eval eval_expr expid
syn keyword gclBuiltin filedir filter findall fingerprint
syn keyword gclBuiltin flatten float fmt get getcwd getenv get_objects
syn keyword gclBuiltin glob has_attribute has_final_attribute has_key
syn keyword gclBuiltin has_local_attribute has_raw_attribute head int
syn keyword gclBuiltin is_bool isdir is_external isfile is_float is_int
syn keyword gclBuiltin is_lambda is_list is_map is_null is_string
syn keyword gclBuiltin is_tuple items join keys len listdir lookup
syn keyword gclBuiltin map maptuple match mkmap mktuple myfilename
syn keyword gclBuiltin objectid objselector objtype parse_prod_hostname
syn keyword gclBuiltin pb_parse_enum_type proto_reflect raw_attributes
syn keyword gclBuiltin real_username reduce replace sort_asc sort_dsc
syn keyword gclBuiltin split strftime stringhash strptime substr tail
syn keyword gclBuiltin tolower tostring toupper translatefully utf8len
syn keyword gclBuiltin values walltime

" Tuples and lists (folding only).
syn region  gclTuple   start='{' end='}'    transparent fold
syn region  gclList    start='\[' end='\]'  transparent fold

if version >= 508 || !exists("did_gcl_syn_inits")
  if version <= 508
    let did_gcl_syn_inits = 1
    command -nargs=+ HiLink hi link <args>
  else
    command -nargs=+ HiLink hi def link <args>
  endif

  " The default methods for highlighting.  Can be overridden later
  HiLink gclKeyword            Keyword
  HiLink gclConstant           Constant
  HiLink gclProperty           StorageClass
  HiLink gclInclude            Include
  HiLink gclComment            Comment
  HiLink gclTodo               Todo
  HiLink gclObjectKeyword      Keyword
  HiLink gclObjectName         Function
  HiLink gclSection            Identifier
  HiLink gclAttribute          Typedef
  HiLink gclString             String
  HiLink gclRawStr             String
  HiLink gclEscape             Special
  HiLink gclExpr               Special
  HiLink gclNumber             Number
  HiLink gclRawIdent           String
  HiLink gclBuiltin            Function

  delcommand HiLink
endif

let b:current_syntax = "gcl"

" vim: ts=8
