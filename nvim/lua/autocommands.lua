-- Autocommands

local vimrc_group = vim.api.nvim_create_augroup('vimrc', { clear = true })

-- Reload config on save
vim.api.nvim_create_autocmd('BufWritePost', {
  group = vimrc_group,
  pattern = vim.env.MYVIMRC or '$MYVIMRC',
  command = 'source $MYVIMRC',
})

-- Toggle relative numbers in insert mode
vim.api.nvim_create_autocmd('InsertEnter', {
  group = vimrc_group,
  pattern = '*',
  callback = function()
    vim.opt.relativenumber = false
  end,
})

vim.api.nvim_create_autocmd('InsertLeave', {
  group = vimrc_group,
  pattern = '*',
  callback = function()
    vim.opt.relativenumber = true
  end,
})

-- FileType specific settings
vim.api.nvim_create_autocmd('FileType', {
  group = vimrc_group,
  pattern = { 'c', 'cpp' },
  callback = function()
    vim.opt.matchpairs:append('=:;')
  end,
})

-- Handle swap files
vim.api.nvim_create_autocmd('SwapExists', {
  group = vimrc_group,
  pattern = '*',
  callback = function()
    vim.v.swapchoice = 'o'
  end,
})
