function LspRoot(...)
	return vim.fs.dirname(vim.fs.find({ ... }, {
		upward = true,
		path = vim.fn.expand("%:p:h"),
	})[1])
end

function LspStatus()
	if next(vim.lsp.get_clients()) == nil then
		return ""
	end

	local data = {
		[vim.diagnostic.severity.ERROR] = 0,
		[vim.diagnostic.severity.WARN] = 0,
		[vim.diagnostic.severity.INFO] = 0,
		[vim.diagnostic.severity.HINT] = 0,
	}

	for _, elem in ipairs(vim.diagnostic.get(0)) do
		data[elem.severity] = data[elem.severity] + 1
	end

	local out = {}

	local v = data[vim.diagnostic.severity.HINT]
	if v > 0 then
		table.insert(out, v .. "H")
	end

	v = data[vim.diagnostic.severity.INFO]
	if v > 0 then
		table.insert(out, v .. "I")
	end

	v = data[vim.diagnostic.severity.WARN]
	if v > 0 then
		table.insert(out, v .. "W")
	end

	v = data[vim.diagnostic.severity.ERROR]
	if v > 0 then
		table.insert(out, v .. "E")
	end

	if next(out) then
		return table.concat(out)
	end
	return ""
end

vim.api.nvim_create_autocmd("LspAttach", {
	callback = function(args)
	local client = vim.lsp.get_client_by_id(args.data.client_id)
	local opts = {
		noremap = true,
		buffer = args.buf,
		silent = true,
	}

	vim.keymap.set("n", "]e", function()
		vim.diagnostic.jump({ count = 1 })
	end, { noremap = true, silent = true, desc = "Next diagnostic" })
	vim.keymap.set("n", "[e", function()
		vim.diagnostic.jump({ count = -1 })
	end, { noremap = true, silent = true, desc = "Previous diagnostic" })
	vim.keymap.set(
		"n",
		"<C-Q>",
		vim.diagnostic.setqflist,
		{ noremap = true, buffer = args.buf, silent = true, desc = "Diagnostics to quickfix" }
	)

	if client.server_capabilities.documentFormattingProvider then
		vim.keymap.set(
			"n",
			"gQ",
			vim.lsp.buf.format,
			{ noremap = true, buffer = args.buf, silent = true, desc = "Format buffer" }
		)
	end
	if client.server_capabilities.documentRangeFormattingProvider then
		vim.bo[args.buf].formatexpr = "v:lua.vim.lsp.formatexpr()"
	end

	if client.server_capabilities.hoverProvider then
		vim.keymap.set(
			"n",
			"K",
			vim.lsp.buf.hover,
			{ noremap = true, buffer = args.buf, silent = true, desc = "LSP Hover" }
		)
	end

	if client.server_capabilities.completionProvider ~= nil then
		vim.bo[args.buf].omnifunc = "v:lua.vim.lsp.omnifunc"
	end

	if client.server_capabilities.definitionProvider then
		vim.bo[args.buf].tagfunc = "v:lua.vim.lsp.tagfunc"
		vim.keymap.set(
			"n",
			"gd",
			vim.lsp.buf.definition,
			{ noremap = true, buffer = args.buf, silent = true, desc = "Go to definition" }
		)
		vim.keymap.set(
			"n",
			"gD",
			vim.lsp.buf.type_definition,
			{ noremap = true, buffer = args.buf, silent = true, desc = "Go to type definition" }
		)
		vim.keymap.set(
			"n",
			"<F2>",
			vim.lsp.buf.rename,
			{ noremap = true, buffer = args.buf, silent = true, desc = "Rename symbol" }
		)
	end

	if client.server_capabilities.codeActionProvider then
		vim.keymap.set(
			"n",
			"<Leader>a",
			vim.lsp.buf.code_action,
			{ noremap = true, buffer = args.buf, silent = true, desc = "LSP Code Action" }
		)
	end

	if client.server_capabilities.referencesProvider then
		vim.keymap.set(
			"n",
			"gR",
			vim.lsp.buf.references,
			{ noremap = true, buffer = args.buf, silent = true, desc = "LSP References" }
		)
	end

	if client.server_capabilities.signatureHelpProvider ~= nil then
		vim.keymap.set(
			"i",
			"<C-Q>",
			vim.lsp.buf.signature_help,
			{ noremap = true, buffer = args.buf, silent = true, desc = "LSP Signature Help" }
		)
	end
	end,
})

vim.keymap.set("", "<Leader>e", vim.diagnostic.open_float, { noremap = true, silent = true })

vim.diagnostic.config({
	underline = { [vim.diagnostic.severity.ERROR] = true },
	signs = false,
	severity_sort = true,
})

for _, key in ipairs({ "grt", "gri", "gra", "grn" }) do
	pcall(vim.keymap.del, "n", key)
end
pcall(vim.keymap.del, "x", "gra")
