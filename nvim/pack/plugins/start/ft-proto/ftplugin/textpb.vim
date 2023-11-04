" Filetype for protobuf textformat.
if exists('b:did_ftplugin')
  finish
endif
let b:did_ftplugin = 1

let b:undo_ftplugin =
    \ 'setlocal formatoptions< formatexpr< comments< commentstring< matchpairs<'

setlocal formatoptions-=t

setlocal comments=b:#
setlocal commentstring=#\ %s

setlocal matchpairs+=<:>

setlocal foldmethod=syntax
setlocal foldtext=ftproto#FoldText()
