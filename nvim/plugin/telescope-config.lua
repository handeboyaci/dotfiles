local t = require("telescope")
local actions = require("telescope.actions")
local builtin = require("telescope.builtin")

t.setup({
  defaults = {
    initial_mode = 'normal',
    mappings = {
      n = {
        ["<Leader>q"] = actions.close,
        ["<Ctrl>-v"] = actions.select_vertical,
      },
    },
  }
})

local opts = { noremap = true, silent = true }

vim.keymap.set("n", "<Leader>f", builtin.find_files, opts)
vim.keymap.set("n", "<Leader>b", builtin.buffers, opts)
vim.keymap.set("n", "<Leader>j", builtin.oldfiles, opts)

t.load_extension "file_browser"
vim.keymap.set("n", "<Leader>F", t.extensions.file_browser.file_browser, opts)
