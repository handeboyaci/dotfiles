local lazy = require("lazy_loader")

lazy.lazy_load("telescope.nvim", {
	keys = {
		{
			"n",
			"<Leader>f",
			function()
				require("telescope.builtin").find_files()
			end,
		},
		{
			"n",
			"<Leader>b",
			function()
				require("telescope.builtin").buffers()
			end,
		},
		{
			"n",
			"<Leader>j",
			function()
				require("telescope.builtin").oldfiles()
			end,
		},
		{
			"n",
			"<Leader>F",
			function()
				require("telescope").extensions.file_browser.file_browser()
			end,
		},
	},
	on_load = function()
		local t = require("telescope")
		local actions = require("telescope.actions")

		t.setup({
			defaults = {
				initial_mode = "normal",
				mappings = {
					n = {
						["<Leader>q"] = actions.close,
						["<Ctrl>-v"] = actions.select_vertical,
					},
				},
			},
		})

		-- Load extension as well
		vim.cmd("packadd telescope-file-browser.nvim")
		t.load_extension("file_browser")
	end,
})
