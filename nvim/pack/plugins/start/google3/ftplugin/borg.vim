setlocal formatoptions-=t
setlocal comments=://
setlocal commentstring=//\ %s

" Make gf work with imports in Borg files for import paths like
"   import '//foo/bar/baz.borg'
" Relative import paths should already work with gf.
setlocal includeexpr=substitute(v:fname,'//','','')
setlocal include=^import\ '\/\/

" Enable syntax-based folding, if specified.
setlocal foldmethod=syntax
setlocal foldtext=google3#GclFold()
