
"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""
"                     Change case of Visual selection                     "
"""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""""

function! tools#TwiddleCase(str)
  if a:str ==# toupper(a:str)
    let result = tolower(a:str)
  elseif a:str ==# tolower(a:str)
    let result = substitute(a:str,'\(\<\w\+\>\)', '\u\1', 'g')
  else
    let result = toupper(a:str)
  endif
  return result
endfunction
vnoremap ~ ygv"=tools#TwiddleCase(@")<CR>Pgv

