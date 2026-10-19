vim.api.nvim_create_autocmd("LspAttach", {
	callback = function(args)
		local client = vim.lsp.get_client_by_id(args.data.client_id)
		local opts = {
			noremap = true,
			buffer = args.buf,
			silent = true,
		}

		vim.keymap.set(
			"n",
			"<C-Q>",
			vim.diagnostic.setqflist,
			{ noremap = true, buffer = args.buf, silent = true, desc = "Diagnostics to quickfix" }
		)


		if client.server_capabilities.completionProvider ~= nil then
			vim.lsp.completion.enable(true, client.id, args.buf, { autotrigger = true })
		end

		if client.server_capabilities.definitionProvider then
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

vim.api.nvim_create_autocmd("FileType", {
	desc = "Re-apply LSP buffer defaults (omnifunc, tagfunc, formatexpr) on reload",
	callback = function(args)
		for _, client in ipairs(vim.lsp.get_clients({ bufnr = args.buf })) do
			vim.lsp._set_defaults(client, args.buf)
		end
	end,
})

vim.keymap.set("", "<Leader>e", vim.diagnostic.open_float, { noremap = true, silent = true })

vim.diagnostic.config({
	underline = { [vim.diagnostic.severity.ERROR] = true },
	signs = false,
	severity_sort = true,
})
