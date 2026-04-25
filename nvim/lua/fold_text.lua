local M = {}

function M.foldtext()
	local reg = "{{{" .. "\\d\\="
	local commentstring = vim.bo.commentstring
	for _, c in ipairs(vim.split(commentstring, "%s")) do
		reg = reg .. "\\|" .. vim.fn.escape(c, "*")
	end

	local fillchars = vim.o.fillchars
	local foldchar = vim.fn.matchstr(fillchars, "fold:\\zs.")
	if foldchar == "" then
		foldchar = " "
	end
	local wlength = math.min(vim.api.nvim_win_get_width(0), 80)
	local indent = math.max(vim.fn.indent(vim.v.foldstart), vim.v.foldlevel - 1)
	local nlines = (vim.v.foldend - vim.v.foldstart + 1) .. " lines "

	local ftext = vim.fn.getline(vim.v.foldstart)
	-- Use vim.fn.substitute to match Vim regex behavior accurately
	ftext = vim.fn.substitute(ftext, reg, "", "g")

	local result = string.rep(" ", indent)
		.. ftext:gsub("^%s*(.-)%s*$", "%1") -- Trim
		.. string.rep(foldchar, wlength - #nlines - #ftext - 2 - indent)
		.. " "
		.. nlines
		.. "≡"

	return result
end

return M
