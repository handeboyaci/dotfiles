set conceallevel=2
set concealcursor=c
hi Conceal guibg=NONE

set foldmethod=expr
syntax spell toplevel

function! SyncTex()
  let cmd = 'evince_forward_search ' . expand("%:p:r") . '.pdf ' . line(".") . ' ' . expand("%:p")
  let output = system(cmd)
endfunction

nnoremap <buffer> <silent> <Leader>ls :call SyncTex()<CR>

call prose#setup()
