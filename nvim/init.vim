" Plugin {{{
call plug#begin('~/.config/nvim/bundle')

Plug 'ayu-theme/ayu-vim'
Plug 'tpope/vim-surround'
Plug 'tpope/vim-unimpaired'
Plug 'tpope/vim-eunuch'
Plug 'tpope/vim-commentary'
Plug 'tpope/vim-rsi'
Plug 'tpope/vim-endwise'
Plug 'tpope/vim-repeat'
Plug 'rstacruz/vim-closer'
Plug 'vim-utils/vim-vertical-move'
Plug 'kana/vim-niceblock'
Plug 'romainl/vim-qf'
Plug 'romainl/vim-qlist'
Plug 'romainl/vim-cool'
Plug 'tommcdo/vim-exchange'
Plug 'AndrewRadev/sideways.vim'
Plug 'stefandtw/quickfix-reflector.vim'
Plug 'junegunn/rainbow_parentheses.vim'
Plug 'kshenoy/vim-signature'
Plug 'salsifis/vim-transpose'

Plug 'lifepillar/vim-mucomplete'
Plug 'neovim/nvim-lsp'
Plug 'wellle/tmux-complete.vim'

Plug 'bfrg/vim-cpp-modern'
Plug 'vim-python/python-syntax'
Plug 'tmhedberg/SimpylFold'

Plug 'christoomey/vim-tmux-navigator'
Plug 'tpope/vim-dispatch'
Plug 'aduki/vim-dispatch-neovim'
Plug 'junegunn/fzf', {'dir': '~/.fzf', 'do': './install --all'}
Plug 'junegunn/fzf.vim'
Plug 'majutsushi/tagbar'
Plug 'mhinz/vim-signify'
Plug 'tpope/vim-fugitive'

Plug 'RRethy/vim-hexokinase', {'do': 'make hexokinase'}
Plug 'junegunn/vim-easy-align'
Plug 'simnalamburt/vim-mundo'
Plug 'enomsg/vim-haskellConcealPlus'
call plug#end()
" }}}
" Visuals {{{
set termguicolors
if has('nvim')
  set guicursor=
        \n-v-c:block-Cursor/lCursor-blinkon0,
        \i-ci:ver25-Cursor/lCursor,
        \r-cr:hor20-Cursor/lCursor
endif
set background=dark
colorscheme ayu
" }}}

" Basic VIM {{{
filetype plugin indent on
syntax on
runtime! macros/matchit.vim
" }}}

" neovim python {{{
let g:python3_host_prog = "$HOME/.local/share/conda/bin/python"
let g:python_host_prog = "/usr/bin/python2.7"
let g:python_highlight_all = 1
" }}}

" μComplete {{{
let g:mucomplete#chains = {
      \ 'default': ['path', 'omni', 'user', 'c-n', 'uspl']
      \ }
let g:mucomplete#enable_auto_at_startup = 1
" }}}
"

" Netrw {{{
nnoremap cof :Vexplore!<CR>
let g:netrw_home = $HOME . '/.dotfiles/tmp'
let g:netrw_browsex_viewer = 'xdg-open'
let g:netrw_list_hide=netrw_gitignore#Hide().'.*\.sw.$,.*\.pyc$,.*~'
let g:netrw_liststyle = 3
let g:netrw_browse_split = 0
let g:netrw_preview = 1
let g:netrw_altv = 0
let g:netrw_winsize = 25
""" }}}

" Signify {{{
highlight SignifySignAdd    guibg=#50FF50
highlight SignifySignDelete guibg=#FF5050
highlight SignifySignChange guibg=#FFAA33
let g:signify_sign_show_text = 0
" }}}

" Other Plug-ins {{{
let g:CoolTotalMatches = 1
let g:tmux_navigator_disable_when_zoomed = 1
let g:surround_no_insert_mappings = 1
let g:Hexokinase_highlighters = ["backgroundfull"]
" }}}

" VIM settings {{{
set cursorline
set whichwrap+=<,>,[,],~,h,l

set scrolloff=3
set sidescrolloff=8
set clipboard^=unnamedplus

set cpoptions+=J

set shell=/bin/zsh

set expandtab
set tabstop=2
set softtabstop=2
set shiftwidth=2
set shiftround

set gdefault
set ignorecase
set smartcase

set modeline

set virtualedit=block,onemore

set number
set relativenumber

set nowrap

set list
set listchars=trail:·,eol:¬,tab:»-,extends:❯,precedes:❮,nbsp:∴
set showbreak=…

set undofile
set undodir=~/.dotfiles/tmp/undodir
set undolevels=1000
set undoreload=1000

set directory=~/.dotfiles/tmp/swap
set updatetime=200

set colorcolumn=81
set showtabline=1
set wildmode=longest:full,full
set showmatch
set showcmd
set noshowmode
set noerrorbells
set visualbell
set lazyredraw

set mouse=a
set mousemodel=popup_setpos

set infercase
set wildignore=*.o,*~,*.sw*
set wildignorecase
set thesaurus+=~/.config/nvim/dictionaries/moby
set dictionary+=/usr/share/dict/words
set completeopt=noinsert,menuone,noselect

set autoread
set switchbuf=useopen,usetab
set hidden

set autoindent
set smartindent
set copyindent
set cindent
set cinoptions+=l1

set foldlevel=1

set grepprg=rg\ --vimgrep

set shortmess+=c
" }}}

" Maps {{{
" Base mappings {{{
nnoremap : ;
nnoremap ; :
nnoremap q; q:
vnoremap : ;
vnoremap ; :
map <Space> <nop>
map <Space> <Leader>

nmap Y y$
noremap Q <C-L>
vnoremap <C-a> <C-a>gv
vnoremap <C-x> <C-x>gv
vnoremap . :norm.<CR>

nnoremap v <C-v>
nnoremap <C-v> v
vnoremap v <C-v>
vnoremap <C-v> v

inoremap <C-U> <C-g>u<C-U>

nnoremap <silent> <Leader>w :up!<CR>
nnoremap <silent> <Leader>x :x!<CR>

" zoom current window
nnoremap <expr> <Leader>z winnr('$')==1?':tabclose<CR>':':tab split<CR>'

" close buffer, and close vim on last buffer
nnoremap <expr> <Leader>q len(filter(range(1, bufnr('$')),
      \ 'buflisted(v:val)')) == 1 ? ':q<CR>' : ':bw<CR>'

nnoremap <Leader>o <C-w>o
" }}}

" Commentary {{{
vmap <Leader><Space> gc
nmap <Leader><Space> gcc
" }}}

" Terminal {{{
if has('nvim')
  tnoremap <silent> <C-h> <C-\><C-n>:TmuxNavigateLeft<CR>
  tnoremap <silent> <C-j> <C-\><C-n>:TmuxNavigateDown<CR>
  tnoremap <silent> <C-k> <C-\><C-n>:TmuxNavigateUp<CR>
  tnoremap <silent> <C-l> <C-\><C-n>:TmuxNavigateRight<CR>
  tnoremap <silent> <C-w> <C-\><C-n><C-w>
  tnoremap <silent> <C-x> <C-\><C-n><C-w>c
endif
" }}}

" Sideways {{{
nmap gl :SidewaysRight<CR>
nmap gh :SidewaysLeft<CR>
omap aa <Plug>SidewaysArgumentTextobjA
xmap aa <Plug>SidewaysArgumentTextobjA
omap ia <Plug>SidewaysArgumentTextobjI
xmap ia <Plug>SidewaysArgumentTextobjI
" }}}

" Tools {{{
nmap cx <Plug>(Exchange)
nnoremap <silent> <F9> :TagbarToggle<CR>
nnoremap <silent> coz :MundoToggle<CR>
nnoremap <silent> cog :SignifyToggle<CR>
nnoremap <silent> coo :RainbowParentheses!!<CR>
nnoremap <silent> coe :HexokinaseToggle<CR>
" }}}

" Fugitive {{{
nnoremap <Leader>gc :Gcommit<CR>
nnoremap <Leader>ga :Gcommit --amend<CR>
nnoremap <Leader>gw :Gwrite<CR>
nnoremap <Leader>gs :Gstatus<CR>
nnoremap <Leader>gd :Gdiff<CR>
" }}}

" }}}

" Autocommands {{{
augroup RNU
  autocmd!
  autocmd InsertEnter * set norelativenumber
  autocmd InsertLeave * set relativenumber
augroup END

augroup vimrc
  autocmd!
  autocmd CmdwinEnter  * nnoremap <buffer> <Leader>q :q<CR>
  autocmd FileType awk setl commentstring=#\ %s
  autocmd FileType tcl setl foldmethod=syntax
  autocmd FileType vim setl keywordprg=:help
  autocmd FileType python
        \ compiler python

  autocmd FileType python,tex,latex,vim,tcl
        \ let b:closer = 1 |
        \ let b:closer_flags = '([{'

  autocmd BufWritePost $MYVIMRC source %

  autocmd BufReadPost *
        \ if line("'\"") > 1 && line("'\"") <= line("$") |
        \ exe "normal! g`\"" |
        \ endif

  autocmd BufNewFile,BufReadPre /dev/shm/*
        \ setl noswapfile nobackup noundofile

  autocmd FileType *
        \ if &omnifunc == "" |
        \   setlocal omnifunc=syntaxcomplete#Complete |
        \ endif
augroup END
" }}}

" Commands {{{
if !exists(":DiffOrig")
  command DiffOrig vert new | set bt=nofile | r ++edit # | 0d_ | diffthis
        \ | wincmd p | diffthis
endif

if !exists(":Redir")
command! -nargs=1 -complete=command -bar -range Redir 
      \ silent call redir#Redir(<q-args>, <range>, <line1>, <line2>)
endif
" }}}

" StatusLine {{{
function! s:STLColor(mode)
  let l:fg   = '#cfcfcf'
  if a:mode == 'i'
    exec "hi statusline guifg=".l:fg." guibg='#446600'"
  elseif a:mode == 'r'
    exec "hi statusline guifg=".l:fg." guibg='#660044'"
  else
    exec "hi statusline guifg=".l:fg." guibg='#004466'"
  endif
endfunction
call s:STLColor('n')

augroup STL
  autocmd!
  autocmd STL InsertEnter  * call <SID>STLColor(v:insertmode)
  autocmd STL InsertChange * call <SID>STLColor(v:insertmode)
  autocmd STL InsertLeave  * call <SID>STLColor('n')
augroup END

function! LspStatus() abort
  let l:errors = luaeval('vim.lsp.util.buf_diagnostics_count("Error")')
  let l:warnings = luaeval('vim.lsp.util.buf_diagnostics_count("Warning")')

  return l:errors == 0 && l:warnings == 0 ? '' : printf(
        \   '%dW %dE |',
        \   l:warnings,
        \   l:errors
        \)
endfunction

let &stl='%n [%{substitute(getcwd(), $HOME, "~", "")}] %f%( [%M%R%H]%)%='
let &stl.='%{LspStatus()} %{tagbar#currenttag("%s | ", "", "f")}'
let &stl.='%P, col %c %y%q'
" }}}

" FZF settings {{{
if !exists("*RgWithMotion")
  let s:rg_command = join(['rg', '--column', '--no-heading', '--line-number',
        \ '--color=never', '--smart-case', '%s', '||', 'true'], " ")

  let s:preview_command = '$HOME/.dotfiles/nvim/bundle/fzf.vim/bin/preview.sh {}'
  let s:opts = {
        \ 'options': [
        \   '--no-border',
        \   '--preview-window', 'right',
        \   '--preview', s:preview_command
        \ ]}

  " Function definitions {{{
  function! GrepWithMotion(cmd, has_col, type, ...)
    let reg_save = @@
    if a:type ==# 'char'
      normal! `[v`]y
    else
      normal! gvy
    endif
    let l:val = @@
    let @@ = reg_save
    let @/ = l:val
    set hlsearch
    call fzf#vim#grep(printf(a:cmd, l:val), a:has_col,
          \           {'options': ['--no-border']})
  endfunction

  function! RgWithMotion(type, ...)
    return GrepWithMotion(s:rg_command, 1, a:type, a:000)
  endfunction

  " }}}

  command! -bang -nargs=? -complete=dir Files
        \ call fzf#vim#files(<q-args>, s:opts, <bang>0)
  command! -bang History call fzf#vim#history(s:opts, <bang>0)
  command! -bang Buffers call fzf#vim#buffers(s:opts, <bang>0)

  nnoremap <silent> <Leader>f :Files<CR>
  nnoremap <silent> <Leader>h :History<CR>
  nnoremap <silent> <Leader>b :Buffers<CR>
  nnoremap <silent> <Leader>/ :set opfunc=RgWithMotion<CR>g@
  xnoremap <silent> <Leader>/ :<C-u>call RgWithMotion(visualmode())<CR>
  nnoremap <silent> <Leader>// :Rg<Space>
endif


augroup FZF
  autocmd! FileType fzf
  autocmd  FileType fzf set laststatus=0 noshowmode noruler nonu nornu
        \| autocmd BufLeave <buffer> set laststatus=2 showmode ruler nu rnu

augroup END
" }}}

" LSP settings {{{
luafile $HOME/.dotfiles/nvim/default.lua
" }}}
