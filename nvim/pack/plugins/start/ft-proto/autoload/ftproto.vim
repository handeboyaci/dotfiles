let s:fold_content_regexes = {
  \ 'textpb': '\m\C^\s*\(name\|language\|type\|label\|description\): \zs.*\ze$',
  \ }

""
" @private
" Build text to be shown when a fold is collapsed.
" It basically concatenates three things:
"   - the first line of the fold
"   - the "content" of the fold
"   - the last line of the fold
"
" The content is decided via a regex, which for most languages boils down to
" finding a line defining the "name" of the region, and showing the name.
"
" Then finally, the text is truncated so that the number of folded lines can be
" shown at the end of the fold.
function! ftproto#FoldText() abort
  let l:start_num = nextnonblank(v:foldstart)
  let l:end_num = prevnonblank(v:foldend)

  if l:end_num <= l:start_num + 1
    " If the fold is empty, don't print anything for the contents.
    let l:content = ''
  else
    " Otherwise look for something matching the content regex.
    " And if nothing matches, print an ellipsis.
    let l:content = '...'
    if has_key(s:fold_content_regexes, &filetype)
      let l:content_regex = s:fold_content_regexes[&filetype]
      for l:line in getline(l:start_num + 1, l:end_num - 1)
        let l:content_match = matchstr(l:line, l:content_regex)
        if !empty(l:content_match)
          let l:content = l:content_match
          break
        endif
      endfor
    endif
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

function! ftproto#ProtoTextfmtGetIndent(lnum) abort
  " Find the first non-blank, non-comment line above the current line.
  let lnum = a:lnum
  while lnum > 0
    let lnum = prevnonblank(lnum - 1)
    let prev_line = getline(lnum)
    if prev_line !~# '^\s*#'
      break
    endif
  endwhile

  " Use a zero indent at the start of the file.
  if lnum == 0
    return 0
  endif

  let this_line = getline(a:lnum)
  let ind = indent(lnum)

  " Indent blocks enclosed by <> or {}.
  " Find a real opening brace
  let bracepos = match(prev_line, '[<>{}]', matchend(prev_line, '^\s*[>}]'))
  " Vim 7.3.693 and later defines a shiftwidth() function to get the effective
  " shiftwidth value. Fall back to &shiftwidth if the function doesn't exist.
  let l:shiftwidth = exists('*shiftwidth') ? shiftwidth() : &shiftwidth
  while bracepos != -1
    let brace = strpart(prev_line, bracepos, 1)
    if brace ==# '<' || brace ==# '{'
      let ind = ind + l:shiftwidth
    else
      let ind = ind - l:shiftwidth
    endif
    let bracepos = match(prev_line, '[<>{}]', bracepos + 1)
  endwhile
  let bracepos = matchend(this_line, '^\s*[>}]')
  if bracepos != -1
    let ind = ind - l:shiftwidth
  endif

  return ind
endfunction
