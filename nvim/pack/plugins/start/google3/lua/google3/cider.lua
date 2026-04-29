M = {}

local function disable_others(bufnr)
	for _, other_client in ipairs(vim.lsp.get_clients({ bufnr = bufnr })) do
		if other_client.name ~= "Cider" then
			other_client.server_capabilities.documentFormattingProvider = false
			other_client.server_capabilities.documentRangeFormattingProvider = false
		end
	end
end

M.attach = function(citc_root, bufnr)
	local cmd = {
		"/google/bin/releases/cider/ciderlsp/ciderlsp",
		"--tooltag=neovim-lsp",
		"--cdpush_name=",
	}
	local proxy = os.getenv("PROD_PROXY")
	if proxy then
		table.insert(cmd, 1, proxy)
		table.insert(cmd, 1, "ssh")
	end

	-- Prioritize Cider formatting: disable others now and anyone who joins later.
	disable_others(bufnr)
	vim.api.nvim_create_autocmd("LspAttach", {
		group = vim.api.nvim_create_augroup("CiderFormatting_" .. bufnr, { clear = true }),
		buffer = bufnr,
		callback = function(event)
			local client = vim.lsp.get_client_by_id(event.data.client_id)
			if client and client.name ~= "Cider" then
				client.server_capabilities.documentFormattingProvider = false
				client.server_capabilities.documentRangeFormattingProvider = false
			elseif client and client.name == "Cider" then
				vim.keymap.set("n", "gQ", function()
					vim.lsp.buf.format({ timeout_ms = 5000 })
				end, { noremap = true, buffer = bufnr, silent = true, desc = "Format buffer (Cider)" })
				vim.bo[bufnr].formatexpr = "v:lua.vim.lsp.formatexpr(#{timeout_ms:5000})"
			end
		end,
	})

	vim.lsp.start({
		name = "Cider",
		cmd = cmd,
		root_dir = vim.fs.dirname(citc_root),
		-- offset_encoding = "utf-8",
		init_options = {},
		settings = {},
		handlers = {
			["$/statusNotification"] = function(_, result, ctx)
				if not result or not result.fileStatuses then
					return
				end
				for _, status in pairs(result.fileStatuses) do
					if status.statusItems then
						for _, item in ipairs(status.statusItems) do
							if item.text or item.detail then
								require("fidget").notify(item.detail or item.text, vim.log.levels.INFO, {
									key = item.id,
									group = "cider",
									annote = "Cider",
								})
							end
						end
					end
				end
			end,
		},
	})
end

return M
