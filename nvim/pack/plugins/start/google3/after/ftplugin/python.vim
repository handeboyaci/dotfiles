function! PythonIncludeExpr(arg) abort
  let l:result = substitute(
        \ substitute(a:arg, b:grandparent_match, b:grandparent_sub, ''),
        \ b:parent_match, b:parent_sub, '')

  let l:result = substitute(l:result, ' as .*', '', '')
  let l:result = substitute(l:result, ' import ', '.', '')
  let l:result = substitute(l:result, '^\(from\|import\) ', '', 'g')
  return substitute(l:result,b:child_match,b:child_sub,'g')
endfunction

set isfname-=,
set include=^\s*\\zs\\(from\ \\f\\+\ \\)\\?import\ \\f\\+\\ze
set includeexpr=PythonIncludeExpr(v:fname)
set suffixesadd+=/__init__.py

nnoremap <expr> gf  getline(".")=~&include ? "Vgf" : "gf"
