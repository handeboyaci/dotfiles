setlocal ft=mail
setlocal noexpandtab
setlocal autoindent
setlocal spell
setlocal spelllang=en,tr
setlocal formatoptions+=w
setlocal textwidth=78
setlocal comments+=nb:>

" indent the e-mail addresses in to/cc/bcc by a tab
function! MuttIndent(lnum)
  let s:prev_line = getline(a:lnum - 1)
  if s:prev_line =~? '\v^(to|b?cc):.*,$'
    return &shiftwidth
  else
    return indent(a:lnum - 1)
  endif
endfunction
setlocal indentexpr=MuttIndent(v:lnum)

syn match mailTable keepend contains=mailEmail "^\s*|.*" transparent

" use C-t to navigate headers
nnoremap <C-t> :call search('\v^(To<bar>Bcc<bar>Cc<bar>Subject):')<CR>A
inoremap <C-t> <ESC>:call search('\v^(To<bar>Bcc<bar>Cc<bar>Subject):')<CR>A

" start with cursor at the email body, and format below
call search('^$')
let b:line = line(".")
normal gqG
call cursor(b:line, 0)
startinsert

au BufWritePre <buffer> %!$HOME/.config/mutt/flowed-to-normal.sh
