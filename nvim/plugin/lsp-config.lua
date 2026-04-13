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

		vim.diagnostic.config({
			underline = { [vim.diagnostic.severity.ERROR] = true },
			signs = false,
			severity_sort = true,
		})

		vim.keymap.set("n", "]e", vim.diagnostic.goto_next, { noremap = true, silent = true })
		vim.keymap.set("n", "[e", vim.diagnostic.goto_prev, { noremap = true, silent = true })
		vim.keymap.set("n", "<C-Q>", vim.diagnostic.setqflist, opts)


    if client.server_capabilities.documentFormattingProvider then
      vim.keymap.set("n", "gQ", vim.lsp.buf.format, opts)
    end
		if client.server_capabilities.documentRangeFormattingProvider then
			vim.opt_local.formatexpr = "v:lua.vim.lsp.formatexpr()"
		end

		if client.server_capabilities.hoverProvider then
			vim.keymap.set("n", "K", vim.lsp.buf.hover, opts)
		end

		if client.server_capabilities.completionProvider ~= nil then
			vim.opt_local.omnifunc = "v:lua.vim.lsp.omnifunc"
		end

		if client.server_capabilities.definitionProvider then
			vim.opt_local.tagfunc = "v:lua.vim.lsp.tagfunc"
      vim.keymap.set("n", "gd", vim.lsp.buf.definition, opts)
      vim.keymap.set("n", "gD", vim.lsp.buf.type_definition, opts)
			vim.keymap.set("n", "<F2>", vim.lsp.buf.rename, opts)
		end

		if client.server_capabilities.codeActionProvider then
			vim.keymap.set("n", "<Leader>a", vim.lsp.buf.code_action, opts)
		end

		if client.server_capabilities.referencesProvider then
			vim.keymap.set("n", "gR", vim.lsp.buf.references, opts)
		end

		if client.server_capabilities.signatureHelpProvider ~= nil then
			vim.keymap.set("i", "<C-Q>", vim.lsp.buf.signature_help, opts)
		end
	end,
})

vim.keymap.set("", "<Leader>e", vim.diagnostic.open_float, { noremap = true, silent = true })
