-- Options and Globals

-- Globals to disable default plugins
vim.g.loaded_tarPlugin = 1
vim.g.loaded_tar = 1
vim.g.loaded_zipPlugin = 1
vim.g.loaded_zip = 1
vim.g.loaded_gzip = 1
vim.g.loaded_matchit = 1
vim.g.loaded_matchparen = 1
vim.g.loaded_spellfile_plugin = 1
vim.g.loaded_tutor_mode_plugin = 1

-- Provider settings
vim.g.loaded_node_provider = 0
vim.g.loaded_ruby_provider = 0
vim.g.loaded_perl_provider = 0
vim.g.loaded_python_provider = 0  -- This is Python2
vim.g.python3_host_prog = '/usr/bin/python3'
vim.g.no_python_maps = 1
vim.g.python_recommended_style = 0
vim.g.tex_flavor = 'latex'

-- Clipboard
if vim.fn.empty(vim.env.TMUX) == 1 and vim.fn.executable('wl-copy') == 1 then
  vim.g.clipboard = {
    name = 'wl-clipboard',
    copy = {
      ['+'] = {'wl-copy', '--type', 'text/plain'},
      ['*'] = {'wl-copy', '--type', 'text/plain', '--primary'},
    },
    paste = {
      ['+'] = {'wl-paste', '--type', 'text/plain'},
      ['*'] = {'wl-paste', '--type', 'text/plain', '--primary'},
    },
  }
else
  vim.g.clipboard = {
    name = 'tmux',
    copy = {
      ['+'] = {'tmux', 'load-buffer', '-w', '-'},
      ['*'] = {'tmux', 'load-buffer', '-w', '-'},
    },
    paste = {
      ['+'] = {'tmux', 'save-buffer', '-'},
      ['*'] = {'tmux', 'save-buffer', '-'},
    },
    cache_enabled = 1,
  }
end

vim.opt.clipboard = 'unnamedplus'

-- Colorscheme
vim.cmd('colo ayu')
vim.opt.termguicolors = true

-- UI
vim.opt.smoothscroll = true
vim.opt.visualbell = true
vim.opt.lazyredraw = true
vim.opt.number = true
vim.opt.relativenumber = true
vim.opt.signcolumn = 'auto'
vim.opt.cursorline = true
vim.opt.textwidth = 80
vim.opt.colorcolumn = '+1'
vim.opt.spell = false

-- Indentation
vim.opt.expandtab = true
vim.opt.shiftround = true
vim.opt.shiftwidth = 2
vim.opt.softtabstop = 2
vim.opt.tabstop = 2

-- Scrolling and wrapping
vim.opt.scrolloff = 3
vim.opt.sidescrolloff = 8
vim.opt.whichwrap:append('<,>,[,],~,h,l')
vim.opt.virtualedit = 'block,onemore'
vim.opt.wrap = false
vim.opt.linebreak = true
vim.opt.breakindent = true
vim.opt.showbreak = '⌊'

-- Search and display
vim.opt.gdefault = true
vim.opt.ignorecase = true
vim.opt.smartcase = true
vim.opt.infercase = true
vim.opt.list = true
vim.opt.listchars = { trail = '·', eol = '¬', tab = '»-', extends = '»', precedes = '«', nbsp = '¤' }
vim.opt.grepprg = 'rg --vimgrep --smart-case'
vim.opt.grepformat = '%f:%l:%c: %#%m,%f:%l: %#%m'

-- CPoptions and formatoptions
vim.opt.cpoptions:append('J')
vim.opt.formatoptions = 'croqbj'

-- Shell and modeline
vim.opt.shell = '/usr/bin/zsh'
vim.opt.modeline = true

-- Mouse
vim.opt.mouse = 'a'
vim.opt.mousehide = true

-- Undo and swap
vim.opt.undofile = true
vim.opt.undodir = vim.fn.expand('$HOME') .. '/.cache/vim/undo'
vim.opt.directory = vim.fn.expand('$HOME') .. '/.cache/vim/swap'
vim.opt.updatetime = 200
vim.opt.backup = false

-- Shortmess
vim.opt.shortmess:append('c')

-- Wildmenu
vim.opt.wildmode = 'longest:full,full'
vim.opt.wildignore = '*.o,*~,*.sw*'
vim.opt.wildignorecase = true

-- Completeopt
vim.opt.completeopt = 'longest,menuone'

-- Tabline and match
vim.opt.showtabline = 0
vim.opt.showmatch = true
vim.opt.showmode = false

-- Dictionary
vim.opt.dictionary = '/usr/share/dict/words'

-- Switchbuf
vim.opt.switchbuf = 'useopen,usetab'

-- Folds
vim.opt.fillchars = { vert = '│', fold = '·', diff = '-' }
vim.opt.foldtext = "v:lua.require('fold_text').foldtext()"
vim.opt.foldmethod = 'expr'
vim.opt.foldexpr = 'nvim_treesitter#foldexpr()'
vim.opt.foldlevel = 100

-- Matchpairs
vim.opt.matchpairs:append('<:>')
