" VIM COLOR SCHEME
" Maintainer:   Karolis Koncevicius
" Inspirations: nova, zenburn

hi clear

if exists('syntax_on')
  syntax reset
endif

let g:colors_name='sacredforest'

set background=dark

hi Normal            guibg=#2c3339 guifg=#e0d7c3   gui=NONE

hi Comment           guibg=NONE    guifg=#616c72   gui=NONE
hi Function          guibg=NONE    guifg=#b2a488   gui=NONE
hi PreProc           guibg=NONE    guifg=#b2a488   gui=NONE
hi Special           guibg=NONE    guifg=#b2a488   gui=NONE
hi Statement         guibg=NONE    guifg=#b2a488   gui=NONE
hi Type              guibg=NONE    guifg=#b2a488   gui=NONE

hi Boolean           guibg=NONE    guifg=#94b380   gui=NONE
hi Constant          guibg=NONE    guifg=#94b380   gui=NONE
hi Identifier        guibg=NONE    guifg=#94b380   gui=NONE
hi String            guibg=NONE    guifg=#94b380   gui=NONE

hi SpecialComment    guibg=NONE    guifg=#c5d4dd   gui=NONE
hi Title             guibg=NONE    guifg=#c5d4dd   gui=BOLD

hi ColorColumn       guibg=#3c4349 guifg=NONE      gui=NONE
hi Conceal           guibg=NONE    guifg=#616c72   gui=NONE
hi CursorLineNr      guibg=NONE    guifg=#ddd668   gui=NONE
hi FoldColumn        guibg=NONE    guifg=#7fc1ca   gui=NONE
hi Folded            guibg=NONE    guifg=#7fc1ca   gui=NONE
hi LineNr            guibg=NONE    guifg=#616c72   gui=NONE
hi MatchParen        guibg=NONE    guifg=#7fc1ca   gui=NONE
hi SignColumn        guibg=#616c72 guifg=NONE      gui=NONE
hi SpecialKey        guibg=NONE    guifg=#616c72   gui=NONE

hi Directory         guibg=NONE    guifg=#94b380   gui=NONE
hi Underlined        guibg=NONE    guifg=NONE      gui=UNDERLINE

hi CurSearch         guibg=#ffbf00 guifg=#2c3339   gui=NONE
hi IncSearch         guibg=#ffbf00 guifg=#2c3339   gui=NONE
hi Search            guibg=#ddd668 guifg=#2c3339   gui=NONE
hi Visual            guibg=#7fc1ca guifg=#2c3339   gui=NONE
hi VisualNOS         guibg=NONE    guifg=NONE      gui=UNDERLINE

hi ModeMsg           guibg=NONE    guifg=#7fc1ca   gui=NONE
hi StatusLineNC      guibg=#616c72 guifg=#e0d7c3   gui=NONE
hi VertSplit         guibg=NONE    guifg=#616c72   gui=NONE
hi WildMenu          guibg=#e0d7c3 guifg=#616c72   gui=NONE

hi DiffAdd           guibg=#8eaf6b guifg=#2c3339   gui=NONE
hi DiffChange        guibg=#2c3339 guifg=#ffbf00   gui=UNDERLINE
hi DiffDelete        guibg=#db6c6c guifg=#2c3339   gui=NONE
hi DiffText          guibg=#ffbf00 guifg=#2c3339   gui=NONE

hi Pmenu             guibg=#616c72 guifg=#e0d7c3   gui=NONE
hi PmenuSbar         guibg=#616c72 guifg=NONE      gui=NONE
hi PmenuSel          guibg=#8eaf6b guifg=#2c3339   gui=NONE
hi PmenuThumb        guibg=#e0d7c3 guifg=NONE      gui=NONE

hi SpellBad          guibg=NONE    guifg=NONE      gui=UNDERCURL
hi SpellCap          guibg=NONE    guifg=NONE      gui=UNDERCURL
hi SpellLocal        guibg=NONE    guifg=NONE      gui=UNDERCURL
hi SpellRare         guibg=NONE    guifg=NONE      gui=UNDERCURL

hi ErrorMsg          guibg=#db6c6c guifg=#616c72   gui=NONE
hi MoreMsg           guibg=NONE    guifg=#7fc1ca   gui=NONE
hi Question          guibg=NONE    guifg=#7fc1ca   gui=NONE
hi WarningMsg        guibg=NONE    guifg=#db6c6c   gui=NONE

hi TabLine           guibg=#616c72 guifg=#e0d7c3   gui=NONE
hi TabLineFill       guibg=#616c72 guifg=#e0d7c3   gui=NONE
hi TabLineSel        guibg=#616c72 guifg=#e0d7c3   gui=REVERSE

hi Error             guibg=NONE    guifg=#db6c6c   gui=REVERSE
hi Ignore            guibg=NONE    guifg=NONE      gui=NONE
hi Todo              guibg=#e0d7c3 guifg=#616c72   gui=NONE

hi NonText           guibg=NONE    guifg=#616c72   gui=NONE

hi CursorColumn      guibg=#4c5866 guifg=NONE      gui=NONE
hi Cursor            guibg=#e0d7c3 guifg=#2c3339   gui=NONE
hi CursorLine        guibg=#4c5866 guifg=NONE      gui=NONE

hi helpleadblank     guibg=NONE    guifg=NONE      gui=NONE
hi helpnormal        guibg=NONE    guifg=NONE      gui=NONE

hi link Number             Constant
hi link Character          Constant

hi link Conditional        Statement
hi link Debug              Special
hi link Define             PreProc
hi link Delimiter          Special
hi link Exception          Statement
hi link Float              Number
hi link HelpCommand        Statement
hi link HelpExample        Statement
hi link Include            PreProc
hi link Keyword            Statement
hi link Label              Statement
hi link Macro              PreProc
hi link Operator           Statement
hi link PreCondit          PreProc
hi link Repeat             Statement
hi link SpecialChar        Special
hi link SpecialComment     Special
hi link StorageClass       Type
hi link Structure          Type
hi link Tag                Special
hi link Typedef            Type

hi link QuickFixLine       Visual
