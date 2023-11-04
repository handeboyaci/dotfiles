hi StatusLine gui=NONE guibg=#8787af guifg=#262626 

augroup StatusLine
  autocmd!
  autocmd StatusLine ModeChanged  *:n* hi StatusLine guibg=#8787af
  autocmd StatusLine ModeChanged  *:i  hi StatusLine guibg=#87af87
  autocmd StatusLine ModeChanged  *:R  hi statusline guibg=#af5f5f
augroup END

function! StatusLine()
  let l:workdir = substitute(
        \ substitute(getcwd(), $HOME, '~', ''), $CITC_ROOT, $CITC_NAME, '')
  let l:stl = '[%n]%(['.l:workdir.'] %)'

  let l:relpath = substitute(expand('%:h'), $HOME, '~', '')

  if l:relpath !=# '.' && l:relpath !=# ''
    let l:stl .= '%('.pathshorten(l:relpath).'/%)'
  endif

  let l:stl .= '%t%( [%M%R%H%W]%)%=%(%{v:lua.LspStatus()} |%) %P col %c %y'

  return l:stl

endfunction

set statusline=%{%StatusLine()%}
