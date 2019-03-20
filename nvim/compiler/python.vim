" Vim compiler file
" Compiler:    Python     
" Maintainer:   Christoph Herzog <ccf.herzog@gmx.net>
" Last Change:  2002 Nov 9

if exists("current_compiler")
  finish
endif

if exists(":CompilerSet") != 2    " older Vim always used :setlocal
  command -nargs=* CompilerSet setlocal <args>
endif

let current_compiler = "python"

let s:cpo_save = &cpo
set cpo-=C

CompilerSet makeprg=python\ %

"the last line: \%-G%.%# is meant to suppress some
"late error messages that I found could occur e.g.
"with wxPython and that prevent one from using :clast
"to go to the relevant file and line of the traceback.
CompilerSet errorformat=
  \%A\ \ File\ \"%f\"\\\,\ line\ %l\\\,%m,
  \%C\ \ \ \ %.%#,
  \%+Z%.%#Error\:\ %.%#,
  \%A\ \ File\ \"%f\"\\\,\ line\ %l,
  \%+C\ \ %.%#,
  \%-C%p^,
  \%Z%m,
  \%-G%.%#

let &cpo = s:cpo_save
unlet s:cpo_save
