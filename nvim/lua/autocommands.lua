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

-- Treesitter highlighting for supported filetypes
vim.api.nvim_create_autocmd('FileType', {
  group = vimrc_group,
  callback = function(args)
    local bufnr = args.buf
    -- Skip large files to prevent UI latency
    local max_size = 1.5 * 1024 * 1024 -- 1.5 MB
    local name = vim.api.nvim_buf_get_name(bufnr)
    if name ~= '' then
      local ok, stats = pcall(vim.uv.fs_stat, name)
      if ok and stats and stats.size > max_size then
        return
      end
    end
    if vim.api.nvim_buf_line_count(bufnr) > 50000 then
      return
    end

    local lang = vim.treesitter.language.get_lang(args.match) or args.match
    if vim.treesitter.query.get(lang, 'highlights') then
      pcall(vim.treesitter.start, bufnr, lang)
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

