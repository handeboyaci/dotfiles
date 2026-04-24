set errorformat=%-GStarting\ local\ Blaze%m
set errorformat+=%-GLoading:%m
set errorformat+=%-GAnalyzing:\ target\ %m
set errorformat+=%-G%\\s%#checking\ cached\ actions
set errorformat+=%-G[%[0-9\\,]%\\+\ /\ %[0-9\\,]%\\+]%m
" set errorformat+=%-GINFO:\ Analyzed\ target\ %m
set errorformat+=%-GINFO:\ Elapsed\ time:\ %m
set errorformat+=%-GINFO:\ Found\ %*\\d\ target...
set errorformat+=%-GERROR:\ %f:%l:%c:\ C++\ compilation\ of\ rule\ %m
set errorformat+=%-ATarget\ %m\ up-to-date:
set errorformat+=%C\ \ blaze
set errorformat+=%Z
set errorformat+=%tRROR:\ %f:%l:%c:\ %m
set errorformat+=%tARNING:\ %f:%l:%c:\ %m
set errorformat+=%tEBUG:\ %f:%l:%c:\ %m
set errorformat+=%Ethis\ rule\ is\ %m\ the\ following\ files\ included\ by\ '%f':
set errorformat+=%C\ \ '%m'
set errorformat+=FAIL:\ %m\ (see\ %f)
set errorformat+=%E%*[\ ]FAILED\ in%m
set errorformat+=%C\ \ %f
set errorformat+=%f:%l:%c:\ fatal\ %trror:\ %m
set errorformat+=%f:%l:%c:\ %trror:\ %m
set errorformat+=%f:%l:%c:\ %tarning:\ %m
set errorformat+=%f:%l:%c:\ note:\ %m
set errorformat+=%f:%l:%c:\ \ \ requi%tred\ from\ here
set errorformat+=%f(%l):\ %tarning:\ %m
set errorformat+=%f:%l:%c:\ %m
set errorformat+=%f:%l:\ %m

set makeprg=blaze\ build\ -c\ opt

command! -complete=file -nargs=* -bar -bang Build
      \ let s:args = <q-args> |
      \ if empty(s:args) | let s:args = expand('%') | endif |
      \ execute 'Make<bang> --compile_one_dependency ' . s:args


command! -nargs=* -complete=file -bang Test
      \ let s:args = <q-args> |
      \ if empty(s:args) | let s:args = expand('%') | endif |
      \ execute 'setl makeprg=blaze\ test\ $(/google/bin/releases/depserver-contrib-tools/affected_targets/affected_targets\ --test_only\ --depth=0\ ' . s:args . ')' |
      \ execute 'Make! ' |
      \ setl makeprg<
