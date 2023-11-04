function! google3#CppProtoHeader(fname) abort
  let l:root = fnamemodify(a:fname, ':r:r')
  let l:ext = fnamemodify(a:fname, ':e:e')
  if l:ext is# 'pb.h' || l:ext is# 'proto.h'
    return l:root . '.proto'
  endif

  return a:fname
endfunction


function! google3#GclFold() abort
  let l:start_num = nextnonblank(v:foldstart)
  let l:end_num = prevnonblank(v:foldend)

  if l:end_num <= l:start_num + 1
    " If the fold is empty, don't print anything for the contents.
    let l:content = ''
  else
    " Otherwise look for something matching the content regex.
    " And if nothing matches, print an ellipsis.
    let l:content = '...'
    let l:content_regex = '\m\C^\s*name = \zs.*\ze$'
    for l:line in getline(l:start_num + 1, l:end_num - 1)
      let l:content_match = matchstr(l:line, l:content_regex)
      if !empty(l:content_match)
        let l:content = l:content_match
        break
      endif
    endfor
  endif

  " Enclose content with start and end
  let l:start_text = getline(l:start_num)
  let l:end_text = substitute(getline(l:end_num), '^\s*', '', '')
  let l:text = l:start_text . ' ' . l:content . ' ' . l:end_text

  " Compute the available width for the displayed text.
  let l:width = winwidth(0) - &foldcolumn - (&number ? &numberwidth : 0)
  let l:lines_folded = ' ' . string(1 + v:foldend - v:foldstart) . ' lines'

  " Expand tabs, truncate, pad, and concatenate
  let l:text = substitute(l:text, '\t', repeat(' ', &tabstop), 'g')
  let l:text = strpart(l:text, 0, l:width - len(l:lines_folded))
  let l:padding = repeat(' ', l:width - len(l:lines_folded) - len(l:text))
  return l:text . l:padding . l:lines_folded
endfunction
