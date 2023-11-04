if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

let b:undo_ftplugin = 'setlocal comments<  commentstring< formatoptions<'

setlocal formatoptions-=t

setlocal comments=sO:*\ -,mO:*\ \ ,exO:*/,://

setlocal commentstring=//\ %s

setlocal foldmethod=syntax
setlocal foldtext=ftproto#FoldText()
