" hyphen(-) is a valid identifier character, often used in varz names.
setlocal isident+=-

setlocal formatoptions-=t
setlocal comments=://
setlocal commentstring=//\ %s

" Make gf work with absolute imports
setlocal includeexpr=substitute(v:fname,'//','','')
setlocal include=^import\ '\/\/

" Enable syntax-based folding, if specified.
setlocal foldmethod=syntax
setlocal foldtext=google3#GclFold()
