if exists('syntax_on')
  syntax reset
endif

let g:colors_name='ayu'

set background=dark

hi!  Normal             guifg=#E6E1CF  guibg=#0F1419  gui=NONE

hi!  Comment            guifg=#5C6773  guibg=NONE     gui=NONE
hi!  Function           guifg=#FFB454  guibg=NONE     gui=NONE
hi!  PreProc            guifg=#E6B673  guibg=NONE     gui=NONE
hi!  Special            guifg=#E6B673  guibg=NONE     gui=NONE
hi!  SpecialKey         guifg=#253340  guibg=NONE     gui=NONE
hi!  Statement          guifg=#FF7733  guibg=NONE     gui=NONE
hi!  Type               guifg=#36A3D9  guibg=NONE     gui=NONE

hi!  Boolean            guifg=#B8CC52  guibg=NONE     gui=NONE
hi!  Constant           guifg=#FFEE99  guibg=NONE     gui=NONE
hi!  Identifier         guifg=#36A3D9  guibg=NONE     gui=NONE
hi!  String             guifg=#B8CC52  guibg=NONE     gui=NONE

hi!  SpecialComment     guifg=#FF7733  guibg=NONE     gui=NONE
hi!  Title              guifg=#FF7733  guibg=NONE     gui=BOLD

hi!  ColorColumn        guifg=NONE     guibg=#252A2E  gui=NONE
hi!  Conceal            guifg=#2D3640  guibg=NONE     gui=NONE
hi!  FoldColumn         guibg=NONE     guifg=#7fc1ca   gui=NONE
hi!  Folded             guibg=NONE     guifg=#7fc1ca   gui=NONE
hi!  LineNr             guifg=#4D5660  guibg=NONE     gui=NONE
hi!  MatchParen         guifg=#E6E1CF  guibg=#0F1419  gui=NONE,underline
hi!  SignColumn         guifg=NONE     guibg=#14191F  gui=NONE
hi!  SpecialKey         guifg=#253340  guibg=NONE     gui=NONE

hi!  Directory          guifg=#3E4B59  guibg=NONE     gui=NONE
hi!  Underlined         guifg=#36A3D9  guibg=NONE     gui=NONE,underline

hi!  CurSearch          guifg=#0F1419  guibg=#FFAAAA  gui=NONE
hi!  IncSearch          guifg=#0F1419  guibg=#FFAAAA  gui=NONE
hi!  Search             guifg=#0F1419  guibg=#FFEE99  gui=NONE
hi!  Visual             guifg=#2c3339  guibg=#7fc1ca  gui=NONE

hi!  ModeMsg            guifg=#B8CC52  guibg=NONE     gui=NONE
hi!  StatusLineNC       guifg=#3E4B59  guibg=#14191F  gui=NONE
hi!  VertSplit          guifg=#0F1419  guibg=NONE     gui=NONE
hi!  WildMenu           guifg=#0F1419  guibg=#F07178  gui=NONE

hi!  DiffAdd            guifg=NONE     guibg=#204020  gui=NONE
hi!  DiffChange         guifg=NONE     guibg=#202090  gui=NONE
hi!  DiffDelete         guifg=#db6c6c  guibg=#db6c6c  gui=NONE
hi!  DiffText           guifg=#0F1419  guibg=#8080FF  gui=NONE

hi!  Pmenu              guifg=#E6E1CF  guibg=#253340  gui=NONE
hi!  PmenuSbar          guifg=#0F1419  guibg=#0F1419  gui=NONE
hi!  PmenuSel           guifg=#E6E1CF  guibg=#253340  gui=NONE,reverse
hi!  PmenuThumb         guifg=#0F1419  guibg=#0F1419  gui=NONE

hi!  SpellBad           guifg=#FF3333  guibg=NONE     gui=NONE,underline
hi!  SpellCap           guifg=#36A3D9  guibg=NONE     gui=NONE,underline
hi!  SpellLocal         guifg=#FF7733  guibg=NONE     gui=NONE,underline
hi!  SpellRare          guifg=#95E6CB  guibg=NONE     gui=NONE,underline

hi!  ErrorMsg           guifg=#E6E1CF  guibg=#FF3333  gui=NONE,standout
hi!  MoreMsg            guifg=#B8CC52  guibg=NONE     gui=NONE
hi!  Question           guifg=#B8CC52  guibg=NONE     gui=NONE
hi!  WarningMsg         guifg=#FF3333  guibg=NONE     gui=NONE

" ERR TabLineFill
" ERR TabLineSel
hi!  TabLine            guifg=#E6E1CF  guibg=#14191F  gui=NONE,reverse

hi!  Error              guifg=#E6E1CF  guibg=#FF3333  gui=NONE
hi!  ErrorMsg           guifg=#E6E1CF  guibg=#FF3333  gui=NONE,standout
hi!  Ignore             guifg=NONE     guibg=NONE     gui=NONE
hi!  Todo               guifg=#F07178  guibg=NONE     gui=NONE

hi!  NonText            guifg=#2D3640  guibg=NONE     gui=NONE

hi!  CursorColumn       guifg=NONE     guibg=#252A2E  gui=NONE
hi!  CursorLineConceal  guifg=#2D3640  guibg=#252A2E  gui=NONE
hi!  CursorLine         guifg=NONE     guibg=#252A2E  gui=NONE
hi!  CursorLineNr       guifg=#F29718  guibg=#252A2E  gui=NONE

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
hi link QuickFixLine       Visual
hi link Repeat             Statement
hi link SpecialChar        Special
hi link SpecialComment     Special
hi link StorageClass       Type
hi link Structure          Type
hi link Tag                Special
hi link Typedef            Type
hi link qfLineNr           LineNr
hi link qfFileName         Normal
