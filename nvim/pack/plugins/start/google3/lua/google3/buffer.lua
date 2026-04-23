local joinpath = require("google3.util").joinpath
local async = require("sencer.async")

local M = {}

local prefix = "/google/src/cloud"

---Asynchronously checks if a glob pattern matches any files.
---@param pattern string The glob pattern to check (e.g., "/path/*.swp").
---@param callback function A function to call with the result (boolean).
local function async_glob_exists(pattern, callback)
	-- We use `ls` to check for the file. It will have a non-zero exit code
	-- if the glob pattern matches no files. We discard stdout and stderr.
	vim.loop.spawn("ls", {
		args = { "-d", pattern }, -- -d prevents listing directory contents
		stdio = { nil, nil, nil }, -- Ignore stdin, stdout, and stderr
	}, function(code)
		-- The callback is scheduled to run on the main thread to safely call vim APIs.
		vim.schedule(function()
			-- If exit code is 0, a file was found.
			callback(code == 0)
		end)
	end)
end

local load_workspace = vim.schedule_wrap(function(j)
	vim.g.workspace_loaded = true
	local files_to_check = j:result() or {}
	local first = true
	for _, b in ipairs(files_to_check) do
		-- Skip comment lines from hg status
		if not b:match("^#") then
			local nm = vim.fn.fnamemodify(b, ":p:~:.")
			local swap_pattern = vim.fn.fnamemodify(nm, ":h") .. "/." .. vim.fn.fnamemodify(nm, ":t") .. ".sw*"
			-- Run our async check for each file.
			async_glob_exists(swap_pattern, function(swap_exists)
				if swap_exists then
					return
				end
				if first and vim.fn.bufname() == "" then
					first = false
					vim.cmd.edit(nm)
				else
					vim.cmd.badd(nm)
				end
			end)
		end
	end
end)


local function buf_var_setter(var)
	local setter = function(j, exit_code)
		if exit_code ~= 0 then
			print("Could not set " .. var)
			return
		end

		local _, val = next(j:result())
		vim.b[var] = val
	end
	return setter
end

local function run_shell_cmds(vcs)
  if vim.api.nvim_get_option_value("diff", {win=0}) then
    vim.g.workspace_loaded = true
  end
	local set_cl_job
	if vcs == "g4" then
		set_cl_job = async.run_shell({
			command = 'g4 -F "%change%" changes -s pending -c "$(g4 -F "%clientName" info|dos2unix)"',
			on_exit = buf_var_setter("citc_cl"),
		})
		if not vim.g.workspace_loaded then
			async
				.run_cmd({
					command = "g4 whatsout",
					on_exit = load_workspace,
				})
				:start()
		end
	elseif vcs == "hg" then
		set_cl_job = async.run_cmd({
			command = "hg exportedcl",
			on_exit = buf_var_setter("citc_cl"),
		})
		if not vim.g.workspace_loaded then
			async
				.run_shell({
					command = "hg pstatus -n | grep '/'",
					on_exit = load_workspace,
				})
				:start()
		end
	else
		return
	end

	local job = async.run_shell({
		command = "srcfs get_readonly",
		on_exit = buf_var_setter("citc_cl_synced"),
	})

	job:and_then_on_success(set_cl_job)

	job:start()
end

local function prepare_workspace(bufnr, fullpath)
	local components = vim.split(fullpath:sub(prefix:len() + 2), "/", { trimempty = true })

	if components[1] == nil then
		return
	end
	local b = vim.b[bufnr]
	b.citc_user = components[1]

	if components[2] == nil then
		return
	end
	b.citc_name = components[2]
	b.citc_root = joinpath(prefix, components[1], components[2], "google3")
	b.vcs = vim.fn.filereadable(joinpath(vim.b.citc_root, "../.citc/p4_client_name")) == 1 and "g4" or "hg"
	run_shell_cmds(b.vcs)

	vim.bo.path = vim.b.citc_root .. ",."

	-- component[3] is not interesting by itself, but it will be if there is another component after it.
	if components[4] == nil then
		return
	end

	local p
	if components[3] == "google3" then
		p = 4
	elseif components[3] == ".snapshot" or components[3] == ".citc" then
		p = 6
	else
		return
	end

	b.citc_path = joinpath(unpack(vim.list_slice(components, p)))
end

M.setup = function(args)
	local bufnr = args.buf or vim.fn.bufnr()

	if vim.b[bufnr].is_google3_file ~= nil then
		return
	end
	vim.b[bufnr].is_google3_file = true

	prepare_workspace(bufnr, args.match)

	if args.event == "BufNewFile" then
		vim.cmd("silent 0r !/usr/lib/autogen/autogen %")
	end
end

return M
