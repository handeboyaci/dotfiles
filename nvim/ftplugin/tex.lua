vim.opt.formatoptions:append("taw")
vim.bo.makeprg = "latexmk -pdflua -interaction=nonstopmode -file-line-error -synctex=1 %:S"
vim.bo.errorformat = "%f:%l: %m,%-G%.%#"

vim.lsp.start({
	name = "texlab",
	cmd = { "texlab" },
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
