local async = require("sencer.async")
local vcs = require("vcs")

local M = {}

local prefix = "/google/src/cloud"

local function get_citc_root(dir)
	local components = vim.split(dir:sub(prefix:len() + 2), "/", { trimempty = true })
	if #components < 2 then
		return nil
	end
	return prefix .. "/" .. components[1] .. "/" .. components[2] .. "/google3"
end

-- G4 Provider
local g4 = {}
function g4.detect(dir)
	if not vim.startswith(dir, prefix) then
		return false
	end
	local citc_root = get_citc_root(dir)
	if not citc_root then
		return false
	end
	return vim.fn.filereadable(citc_root .. "/../.citc/p4_client_name") == 1
end

function g4.get_modified_files(dir, callback)
	async.run_cmd({
		command = "g4 whatsout",
		on_exit = function(obj)
			if obj.code ~= 0 then
				callback({})
				return
			end
			local files = {}
			for _, line in ipairs(vim.split(obj.stdout or "", "\n", { plain = true })) do
				if line ~= "" and not line:match("^#") then
					table.insert(files, line)
				end
			end
			callback(files)
		end,
	})
end

-- Hg CitC Provider
local hg_citc = {}
function hg_citc.detect(dir)
	if not vim.startswith(dir, prefix) then
		return false
	end
	local citc_root = get_citc_root(dir)
	if not citc_root then
		return false
	end
	return vim.fn.filereadable(citc_root .. "/../.citc/p4_client_name") == 0
end

function hg_citc.get_modified_files(dir, callback)
	async.run_shell({
		command = "hg pstatus -n | grep '/'",
		on_exit = function(obj)
			if obj.code ~= 0 then
				callback({})
				return
			end
			local files = {}
			for _, line in ipairs(vim.split(obj.stdout or "", "\n", { plain = true })) do
				if line ~= "" and not line:match("^#") then
					table.insert(files, line)
				end
			end
			callback(files)
		end,
	})
end

function M.setup()
	vcs.register("g4", g4)

	local hg = vcs.get_provider("hg")
	if hg then
		local original_detect = hg.detect
		local original_get_modified = hg.get_modified_files

		hg.detect = function(dir)
			return original_detect(dir) or hg_citc.detect(dir)
		end

		hg.get_modified_files = function(dir, callback)
			if hg_citc.detect(dir) then
				hg_citc.get_modified_files(dir, callback)
			else
				original_get_modified(dir, callback)
			end
		end
	else
		vcs.register("hg", hg_citc)
	end
end

return M
