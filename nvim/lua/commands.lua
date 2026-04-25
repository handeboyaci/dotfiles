-- Custom Commands

-- DiffOrig: Show diff against saved file
vim.api.nvim_create_user_command('DiffOrig', [[
  let g:diff_orig_filetype = &ft
  vert new
  set bt=nofile
  0r ++edit #
  let &ft = g:diff_orig_filetype
  unlet g:diff_orig_filetype
  diffthis
  wincmd p
  diffthis
]], {})

-- Redir: Redirect command output
vim.api.nvim_create_user_command('Redir', function(opts)
  local status, redir = pcall(require, 'redir')
  if status then
    redir.redir(opts.args, opts.range, opts.line1, opts.line2)
  else
    print("Error: 'redir' module not found.")
  end
end, { nargs = 1, complete = 'command', bar = true, range = true })

-- Color: Enable colorizer
vim.api.nvim_create_user_command('Color', function()
  vim.cmd('packadd nvim-colorizer.lua')
  local status, colorizer = pcall(require, 'colorizer')
  if status then
    colorizer.setup()
    vim.cmd('e')
  else
    print("Error: 'colorizer' module not found.")
  end
end, {})
