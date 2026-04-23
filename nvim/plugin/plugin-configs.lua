require("Comment").setup()

vim.api.nvim_create_autocmd("TextYankPost", {
  group = "vimrc",
  callback = function()
    vim.highlight.on_yank({ higroup = "Visual", timeout = 200 })
  end,
})

require("aerial").setup({
  on_attach = function(bufnr)
    vim.keymap.set("n", "<F9>", "<CMD>AerialToggle!<CR>")
    vim.keymap.set("n", "<C-{>", "<cmd>AerialPrev<CR>", {buffer = bufnr})
    vim.keymap.set("n", "<C-}>", "<cmd>AerialNext<CR>", {buffer = bufnr})
  end
})

local remotefiles = require("remotefiles")

remotefiles.register({ "/placer/*", "/google_src/*" }, function(match)
	return "fileutil cat " .. match
end)

remotefiles.register("/cns/*", function(match)
	return "fileutil cat " .. match
end, function(match)
	return "fileutil tee -f -output " .. match
end)

remotefiles.register_local("//depot/google3/*", function(match)
	local client_path = os.getenv("PWD"):gsub("google3/?.*", "google3/")
	local rel_path = match:sub(1 + #"//depot/google3/")
	return client_path .. rel_path
end)

remotefiles.register_local("*:*:*", function(match)
	return vim.split(match, ":")[1]
end, function(match)
	local _, line, col = unpack(vim.split(match, ":"))
	vim.cmd(line)
	if col ~= nil then
		vim.cmd("normal " .. col .. "|")
	end
end)
