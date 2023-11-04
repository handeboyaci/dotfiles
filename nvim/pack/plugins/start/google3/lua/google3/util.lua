local M = {}

M.joinpath = function(...)
	return table.concat({ ... }, "/")
end

return M
