call plug#begin('~/.config/nvim/bundle')

Plug 'airblade/vim-gitgutter'
Plug 'airblade/vim-rooter'
Plug 'AndrewRadev/sideways.vim'
Plug 'arecarn/crunch.vim'
Plug 'arecarn/selection.vim'  " required for crunch
Plug 'vim-utils/vim-vertical-move'
Plug 'chrisbra/Colorizer'
Plug 'christoomey/vim-tmux-navigator'
Plug 'gioele/vim-autoswap'
Plug 'kana/vim-textobj-user'
Plug 'reedes/vim-textobj-sentence'
Plug 'sencer/textobj-word-column.vim'
Plug 'idbrii/vim-endoscope'
Plug 'junegunn/fzf', {'dir': '~/.fzf', 'do': './install --all'}
Plug 'junegunn/fzf.vim'
Plug 'junegunn/goyo.vim'
Plug 'junegunn/limelight.vim'
Plug 'junegunn/rainbow_parentheses.vim'
Plug 'junegunn/vim-easy-align'
Plug 'KabbAmine/vCoolor.vim'
Plug 'kana/vim-niceblock'
Plug 'kana/vim-repeat'
Plug 'kshenoy/vim-signature'
Plug 'majutsushi/tagbar'
Plug 'mhinz/vim-hugefile'
Plug 'morhetz/gruvbox'
Plug 'purplep/python-syntax'
Plug 'romainl/vim-qf'
Plug 'romainl/vim-qlist'
Plug 'romainl/vim-cool'
Plug 'rstacruz/vim-closer'
Plug 'salsifis/vim-transpose'
Plug 'sencer/vim-snippets'
Plug 'sencer/vis'
Plug 'sencer/wordnet.vim'
Plug 'simnalamburt/vim-mundo'
Plug 'SirVer/UltiSnips'
Plug '/opt/vim-gnuplot'
Plug 'stefandtw/quickfix-reflector.vim'
Plug 'tmhedberg/SimpylFold'
Plug 'tommcdo/vim-exchange'
Plug 'tpope/vim-abolish'
Plug 'tpope/vim-commentary'
Plug 'tpope/vim-dispatch'
Plug 'tpope/vim-endwise'
Plug 'tpope/vim-eunuch'
Plug 'tpope/vim-fugitive'
Plug 'tpope/vim-rsi'
Plug 'tpope/vim-surround'
Plug 'tpope/vim-unimpaired'
Plug 'AdUki/vim-dispatch-neovim'
Plug 'w0rp/ale'
Plug 'roxma/nvim-yarp'
Plug 'ncm2/ncm2'
Plug 'ncm2/ncm2-bufword'
Plug 'ncm2/ncm2-path'
Plug 'ncm2/ncm2-ultisnips'
Plug 'fgrsnau/ncm2-otherbuf'
Plug 'yuki-ycino/ncm2-dictionary', {'branch': 'ncm2'}
Plug 'wellle/tmux-complete.vim'
Plug 'ludovicchabant/vim-gutentags'
Plug 'kassio/neoterm'

call plug#end()

set background=dark
set cursorline
colorscheme gruvbox
set termguicolors
if has('nvim')
  set guicursor=n-v-c:block-Cursor/lCursor-blinkon0,i-ci:ver25-Cursor/lCursor,r-cr:hor20-Cursor/lCursor
endif

if exists('$CURRENTSERVER')
  setlocal eventignore=BufEnter,VimEnter
endif

filetype plugin indent on
syntax on
runtime! macros/matchit.vim

" netrw
nnoremap cof :Vexplore!<CR>
let g:netrw_home = $HOME . '/.dotfiles/tmp'
let g:netrw_browsex_viewer = 'xdg-open'
let g:netrw_list_hide=netrw_gitignore#Hide().'.*\.sw.$,.*\.pyc$,.*~'
let g:netrw_liststyle = 3
let g:netrw_browse_split = 0
let g:netrw_preview = 1
let g:netrw_altv = 0
let g:netrw_winsize = 25

" huge file
let g:hugefile_trigger_size = 5

" ultisnips
let g:UltiSnipsJumpForwardTrigger = "<Tab>"
let g:UltiSnipsJumpBackwardTrigger = "<S-Tab>"
let g:UltiSnipsSnippetsDir = "~/.config/nvim/bundle/vim-snippets/UltiSnips"
let g:UltiSnipsEditSplit = "vertical"

" neoterm
nnoremap <F1> :Tnew<CR>
nmap gs <Plug>(neoterm-repl-send)
xmap gs <Plug>(neoterm-repl-send)
nmap gss <Plug>(neoterm-repl-send-line)
let g:neoterm_default_mod = ':vert rightbelow'
let g:neoterm_autoscroll = 1
let g:neoterm_direct_open_repl = 1
let g:neoterm_automap_keys = "<Nop>"

" ale
let g:ale_sign_column_always = 1
let g:ale_sign_error   = '⚑'
let g:ale_sign_warning = '⚠'

let g:ale_virtualtext_cursor = 1

let g:ale_lint_on_text_changed = 0
let g:ale_lint_on_insert_leave = 1

let g:ale_echo_msg_format = '%severity%: %s (%linter%)'

let g:ale_fixers = {
      \ '*': ['remove_trailing_lines', 'trim_whitespace'],
     \ }

inoremap <silent> <C-z> <C-\><C-o>:ALEComplete<CR>
nnoremap <F3> :ALEFix<CR>

" crunch
let g:crunch_result_type_append = 0
let g:crunch_user_variables = {
      \ 'e': 2.71828182846,
      \ 'pi': 3.14159265359,
      \ 'b2a': 0.529177249,
      \ 'ev': 13.6056923,
      \ }

" vcoolor
let g:vcoolor_disable_mappings = 1
let g:vcoolor_map = '<F5>'

" other
let g:CoolTotalMatches = 1
let g:tmux_navigator_disable_when_zoomed = 1
let g:surround_no_insert_mappings = 1
let g:rooter_change_directory_for_non_project_files = 'current'
let g:rooter_use_lcd = 1
let g:tcl_extended_syntax = 1
let g:tex_flavor = "latex"

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

nmap Y y$
noremap Q <C-L>
vnoremap <C-a> <C-a>gv
vnoremap <C-x> <C-x>gv
vnoremap . :norm.<CR>

map <Space> <nop>
map <Space> <Leader>

nnoremap v <C-v>
nnoremap <C-v> v
vnoremap v <C-v>
vnoremap <C-v> v

nnoremap <silent> <F9> :TagbarToggle<CR>
nnoremap <silent> cog :GitGutterToggle<CR>
nnoremap <silent> coe :ColorToggle<CR>
nnoremap <silent> coz :MundoToggle<CR>
nnoremap <silent> coo :RainbowParentheses!!<CR>

nmap cx <Plug>(Exchange)
nmap gl :SidewaysRight<CR>
nmap gh :SidewaysLeft<CR>
omap aa <Plug>SidewaysArgumentTextobjA
xmap aa <Plug>SidewaysArgumentTextobjA
omap ia <Plug>SidewaysArgumentTextobjI
xmap ia <Plug>SidewaysArgumentTextobjI

vmap <Enter> <Plug>(LiveEasyAlign)
nmap ga <Plug>(EasyAlign)

inoremap <C-U> <C-g>u<C-U>
inoremap ;; <Esc>g_a;
imap <A-=> <C-g>u<C-o>vBg=

vmap <Leader><Space> gc
nmap <Leader><Space> gcc

nnoremap <silent> <Leader>w :up!<CR>
nnoremap <silent> <Leader>x :x!<CR>
nnoremap <expr> <Leader>z winnr('$')==1?':tabclose<CR>':':tab split<CR>'
nnoremap <expr> <Leader>q len(filter(range(1, bufnr('$')), 'buflisted(v:val)'))==1?':q<CR>':':bw<CR>'

nnoremap <Leader>gc :Gcommit<CR>
nnoremap <Leader>ga :Gcommit --amend<CR>
nnoremap <Leader>gw :Gwrite<CR>
nnoremap <Leader>gs :Gstatus<CR>
nnoremap <Leader>gd :Gdiff<CR>

" inoremap <expr> <Cr>    pumvisible() ? "\<C-y>" : "\<Cr>"

command! -bang -nargs=* Rg
      \ call fzf#vim#grep(
      \   'rg --column --line-number --no-heading --color=always '.shellescape(<q-args>), 1,
      \   <bang>0 ? fzf#vim#with_preview('up:60%')
      \           : fzf#vim#with_preview('right:50%:hidden', '?'),
      \   <bang>0)

nnoremap <Leader>a :Rg <C-r>=expand("<cword>")<CR><CR>
nnoremap <Leader>A :Rg <C-r>=expand("<cWORD>")<CR><CR>
nnoremap <Leader>/ :Rg 
nnoremap <Leader>b :Buffers<CR>
nnoremap <Leader>c :BCommits<CR>
nnoremap <Leader>f :Files<CR>
nnoremap <Leader>l :Lines<CR>
nnoremap <Leader>j :History<CR>
nnoremap <Leader>m :Marks<CR>
nnoremap <Leader>s :Snippets<CR>
nnoremap <Leader>t :Tags<CR>
nnoremap <Leader>; :History:<CR>
nnoremap <Leader>: :Commands<CR>

if has('nvim')
  tnoremap <silent> <C-h> <C-\><C-n>:TmuxNavigateLeft<CR>
  tnoremap <silent> <C-j> <C-\><C-n>:TmuxNavigateDown<CR>
  tnoremap <silent> <C-k> <C-\><C-n>:TmuxNavigateUp<CR>
  tnoremap <silent> <C-l> <C-\><C-n>:TmuxNavigateRight<CR>
  tnoremap <silent> <C-w> <C-\><C-n><C-w>
  tnoremap <silent> <C-x> <C-\><C-n><C-w>c
endif

augroup RNU
  autocmd!
  autocmd RNU InsertEnter * set norelativenumber
  autocmd RNU InsertLeave * set relativenumber
augroup END

augroup vimrc
  autocmd!

  autocmd vimrc FileType awk setl commentstring=#\ %s
  autocmd vimrc FileType tcl setl foldmethod=syntax
  autocmd vimrc FileType vim setl keywordprg=:help
  autocmd vimrc FileType python
        \ compiler python

  autocmd vimrc FileType python,tex,latex,vim,tcl
        \ let b:closer = 1 |
        \ let b:closer_flags = '([{'

  autocmd vimrc BufWritePost $MYVIMRC source %

  autocmd vimrc BufEnter * call ncm2#enable_for_buffer()

  autocmd vimrc BufReadPost *
        \ if line("'\"") > 1 && line("'\"") <= line("$") |
        \ exe "normal! g`\"" |
        \ endif

  autocmd vimrc BufNewFile,BufReadPre /dev/shm/* setl noswapfile nobackup noundofile

  au vimrc FileType *
        \ if &omnifunc == "" |
        \   setlocal omnifunc=syntaxcomplete#Complete |
        \ endif

augroup END

if !exists(":DiffOrig")
  command DiffOrig vert new | set bt=nofile | r ++edit # | 0d_ | diffthis
        \ | wincmd p | diffthis
endif

" set status line
let s:norm = '#004466'
let s:ins  = '#446600'
let s:rep  = '#660044'

function! s:STLColor(mode)
  if a:mode == 'i'
    exec "hi statusline guifg=".s:ins
  elseif a:mode == 'r'
    exec "hi statusline guifg=".s:rep
  else
    exec "hi statusline guifg=".s:norm
  endif
endfunction
exec "hi statusline guifg=".s:norm

augroup STL
  autocmd!
  autocmd STL InsertEnter  * call <SID>STLColor(v:insertmode)
  autocmd STL InsertChange * call <SID>STLColor(v:insertmode)
  autocmd STL InsertLeave  * exec "hi statusline guifg=".s:norm
augroup END

function! LinterStatus() abort
  let l:counts = ale#statusline#Count(bufnr(''))

  let l:all_errors = l:counts.error + l:counts.style_error
  let l:all_non_errors = l:counts.total - l:all_errors

  return l:counts.total == 0 ? '' : printf(
        \   '%dW %dE |',
        \   all_non_errors,
        \   all_errors
        \)
endfunction

let &stl='%n [%{substitute(getcwd(), $HOME, "~", "")}] %f%( [%M%R%H]%)%='
let &stl.='%{LinterStatus()} %{tagbar#currenttag("%s | ", "", "f")}'
let &stl.='%P, col %c %y%q'
