-- Autocommands

local vimrc_group = vim.api.nvim_create_augroup('vimrc', { clear = true })

-- Toggle relative numbers in insert mode (window-local)
vim.api.nvim_create_autocmd('InsertEnter', {
  group = vimrc_group,
  pattern = '*',
  callback = function()
    vim.wo.relativenumber = false
  end,
})

vim.api.nvim_create_autocmd('InsertLeave', {
  group = vimrc_group,
  pattern = '*',
  callback = function()
    if vim.wo.number then
      vim.wo.relativenumber = true
    end
  end,
})

-- Treesitter highlighting and folding for supported filetypes
vim.api.nvim_create_autocmd('FileType', {
  group = vimrc_group,
  callback = function(args)
    if pcall(vim.treesitter.start, args.buf) then
      vim.opt_local.foldmethod = 'expr'
      vim.opt_local.foldexpr = 'v:lua.vim.treesitter.foldexpr()'
    end
  end,
})

-- FileType specific settings
vim.api.nvim_create_autocmd('FileType', {
  group = vimrc_group,
  pattern = { 'c', 'cpp' },
  callback = function()
    vim.opt_local.matchpairs:append('=:;')
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

