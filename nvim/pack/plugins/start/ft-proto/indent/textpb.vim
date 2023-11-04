" Vim indent file
" Language:    Text-Format Protocol Buffer
" Author:      Mark Lodato <lodato@google.com>
"
" Based on vb.vim and http://stackoverflow.com/questions/4829244.

if exists("b:did_indent")
  finish
endif
let b:did_indent = 1

setlocal indentexpr=ftproto#ProtoTextfmtGetIndent(v:lnum)
setlocal indentkeys=0{,0<<>,0},0<>>,o,O,!^F
