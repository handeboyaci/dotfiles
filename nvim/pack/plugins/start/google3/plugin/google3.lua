if vim.g.google3_config_loaded then
	return
end

vim.g.google3_config_loaded = true

-- Add extension to filetype mappings.
vim.filetype.add({
	extension = {
		gcl = "gcl",
		borg = "borg",
		dremel = "dremel",
		txtpb = "textpb",
		pbtxt = "textpb",
		pq = "pathquery",
	},
})

-- These are useful out of citc, too.
if vim.fn.bufname() == "" and vim.startswith(vim.fn.getcwd(), "/google/src/cloud/") then
	-- If an empty vim started in possible citc dir, set things up.
	require("google3").setup({
		buf = vim.fn.bufnr(),
		match = vim.fn.getcwd(),
	})
else
	-- Otherwise delay configuration until the first Google3 buffer is loaded.
	vim.api.nvim_create_autocmd({ "BufNewFile", "BufReadPre" }, {
		pattern = "/google/src/cloud/*",
		callback = require("google3").setup,
		once = true,
	})
end
