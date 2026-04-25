-- Mappings

-- Set Leader
vim.g.mapleader = ' '
vim.g.maplocalleader = ' '

-- Disable Space in normal mode
vim.keymap.set('', '<Space>', '<nop>')

-- Beginning / end of line
vim.keymap.set('i', '<C-A>', '<C-O>^')
vim.keymap.set('i', '<C-X><C-A>', '<C-A>')
vim.keymap.set('c', '<C-A>', '<Home>')
vim.keymap.set('c', '<C-X><C-A>', '<C-A>')
vim.keymap.set('i', '<C-E>', '<End>')

-- Redraw screen
vim.keymap.set('n', 'ZL', '<C-L>')

-- Swap column / standard visual mode
vim.keymap.set('n', 'v', '<C-v>')
vim.keymap.set('n', '<C-v>', 'v')
vim.keymap.set('v', 'v', '<C-v>')
vim.keymap.set('v', '<C-v>', 'v')

-- Increment / decrement in visual mode
vim.keymap.set('v', '<C-a>', '<C-a>gv')
vim.keymap.set('v', '<C-x>', '<C-x>gv')

-- Change directory sensible
vim.keymap.set('n', '<Leader>.', ':lcd %:h<CR>', { silent = true })
vim.keymap.set('n', '<Leader>-', function()
  vim.cmd('lcd ' .. vim.fn.fnamemodify(vim.fn.getcwd(), ':h'))
end, { silent = true })

-- Unimpaired style toggles
vim.keymap.set('n', 'cod', function()
  return vim.wo.diff and ':diffoff<CR>' or ':diffthis<CR>'
end, { expr = true, silent = true })

vim.keymap.set('n', 'coh', ':set hlsearch!<CR>', { silent = true })
vim.keymap.set('n', 'col', ':set cursorline!<CR>', { silent = true })
vim.keymap.set('n', 'coi', ':set ignorecase! | set ignorecase?<CR>', { silent = true })
vim.keymap.set('n', 'con', ':set number!<CR>', { silent = true })
vim.keymap.set('n', 'cor', ':set relativenumber!<CR>', { silent = true })
vim.keymap.set('n', 'cos', ':set list!<CR>', { silent = true })
vim.keymap.set('n', 'cou', ':set cursorcolumn!<CR>', { silent = true })
vim.keymap.set('n', 'cow', ':set wrap! | set wrap?<CR>', { silent = true })

-- Conflict marker jumps
local conflict_forward = [[/^\s*\(>>>>\|====\|<<<<\)<CR>]]
local conflict_backward = [[?^\s*\(>>>>\|====\|<<<<\)<CR>]]

vim.keymap.set({'n', 'v', 'o'}, ']x', conflict_forward, { silent = true })
vim.keymap.set({'n', 'v', 'o'}, '[x', conflict_backward, { silent = true })
