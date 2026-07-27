vim.opt.formatoptions:append("taw")
vim.bo.makeprg = "latexmk -pdflua -interaction=nonstopmode -file-line-error -synctex=1 %:S"
vim.bo.errorformat = "%f:%l: %m,%-G%.%#"

local path = vim.fn.stdpath("data") .. "/mason/bin/"
vim.env.PATH = path .. ":" .. vim.env.PATH

vim.lsp.start({
	cmd = { path .. "texlab" },
	filetypes = { "tex", "plaintex", "bib" },
	single_file_support = true,
	settings = {
		texlab = {
			build = {
				executable = "latexmk",
				args = { "-pdflua", "-interaction=nonstopmode", "-file-line-error", "-synctex=1", "%f" },
				onSave = true,
			},
			chktex = {
				onOpenAndSave = true,
				onEdit = false,
			},
			diagnosticsDelay = 100,
			latexFormatter = "tex-fmt",
			bibtexFormatter = "texlab",
			formatterLineLength = 80,
		},
	},
})
