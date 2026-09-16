-- nvim-treesitter (main branch): manages parsers
require("nvim-treesitter").setup({
	install_dir = vim.fn.stdpath("data") .. "/site",
})

-- mini.ai: textobjects
local ai = require("mini.ai")
ai.setup({
	custom_textobjects = {
		f = ai.gen_spec.treesitter({ a = "@function.outer", i = "@function.inner" }),
		c = ai.gen_spec.treesitter({ a = "@class.outer", i = "@class.inner" }),
		a = ai.gen_spec.treesitter({ a = "@parameter.outer", i = "@parameter.inner" }),
		l = ai.gen_spec.treesitter({ a = "@block.outer", i = "@block.inner" }),
	},
})

-- nvim-treesitter-textobjects: motions and parameter swapping
local move = require("nvim-treesitter-textobjects.move")
local swap = require("nvim-treesitter-textobjects.swap")

-- Swap parameters
vim.keymap.set("n", "]a", function()
	swap.swap_next("@parameter.inner")
end, { desc = "Swap next parameter" })
vim.keymap.set("n", "[a", function()
	swap.swap_previous("@parameter.inner")
end, { desc = "Swap previous parameter" })

-- Move to function / class
vim.keymap.set({ "n", "x", "o" }, "]f", function()
	move.goto_next_start("@function.outer", "textobjects")
end, { desc = "Next function start" })
vim.keymap.set({ "n", "x", "o" }, "[f", function()
	move.goto_previous_start("@function.outer", "textobjects")
end, { desc = "Previous function start" })
vim.keymap.set({ "n", "x", "o" }, "]F", function()
	move.goto_next_end("@function.outer", "textobjects")
end, { desc = "Next function end" })
vim.keymap.set({ "n", "x", "o" }, "[F", function()
	move.goto_previous_end("@function.outer", "textobjects")
end, { desc = "Previous function end" })

vim.keymap.set({ "n", "x", "o" }, "]]", function()
	move.goto_next_start("@class.outer", "textobjects")
end, { desc = "Next class start" })
vim.keymap.set({ "n", "x", "o" }, "[[", function()
	move.goto_previous_start("@class.outer", "textobjects")
end, { desc = "Previous class start" })
vim.keymap.set({ "n", "x", "o" }, "][", function()
	move.goto_next_end("@class.outer", "textobjects")
end, { desc = "Next class end" })
vim.keymap.set({ "n", "x", "o" }, "[]", function()
	move.goto_previous_end("@class.outer", "textobjects")
end, { desc = "Previous class end" })

-- treesitter-context
require("treesitter-context").setup({
	patterns = {
		cpp = {
			"namespace",
			"struct",
			"enum",
		},
	},
})

vim.keymap.set("n", "[t", require("treesitter-context").go_to_context, { silent = true })
vim.keymap.set("n", "]t", require("treesitter-context").toggle, { silent = true })
