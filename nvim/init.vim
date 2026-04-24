" vi:fdm=marker
" TODO list
" Options {{{
let g:loaded_tarPlugin = 1
let g:loaded_tar       = 1
let g:loaded_zipPlugin = 1
let g:loaded_zip       = 1
let g:loaded_gzip      = 1
let g:loaded_matchit   = 1
let g:loaded_matchparen   = 1
let g:loaded_spellfile_plugin = 1
let g:loaded_tutor_mode_plugin = 1

let g:loaded_node_provider = 0
let g:loaded_ruby_provider = 0
let g:loaded_perl_provider = 0
let g:loaded_python_provider = 0  " This is Python2. Python3 is separate.
let g:python3_host_prog = '/usr/bin/python3'
let g:no_python_maps = 1
let g:python_recommended_style = 0
let g:tex_flavor = 'latex'

if empty($TMUX) && executable('wl-copy')
  let g:clipboard = {
        \   'name': 'wl-clipboard',
        \   'copy': {
        \      '+': ['wl-copy', '--type', 'text/plain'],
        \      '*': ['wl-copy', '--type', 'text/plain', '--primary'],
        \    },
        \   'paste': {
        \      '+': ['wl-paste', '--type', 'text/plain'],
        \      '*': ['wl-paste', '--type', 'text/plain', '--primary'],
        \   },
        \ }
else
  let g:clipboard = {
        \   'name': 'tmux',
        \   'copy': {
        \      '+': ['tmux', 'load-buffer', '-w', '-'],
        \      '*': ['tmux', 'load-buffer', '-w', '-'],
        \    },
        \   'paste': {
         \      '+': ['tmux', 'save-buffer', '-'],
         \      '*': ['tmux', 'save-buffer', '-'],
        \   },
        \   'cache_enabled': 1,
        \ }
endif

set clipboard=unnamedplus

colo ayu
set termguicolors
set smoothscroll
set visualbell
set lazyredraw
set number
set relativenumber
set signcolumn=auto
set cursorline
set textwidth=80
set colorcolumn=+1
set nospell

set expandtab
set shiftround
set shiftwidth=2
set softtabstop=2
set tabstop=2

set scrolloff=3
set sidescrolloff=8
set whichwrap+=<,>,[,],~,h,l
set virtualedit=block,onemore
set nowrap
set linebreak
set breakindent
set showbreak=⌊

set gdefault
set ignorecase
set smartcase
set infercase
set list
set listchars=trail:·,eol:¬,tab:»-,extends:»,precedes:«,nbsp:¤
set grepprg=rg\ --vimgrep\ --smart-case
set grepformat=%f:%l:%c:\ %#%m,%f:%l:\ %#%m

set cpoptions+=J
set formatoptions=croqbj

set shell=/usr/bin/zsh
set modeline

set mouse=a
set mousehide

set undofile
set undodir=$HOME/.cache/vim/undo

set directory=$HOME/.cache/vim/swap
set updatetime=200
set nobackup

set shortmess+=c

set wildmode=longest:full,full
set wildignore=*.o,*~,*.sw*
set wildignorecase

set completeopt=longest,menuone

set showtabline=0
set showmatch
set noshowmode

set dictionary=/usr/share/dict/words

set switchbuf=useopen,usetab

set fillchars=vert:│,fold:·,diff:-
set foldtext=init#FoldText()
set foldmethod=expr
set foldexpr=nvim_treesitter#foldexpr()
set foldlevel=100

set matchpairs+=<:>
" }}}

" Maps {{{
" Set Space as Leader.
let g:mapleader = ' '
let g:maplocalleader = ' '

map <Space> <nop>

" Beginning / end of line.
inoremap        <C-A> <C-O>^
inoremap   <C-X><C-A> <C-A>
cnoremap        <C-A> <Home>
cnoremap   <C-X><C-A> <C-A>
inoremap        <C-E> <End>

" Redraw screen.
nnoremap ZL <C-L>

" Swap column / standard visual mode.
nnoremap v <C-v>
nnoremap <C-v> v
vnoremap v <C-v>
vnoremap <C-v> v

" Increment / decrement in visual mode.
vnoremap <C-a> <C-a>gv
vnoremap <C-x> <C-x>gv

" Change directory sensible.
nnoremap <silent> <Leader>. :lcd %:h<CR>
nnoremap <silent> <Leader>- :exe 'lcd '.fnamemodify(getcwd(), ':h')<CR>

" Unimpaired
nnoremap <silent><expr> cod &diff ?':diffoff<CR>':':diffthis<CR>'
nnoremap <silent> coh :set hlsearch!<CR>
nnoremap <silent> col :set cursorline!<CR>
nnoremap <silent> coi :set ignorecase! \| set ignorecase?<CR>
nnoremap <silent> con :set number!<CR>
nnoremap <silent> cor :set relativenumber!<CR>
nnoremap <silent> cos :set list!<CR>
nnoremap <silent> cou :set cursorcolumn!<CR>
nnoremap <silent> cow :set wrap! \| set wrap?<CR>
nnoremap <silent> ]x /^\s*\(>>>>\\|====\\|<<<<\)<CR>
nnoremap <silent> [x ?^\s*\(>>>>\\|====\\|<<<<\)<CR>
vnoremap <silent> ]x /^\s*\(>>>>\\|====\\|<<<<\)<CR>
vnoremap <silent> [x ?^\s*\(>>>>\\|====\\|<<<<\)<CR>
onoremap <silent> ]x /^\s*\(>>>>\\|====\\|<<<<\)<CR>
onoremap <silent> [x ?^\s*\(>>>>\\|====\\|<<<<\)<CR>
" }}}

" Autocommands {{{
augroup vimrc
  autocmd!
  autocmd BufWritePost $MYVIMRC source $MYVIMRC

  autocmd InsertEnter * set norelativenumber
  autocmd InsertLeave * set relativenumber

  autocmd FileType c,cpp set matchpairs+==:;

  autocmd SwapExists * let v:swapchoice='o'

  autocmd BufEnter * if winnr('$') == 1 && &buftype == 'quickfix' | quit | endif
augroup END  " }}}

" Commands {{{
" Show the changes
if !exists(':DiffOrig')
  command DiffOrig
        \   let g:diff_orig_filetype = &ft
        \ | vert new
        \ | set bt=nofile
        \ | 0r ++edit #
        \ | let &ft = g:diff_orig_filetype
        \ | unlet g:diff_orig_filetype
        \ | diffthis
        \ | wincmd p
        \ | diffthis
endif

if !exists(':Redir')
  command! -nargs=1 -complete=command -bar -range Redir
        \ silent call init#Redir(<q-args>, <range>, <line1>, <line2>)
endif

if !exists(':Color')
  command! Color
        \ packadd nvim-colorizer.lua |
        \ lua require('colorizer').setup() vim.cmd("e")
endif
" }}}
