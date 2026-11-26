local home = os.getenv("HOME")

local function escape_pattern(str)
	return (str:gsub("[%(%)%.%%%+%-%*%?%[%]%^%$]", "%%%1"))
end

local home_pattern = home and escape_pattern(home)

-- Highlight StatusLine
vim.api.nvim_set_hl(0, "StatusLine", { bg = "#8787af", fg = "#262626" })

-- Autocmds for ModeChanged & DirChanged
local statusline_group = vim.api.nvim_create_augroup("StatusLine", { clear = true })

local cached_workdir = ""
local cached_branch = ""
local function update_workdir()
	local workdir = vim.fn.getcwd()
	if home_pattern then
		workdir = workdir:gsub(home_pattern, "~")
	end
	cached_workdir = workdir
	local branch = vim.fn.system("git rev-parse --abbrev-ref HEAD 2>/dev/null"):gsub("%s+$", "")
	cached_branch = (vim.v.shell_error == 0 and branch ~= "") and branch or ""
end
update_workdir() -- Initial run

vim.api.nvim_create_autocmd({ "DirChanged", "FocusGained" }, {
	group = statusline_group,
	callback = update_workdir,
})

vim.api.nvim_create_autocmd("ModeChanged", {
	group = statusline_group,
	pattern = "*:n*",
	callback = function()
		vim.api.nvim_set_hl(0, "StatusLine", { bg = "#8787af", fg = "#262626" })
	end,
})

vim.api.nvim_create_autocmd("ModeChanged", {
	group = statusline_group,
	pattern = "*:i",
	callback = function()
		vim.api.nvim_set_hl(0, "StatusLine", { bg = "#87af87", fg = "#262626" })
	end,
})

vim.api.nvim_create_autocmd("ModeChanged", {
	group = statusline_group,
	pattern = "*:R",
	callback = function()
		vim.api.nvim_set_hl(0, "StatusLine", { bg = "#af5f5f", fg = "#262626" })
	end,
})

-- StatusLine function
function _G.StatusLine()
	local branch_part = cached_branch ~= "" and (" (" .. cached_branch .. ")") or ""
	local stl = "[%n][" .. cached_workdir .. branch_part .. "] "

	local relpath = vim.fn.expand("%:h")
	if home_pattern then
		relpath = relpath:gsub(home_pattern, "~")
	end

	if relpath ~= "." and relpath ~= "" then
		stl = stl .. vim.fn.pathshorten(relpath) .. "/"
	end

	local diag_status = vim.diagnostic.status and vim.diagnostic.status() or ""
	local diag_part = (diag_status ~= "" and (" " .. diag_status .. " |") or "")
	stl = stl .. "%t%([%M%R%H%W]%)%=" .. diag_part .. " %P col %c %y"

	return stl
end

vim.opt.statusline = "%!v:lua.StatusLine()"
