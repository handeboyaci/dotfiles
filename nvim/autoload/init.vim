function! init#Redir(cmd, rng, start, end) abort
  for win in range(1, winnr('$'))
    if getwinvar(win, 'scratch')
      execute win . 'windo close'
    endif
  endfor
  if a:cmd =~# '^!'
    let cmd = a:cmd =~# ' %'
      \ ? matchstr(substitute(a:cmd, ' %', ' ' . expand('%:p'), ''), '^!\zs.*')
      \ : matchstr(a:cmd, '^!\zs.*')
    if a:rng == 0
      let output = systemlist(cmd)
    else
      let joined_lines = join(getline(a:start, a:end), '\n')
      let cleaned_lines = substitute(shellescape(joined_lines), "'\\\\''", "\\\\'", 'g')
      let output = systemlist(cmd . ' <<< $' . cleaned_lines)
    endif
  else
    redir => output
    execute a:cmd
    redir END
    let output = split(output, "\n")
  endif
  vnew
  let w:scratch = 1
  setlocal buftype=nofile bufhidden=wipe nobuflisted noswapfile
  call setline(1, output)
endfunction

function! init#FoldText() abort
  let reg='{{'.'{\d\='

  for c in split(&commentstring, '%s')
    let reg = reg.'\|'.escape(c, '*')
  endfor

  let foldchar = matchstr(&fillchars, 'fold:\zs.')
  let wlength = min([winwidth(0), 80])
  let indent = max([indent(v:foldstart), v:foldlevel-1])
  let nlines = (v:foldend-v:foldstart+1).' lines '
  let ftext = substitute(getline(v:foldstart), reg,'','g')

  return repeat(' ', indent) 
        \ . substitute(ftext, '^\s*\(.\{-}\)\s*$', '\1', '')
        \ . repeat(foldchar,
        \          wlength - strlen(nlines.ftext) - 2 - indent)
        \ . ' ' . nlines  . '≡'

endfunction
