inoremap JK <C-c>"zyy"zpVr=o<CR>
inoremap KJ <C-c>"zyy"zpVr-o<CR>

let g:markdown_folding = 1

let g:tagbar_type_markdown = {
    \ 'ctagstype' : 'markdown',
    \ 'kinds' : [
        \ 'h:TOC',
    \ ],
    \ 'sort' : 0,
\ }

function! SetMakePrg(format)
  let &makeprg="panzer % -o %:p:r.".a:format
endfunction

command! -n=+ SMP call SetMakePrg(<f-args>)

if &makeprg == "make"
  SMP html
endif

call prose#setup()

iabbrev h2 H~2~
iabbrev o2 O~2~
iabbrev h2o H~2~O
iabbrev tio2 TiO~2~
iabbrev TiO2 TiO~2~
