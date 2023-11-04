if version < 600
  syntax clear
elseif exists('b:current_syntax')
  finish
endif

syn case match

syn keyword pbStorageClass parsed
syn keyword pbKeyword      syntax package
syn keyword pbStructure    group message oneof
syn keyword pbRepeat       required optional repeated
syn keyword pbFieldOption  default deprecated

" NOTE: We intentionally no longer highlight boolean. 'boolean' is fully
" compatibile with 'bool' in proto1, and it is not a valid keyword in proto2.
" You can safely s/boolean/bool/ in your .proto files if you want it be
" highlighted.
syn keyword pbType double float int64 uint64 int32 fixed32 fixed64
syn keyword pbType bool string map
syn keyword pbBool true false
syn keyword pbTypedef enum
" Types that are new in proto2:
syn keyword pbType uint32 sfixed32 sfixed64 sint32 sint64 bytes

setlocal iskeyword+=+
syn keyword pbLangSpec c++ c++header c# go java python sawzall

" Note: the leading expression below inhibits matches unless they appear
" immediately after a '=' or whitespace character.
syn match   pbNumber /\(^\|[ \t=]\)\@<=-\?0[xX]\x\+\>/
syn match   pbNumber /\(^\|[ \t=]\)\@<=-\?\(\d*\.\)\?\d\+\([eE][-+]\?\d\+\)\?\>/

syn keyword pbTodo    TODO FIXME XXX contained
syn match   pbNext    /\c\vnext (available )?(tag|id)/ contained
syn region  pbComment start="/\*" end="\*/" contains=pbTodo,pbNext,@Spell
syn match   pbComment /\/\/.*$/ contains=pbTodo,pbNext,@Spell
syn region  pbString  start=/"/ skip=/\\./ end=/"/ contains=@Spell
syn region  pbString  start=/'/ skip=/\\./ end=/'/ contains=@Spell

syn keyword pbKeyword extensions to max
syn keyword pbKeyword reserved to max

" TODO(mbrukman): Is this complete support for services and RPC?
" FIXME: Many of the keywords below are valid identifiers for fields, but they
" are always highlighted. They should be modified to be highlighted only in
" context. E.g. many need to be preceded by a 'option' and others by
" 'option x ='
syn keyword pbKeyword       service rpc stream returns option import extend
syn keyword pbMethodOption  deadline duplicate_suppression
syn keyword pbMethodOption  protocol client_logging server_logging
syn keyword pbMethodOption  fail_fast request_format response_format
syn keyword pbNetProto      tcp udp
syn keyword pbSecurity      security_level none integrity privacy_and_integrity
syn keyword pbSecurity      strong_privacy_and_integrity
syn keyword pbServiceOption failure_detection_delay
syn keyword pbFormatOption  uncompressed zippy_compressed
syn keyword pbFormatOption  UNCOMPRESSED ZIPPY_COMPRESSED

" Folding.
syn region  pbMessage  start='{' end='}' transparent fold

if version >= 508 || !exists('did_proto_syn_inits')
  if version < 508
    let did_proto_syn_inits = 1
    command -nargs=+ HiLink hi link <args>
  else
    command -nargs=+ HiLink hi def link <args>
  endif

  " Protocol buffer
  HiLink pbStorageClass StorageClass
  HiLink pbKeyword      Keyword
  HiLink pbStructure    Structure
  HiLink pbRepeat       Repeat
  HiLink pbFieldOption  Keyword
  HiLink pbLangSpec     Include
  HiLink pbType         Type
  HiLink pbTypedef      Typedef
  HiLink pbBool         Boolean
  HiLink pbNumber       Number
  HiLink pbTodo         Todo
  HiLink pbNext         Todo
  HiLink pbComment      Comment
  HiLink pbString       String

  " Stubby services and RPC
  HiLink pbMethodOption  Keyword
  HiLink pbNetProto      Keyword
  HiLink pbSecurity      Keyword
  HiLink pbServiceOption Keyword
  HiLink pbFormatOption  Keyword

  delcommand HiLink
endif

let b:current_syntax = 'proto'
