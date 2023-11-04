vim.opt.formatoptions:append("taw")
vim.bo.makeprg = "latexmk"
vim.bo.errorformat = "%f:%l: %m,%-G%.%#"

local path = os.getenv("HOME") .. "/.local/share/nvim/mason/bin/"

vim.lsp.start({
	cmd = { path .. "texlab" },
	filetypes = { "tex", "plaintex", "bib" },
	single_file_support = true,
	settings = {
		texlab = {
			rootDirectory = nil,
			chktex = {
				onOpenAndSave = true,
				onEdit = false,
			},
			diagnosticsDelay = 100,
			latexFormatter = "latexindent",
			latexindent = {
				["local"] = nil, -- local is a reserved keyword
				modifyLineBreaks = false,
			},
			bibtexFormatter = "texlab",
			formatterLineLength = 80,
		},
	},
})

vim.lsp.start({
	name = "ltex",
	cmd = { path .. "ltex-ls" },
	filetypes = { "bib", "plaintex", "tex" },
	single_file_support = true,
	get_language_id = function(_, _)
		return "latex"
	end,
	on_attach = function(_, _)
		require("ltex_extra").setup({
			load_langs = { "en-US" },
			path = os.getenv("HOME") .. "/.dotfiles/nvim/spell",
		})
	end,
})
