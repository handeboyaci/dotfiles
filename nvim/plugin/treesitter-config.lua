require("nvim-treesitter.configs").setup({
	query_linter = {
		enable = true,
		use_virtual_text = true,
		lint_events = { "BufWrite", "CursorHold" },
	},
	playground = {
		enable = true,
		disable = {},
		updatetime = 25, -- Debounced time for highlighting nodes in the playground from source code
		persist_queries = false, -- Whether the query persists across vim sessions
		keybindings = {
			toggle_query_editor = "o",
			toggle_hl_groups = "i",
			toggle_injected_languages = "t",
			toggle_anonymous_nodes = "a",
			toggle_language_display = "I",
			focus_language = "f",
			unfocus_language = "F",
			update = "R",
			goto_node = "<cr>",
			show_help = "?",
		},
	},
	highlight = {
		enable = true,
		disable = { "help" },
		additional_vim_regex_highlighting = false,
	},
	indent = {
		enable = false,
	},
	incremental_selection = {
		enable = true,
		keymaps = {
			init_selection = "vin",
			node_incremental = "n",
			scope_incremental = "<Space>",
			node_decremental = "N",
		},
	},
	textobjects = {
		lsp_interop = {
			enable = true,
			border = "none",
			peek_definition_code = {
				["<leader>d"] = "@function.outer",
				["<leader>D"] = "@class.outer",
			},
		},
		select = {
			enable = true,
			keymaps = {
				-- You can use the capture groups defined in textobjects.scm
				["af"] = "@function.outer",
				["if"] = "@function.inner",
				["ac"] = "@class.outer",
				["ic"] = "@class.inner",
				["al"] = "@block.outer",
				["il"] = "@block.inner",
				["aa"] = "@parameter.outer",
				["ia"] = "@parameter.inner",
			},
		},
		swap = {
			enable = true,
			swap_next = {
				["]a"] = "@parameter.inner",
			},
			swap_previous = {
				["[a"] = "@parameter.inner",
			},
		},
		move = {
			enable = true,
			goto_next_start = {
				["]m"] = "@function.outer",
				["]]"] = "@class.outer",
			},
			goto_next_end = {
				["]M"] = "@function.outer",
				["]["] = "@class.outer",
			},
			goto_previous_start = {
				["[m"] = "@function.outer",
				["[["] = "@class.outer",
			},
			goto_previous_end = {
				["[M"] = "@function.outer",
				["[]"] = "@class.outer",
			},
		},
	},
	rainbow = {
		enable = true,
	},
})

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
