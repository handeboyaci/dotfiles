M = {}

M.attach = function(citc_root, bufnr)
	local cmd = {
		"/google/bin/releases/cider/ciderlsp/ciderlsp",
		"--noforward_sync_responses",
		"--tooltag=neovim-lsp",
  }
	local proxy = os.getenv("PROD_PROXY")
	if proxy then
		table.insert(cmd, 1, proxy)
		table.insert(cmd, 1, "ssh")
	end
	vim.lsp.start({
		-- Attach a separate Cider process per file, as it keeps throwing errors otherwise.
		name = "Cider#" .. bufnr,
		cmd = cmd,
		root_dir = vim.fs.dirname(vim.fn.findfile("BUILD", ".;" .. citc_root)),
		on_attach = function(client, buf)
			-- Cider doesn't have formatting capabilities for python, yet it reports that it has. Make things right.
			if vim.bo[buf].filetype == "python" then
				client.server_capabilities.documentFormattingProvider = false
				client.server_capabilities.documentRangeFormattingProvider = false
			end
		end,
		settings = {
			ciderlsp = {
				hubAddress = "blade:languageservices",
				handleFilePattern = "**/google/src/cloud/*/*/google3/**",
			},
		},
	})
end

return M
